#include "flat_executor_txloglogic.h"
#include "flat_executor_counters.h"
#include "flat_exec_seat.h"
#include "flat_exec_commit_mgr.h"
#include "flat_bio_eggs.h"
#include "logic_redo_batch.h"
#include "logic_redo_entry.h"
#include "logic_redo_queue.h"
#include "probes.h"
#include "util_fmt_abort.h"
#include "util_string.h"
#include <ydb/core/tablet_flat/flat_executor.pb.h>
#include <util/system/sanitizers.h>

namespace NKikimr {
namespace NTabletFlatExecutor {

LWTRACE_USING(TABLET_FLAT_PROVIDER)

const static ui64 MaxSizeToEmbedInLog = 2048;
const static ui64 MaxBytesToBatch = 2 * 1024 * 1024;
const static ui64 MaxItemsToBatch = 64;

TLogicRedo::TCompletionEntry::TCompletionEntry(std::unique_ptr<TSeat> seat, ui32 step)
    : Step(step)
{
    Transactions.PushBack(seat.release());
}

TLogicRedo::TLogicRedo(TAutoPtr<NPageCollection::TSteppedCookieAllocator> cookies, TCommitManager *commitManager, TAutoPtr<NRedo::TQueue> queue)
    : CommitManager(commitManager)
    , Cookies(cookies)
    , Batch(new NRedo::TBatch)
    , Queue(queue)
    , Slicer(1, Cookies.Get(), NBlockIO::BlockSize)
{}

TLogicRedo::~TLogicRedo()
{}

void TLogicRedo::Describe(IOutputStream &out) const
{
    return Queue->Describe(out);
}

void TLogicRedo::InstallCounters(TExecutorCounters *counters, TTabletCountersWithTxTypes *appTxCounters) {
    Counters = counters;
    AppTxCounters = appTxCounters;
}

NRedo::TStats TLogicRedo::LogStats() const
{
    return { Queue->Items, Queue->Memory, Queue->LargeGlobIdsBytes };
}

TArrayRef<const NRedo::TUsage> TLogicRedo::GrabLogUsage() const
{
    return Queue->GrabUsage();
}

void CompleteRoTransaction(std::unique_ptr<TSeat> seat, const TActorContext &ownerCtx, TExecutorCounters *counters, TTabletCountersWithTxTypes *appTxCounters) {
    const TTxType txType = seat->TxType;

    const ui64 latencyus = ui64(1000000. * seat->LatencyTimer.Passed());
    counters->Percentile()[TExecutorCounters::TX_PERCENTILE_LATENCY_RO].IncrementFor(latencyus);

    THPTimer completeTimer;
    LWTRACK(TransactionCompleteBegin, seat->Self->Orbit, seat->UniqID);
    seat->Complete(ownerCtx, false);
    LWTRACK(TransactionCompleteEnd, seat->Self->Orbit, seat->UniqID);

    const ui64 completeTimeus = ui64(1000000. * completeTimer.Passed());

    if (Y_UNLIKELY(seat->IsTerminated())) {
        counters->Cumulative()[TExecutorCounters::TX_TERMINATED].Increment(1);
        if (appTxCounters && txType != UnknownTxType) {
            appTxCounters->TxSimple(txType, COUNTER_TT_INFLY) -= 1;
            appTxCounters->TxCumulative(txType, COUNTER_TT_TERMINATED).Increment(1);
        }
    } else {
        counters->Cumulative()[TExecutorCounters::TX_RO_COMPLETED].Increment(1);
        if (appTxCounters && txType != UnknownTxType) {
            appTxCounters->TxSimple(txType, COUNTER_TT_INFLY) -= 1;
            appTxCounters->TxCumulative(txType, COUNTER_TT_RO_COMPLETED).Increment(1);
        }
    }

    counters->Percentile()[TExecutorCounters::TX_PERCENTILE_COMMITED_CPUTIME].IncrementFor(completeTimeus);
    counters->Cumulative()[TExecutorCounters::CONSUMED_CPU].Increment(completeTimeus);
    if (appTxCounters && txType != UnknownTxType)
        appTxCounters->TxCumulative(txType, COUNTER_TT_COMMITED_CPUTIME).Increment(completeTimeus);
}

bool TLogicRedo::CommitROTransaction(std::unique_ptr<TSeat> seat, const TActorContext &ownerCtx) {
    if (CompletionQueue.empty()) {
        CompleteRoTransaction(std::move(seat), ownerCtx, Counters, AppTxCounters);
        return true;
    } else {
        LWTRACK(TransactionReadOnlyWait, seat->Self->Orbit, seat->UniqID, CompletionQueue.back().Step);
        CompletionQueue.back().Transactions.PushBack(seat.release());
        return false;
    }
}

void TLogicRedo::FlushBatchedLog()
{
    if (TAutoPtr<TLogCommit> commit = Batch->Commit) {
        if (commit->TraceId) {
            i64 batchSize = Batch->Bodies.size();

            for (TSeat* curSeat = commit->FirstTx; curSeat != nullptr; curSeat = curSeat->NextCommitTx) {
                // Update batch size of the transaction, whose TraceId the commit uses (first transaction in batch, that has TraceId).
                if (curSeat->Self->TxSpan) {
                    curSeat->Self->TxSpan.Attribute("BatchSize", batchSize);
                    break;
                }
            }
        }

        auto affects = Batch->Affects();
        MakeLogEntry(*commit, Batch->Flush(), affects, true);
        CommitManager->Commit(commit);
    }

    Y_ENSURE(Batch->Commit == nullptr, "Batch still has acquired commit");
}

TLogicRedo::TCommitRWTransactionResult TLogicRedo::CommitRWTransaction(
                std::unique_ptr<TSeat> seat, NTable::TChange &change, bool force)
{
    seat->CommitTimer.Reset();

    Y_ENSURE(force || !(change.Scheme || change.Annex));

    const TTxType txType = seat->TxType;

    if (auto bytes = change.Redo.size()) {
        Counters->Cumulative()[TMonCo::DB_REDO_WRITTEN_BYTES].Increment(bytes);
        if (AppTxCounters && txType != UnknownTxType)
            AppTxCounters->TxCumulative(txType, COUNTER_TT_REDO_WRITTEN_BYTES).Increment(bytes);
    }

    if (change.Annex) {
        ui64 bytes = 0;
        for (const auto &one : change.Annex) {
            bytes += one.Data.size();
        }

        Counters->Cumulative()[TMonCo::DB_ANNEX_ITEMS_GROW].Increment(change.Annex.size());
        Counters->Cumulative()[TMonCo::DB_ANNEX_WRITTEN_BYTES].Increment(bytes);
        if (AppTxCounters && txType != UnknownTxType)
            AppTxCounters->TxCumulative(txType, COUNTER_TT_ANNEX_WRITTEN_BYTES).Increment(bytes);
    }

    if (force || MaxItemsToBatch < 2 || change.Redo.size() > MaxBytesToBatch) {
        FlushBatchedLog();

        auto commit = CommitManager->Begin(true, ECommit::Redo, seat->GetTxTraceId());

        commit->PushTx(seat.get());
        CompletionQueue.push_back({ std::move(seat), commit->Step });
        MakeLogEntry(*commit, std::move(change.Redo), change.Affects, !force);

        const auto was = commit->GcDelta.Created.size();

        for (auto &one: change.Annex) {
            if (one.GId.Logo.Step() != commit->Step) {
                Y_TABLET_ERROR(
                    "Leader{" << Cookies->Tablet << ":" << Cookies->Gen << "}"
                    << " got for " << NFmt::Do(*commit) << " annex blob "
                    << one.GId.Logo << " out of step order");
            }

            commit->GcDelta.Created.emplace_back(one.GId.Logo);
            auto& ref = commit->Refs.emplace_back(one.GId.Logo, one.Data.ToString());

            // External blobs are never recompacted.
            // Prioritize small number of copies over latency.
            ref.Tactic = TEvBlobStorage::TEvPut::ETactic::TacticMaxThroughput;
        }

        Y_ENSURE(was + change.Annex.size() == commit->GcDelta.Created.size());

        return { commit, false };
    } else {
        if (Batch->Bytes + change.Redo.size() > MaxBytesToBatch)
            FlushBatchedLog();

        if (!Batch->Commit) {
            Batch->Commit = CommitManager->Begin(false, ECommit::Redo, seat->GetTxTraceId());
        } else {
            const TAutoPtr<ITransaction> &tx = seat->Self;
            // Batch commit's TraceId will be used for all blobstorage requests of the batch.
            if (!Batch->Commit->TraceId && tx->TxSpan) {
                // It is possible that the original or consequent transactions didn't have a TraceId,
                // but if a new transaction of a batch has TraceId, use it for the whole batch
                // (and consequent traced transactions).
                Batch->Commit->TraceId = seat->GetTxTraceId();
            } else if (Batch->Commit->TraceId) {
                tx->TxSpan.Link(Batch->Commit->TraceId, {});
            }
        }
        
        Batch->Commit->PushTx(seat.get());

        CompletionQueue.push_back({ std::move(seat), Batch->Commit->Step });

        Batch->Add(std::move(change.Redo), change.Affects);

        if (Batch->Bodies.size() >= MaxItemsToBatch)
            FlushBatchedLog();

        return { nullptr, bool(Batch->Commit) };
    }
}

void TLogicRedo::MakeLogEntry(TLogCommit &commit, TString redo, TArrayRef<const ui32> affects, bool embed)
{
    if (redo) {
        NSan::CheckMemIsInitialized(redo.data(), redo.size());

        Cookies->Switch(commit.Step, true /* require step switch */);

        auto coded = NPageCollection::TSlicer::Lz4()->Encode(redo);

        Counters->Cumulative()[TMonCo::LOG_REDO_WRITTEN].Increment(coded.size());

        if (embed && coded.size() <= MaxSizeToEmbedInLog) {
            // Note: Encode reserves MaxCompressedLength bytes
            NUtil::ShrinkToFit(coded);

            commit.Embedded = std::move(coded);
            Queue->Push({ Cookies->Gen, commit.Step }, affects, commit.Embedded);
        } else {
            auto largeGlobId = Slicer.Do(commit.Refs, std::move(coded), false);
            largeGlobId.MaterializeTo(commit.GcDelta.Created);

            Queue->Push({ Cookies->Gen, commit.Step }, affects, largeGlobId);
        }
    }
}

ui64 TLogicRedo::Confirm(ui32 step, const TActorContext &ctx, const TActorId &ownerId) {
    Y_ENSURE(!CompletionQueue.empty(),
        "tablet: " << Cookies->Tablet
        << " unexpected confirmation step: " << step
        << ", prev: " << PrevConfirmedStep);

    Y_ENSURE(CompletionQueue.front().Step == step,
        "tablet: " << Cookies->Tablet
        << " inconsistent confirmation step: " << step
        << ", queue head: " << CompletionQueue.front().Step
        << ", queue size: " << CompletionQueue.size()
        << ", prev confimed: " << PrevConfirmedStep);

    PrevConfirmedStep = step;

    const TActorContext ownerCtx = ctx.MakeFor(ownerId);
    ui64 confirmedTransactions = 0;
    do {
        TCompletionEntry &entry = CompletionQueue.front();

        Y_ENSURE(entry.Transactions,
            "tablet: " << Cookies->Tablet
            << " entry without transactions, step: " << step);

        std::unique_ptr<TSeat> seat{entry.Transactions.PopFront()};

        const TTxType txType = seat->TxType;
        const ui64 commitLatencyus = ui64(1000000. * seat->CommitTimer.Passed());
        const ui64 latencyus = ui64(1000000. * seat->LatencyTimer.Passed());
        Counters->Percentile()[TExecutorCounters::TX_PERCENTILE_LATENCY_COMMIT].IncrementFor(commitLatencyus);
        Counters->Percentile()[TExecutorCounters::TX_PERCENTILE_LATENCY_RW].IncrementFor(latencyus);

        ++confirmedTransactions;
        THPTimer completeTimer;
        LWTRACK(TransactionCompleteBegin, seat->Self->Orbit, seat->UniqID);
        seat->Complete(ownerCtx, true);
        LWTRACK(TransactionCompleteEnd, seat->Self->Orbit, seat->UniqID);

        const ui64 completeTimeus = ui64(1000000. * completeTimer.Passed());

        Counters->Cumulative()[TExecutorCounters::TX_RW_COMPLETED].Increment(1);
        if (AppTxCounters && txType != UnknownTxType) {
            AppTxCounters->TxSimple(txType, COUNTER_TT_INFLY) -= 1;
            AppTxCounters->TxCumulative(txType, COUNTER_TT_RW_COMPLETED).Increment(1);
        }
        Counters->Percentile()[TExecutorCounters::TX_PERCENTILE_COMMITED_CPUTIME].IncrementFor(completeTimeus);
        Counters->Cumulative()[TExecutorCounters::CONSUMED_CPU].Increment(completeTimeus);
        if (AppTxCounters && txType != UnknownTxType)
            AppTxCounters->TxCumulative(txType, COUNTER_TT_COMMITED_CPUTIME).Increment(completeTimeus);

        seat.reset();

        while (entry.Transactions) {
            std::unique_ptr<TSeat> seat{entry.Transactions.PopFront()};
            ++confirmedTransactions;
            CompleteRoTransaction(std::move(seat), ownerCtx, Counters, AppTxCounters);
        }

        CompletionQueue.pop_front();
    } while (!CompletionQueue.empty() && CompletionQueue.front().Step == step);

    return confirmedTransactions;
}

void TLogicRedo::SnapToLog(NKikimrExecutorFlat::TLogSnapshot &snap)
{
    Y_ENSURE(Batch->Commit == nullptr);

    Queue->Flush(snap);

    for (auto &xpair : Queue->Edges) {
        auto genstep = ExpandGenStepPair(xpair.second.TxStamp);
        auto *x = snap.AddTableSnapshoted();
        x->SetTable(xpair.first);
        x->SetGeneration(genstep.first);
        x->SetStep(genstep.second);
        x->SetHead(xpair.second.Head.ToProto());
    }
}

void TLogicRedo::CutLog(ui32 table, NTable::TSnapEdge edge, TGCBlobDelta &gc)
{
    Queue->Cut(table, edge, gc);
}

}}
