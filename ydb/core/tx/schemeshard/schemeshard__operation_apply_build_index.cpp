#include "schemeshard__operation_common.h"
#include "schemeshard__operation_part.h"
#include "schemeshard_impl.h"
#include "schemeshard_path_element.h"
#include "schemeshard_utils.h"
#include "schemeshard_utils.h"  // for TransactionTemplate

#include <ydb/core/base/table_index.h>
#include <ydb/core/protos/flat_scheme_op.pb.h>
#include <ydb/core/protos/flat_tx_scheme.pb.h>

#include <yql/essentials/minikql/mkql_type_ops.h>

namespace NKikimr {
namespace NSchemeShard {
namespace {

ISubOperation::TPtr FinalizeIndexImplTable(TOperationContext& context, const TPath& index, const TOperationId& partId, const TString& name, const TPathId& pathId) {
    Y_ABORT_UNLESS(index.Child(name)->PathId == pathId);
    Y_ABORT_UNLESS(index.Child(name).LeafName() == name);
    TTableInfo::TPtr table = context.SS->Tables.at(pathId);
    auto transaction = TransactionTemplate(index.PathString(), NKikimrSchemeOp::EOperationType::ESchemeOpFinalizeBuildIndexImplTable);
    auto operation = transaction.MutableAlterTable();
    operation->SetName(name);
    operation->MutablePartitionConfig()->MutableCompactionPolicy()->CopyFrom(table->PartitionConfig().GetCompactionPolicy());
    operation->MutablePartitionConfig()->MutableCompactionPolicy()->SetKeepEraseMarkers(false);
    operation->MutablePartitionConfig()->SetShadowData(false);
    return CreateFinalizeBuildIndexImplTable(partId, transaction);
}

ISubOperation::TPtr DropIndexImplTable(const TPath& index, const TOperationId& nextId, const TOperationId& partId, const TString& name, const TPathId& pathId, const NKikimrSchemeOp::TLockGuard& lockGuard, bool& rejected) {
    TPath implTable = index.Child(name);
    Y_ABORT_UNLESS(implTable->PathId == pathId);
    Y_ABORT_UNLESS(implTable.LeafName() == name);
    auto checks = implTable.Check();
    checks.NotEmpty()
        .IsResolved()
        .NotDeleted()
        .IsTable()
        .IsInsideTableIndexPath()
        .NotUnderDeleting()
        .NotUnderOperation();
    if (!checks) {
        rejected = true;
        return CreateReject(nextId, checks.GetStatus(), checks.GetError());
    }
    rejected = false;
    auto transaction = TransactionTemplate(index.PathString(), NKikimrSchemeOp::EOperationType::ESchemeOpDropTable);
    if (implTable.IsLocked()) {
        // because some impl tables may be not locked, do not pass lock guard for them
        // otherwise `CheckLocks` check would fail
        *transaction.MutableLockGuard() = lockGuard;
    }
    auto operation = transaction.MutableDrop();
    operation->SetName(name);
    return CreateDropTable(partId, transaction);
}

}

TVector<ISubOperation::TPtr> ApplyBuildIndex(TOperationId nextId, const TTxTransaction& tx, TOperationContext& context) {
    Y_ABORT_UNLESS(tx.GetOperationType() == NKikimrSchemeOp::EOperationType::ESchemeOpApplyIndexBuild);

    auto config = tx.GetApplyIndexBuild();
    TString tablePath = config.GetTablePath();
    TString indexName = config.GetIndexName();

    TPath table = TPath::Resolve(tablePath, context.SS);
    TVector<ISubOperation::TPtr> result;
    {
        auto finalize = TransactionTemplate(table.Parent().PathString(), NKikimrSchemeOp::EOperationType::ESchemeOpFinalizeBuildIndexMainTable);
        *finalize.MutableLockGuard() = tx.GetLockGuard();
        auto op = finalize.MutableFinalizeBuildIndexMainTable();
        op->SetTableName(table.LeafName());
        op->SetSnapshotTxId(config.GetSnapshotTxId());
        op->SetBuildIndexId(config.GetBuildIndexId());
        if (!indexName.empty()) {
            TPath index = table.Child(indexName);
            index.Base()->PathId.ToProto(op->MutableOutcome()->MutableApply()->MutableIndexPathId());
        }

        result.push_back(CreateFinalizeBuildIndexMainTable(NextPartId(nextId, result), finalize));
    }

    if (!indexName.empty()) {
        TPath index = table.Child(indexName);
        auto tableIndexAltering = TransactionTemplate(table.PathString(), NKikimrSchemeOp::EOperationType::ESchemeOpAlterTableIndex);
        *tableIndexAltering.MutableLockGuard() = tx.GetLockGuard();
        auto alterIndex = tableIndexAltering.MutableAlterTableIndex();
        alterIndex->SetName(index.LeafName());
        alterIndex->SetState(NKikimrSchemeOp::EIndexState::EIndexStateReady);

        result.push_back(CreateAlterTableIndex(NextPartId(nextId, result), tableIndexAltering));
    }

    if (!indexName.empty()) {
        TPath index = table.Child(indexName);
        Y_ABORT_UNLESS(index.Base()->GetChildren().size() >= 1);
        for (auto& indexChildItems : index.Base()->GetChildren()) {
            const auto& indexImplTableName = indexChildItems.first;
            const auto partId = NextPartId(nextId, result);
            if (NTableIndex::IsBuildImplTable(indexImplTableName)) {
                bool rejected = false;
                auto op = DropIndexImplTable(index, nextId, partId, indexImplTableName, indexChildItems.second, tx.GetLockGuard(), rejected);
                if (rejected) {
                    return {std::move(op)};
                }
                result.push_back(std::move(op));
            } else {
                result.push_back(FinalizeIndexImplTable(context, index, partId, indexImplTableName, indexChildItems.second));
            }
        }
    }

    return result;
}

TVector<ISubOperation::TPtr> CancelBuildIndex(TOperationId nextId, const TTxTransaction& tx, TOperationContext& context) {
    Y_ABORT_UNLESS(tx.GetOperationType() == NKikimrSchemeOp::EOperationType::ESchemeOpCancelIndexBuild);

    auto config = tx.GetCancelIndexBuild();
    TString tablePath = config.GetTablePath();
    TString indexName = config.GetIndexName();

    TPath table = TPath::Resolve(tablePath, context.SS);

    TVector<ISubOperation::TPtr> result;
    {
        auto finalize = TransactionTemplate(table.Parent().PathString(), NKikimrSchemeOp::EOperationType::ESchemeOpFinalizeBuildIndexMainTable);
        *finalize.MutableLockGuard() = tx.GetLockGuard();
        auto op = finalize.MutableFinalizeBuildIndexMainTable();
        op->SetTableName(table.LeafName());
        op->SetSnapshotTxId(config.GetSnapshotTxId());
        op->SetBuildIndexId(config.GetBuildIndexId());

        if (!indexName.empty()) {
            TPath index = table.Child(indexName);
            index.Base()->PathId.ToProto(op->MutableOutcome()->MutableCancel()->MutableIndexPathId());
        }

        result.push_back(CreateFinalizeBuildIndexMainTable(NextPartId(nextId, result), finalize));
    }

    if (!indexName.empty()) {
        TPath index = table.Child(indexName);
        auto tableIndexDropping = TransactionTemplate(table.PathString(), NKikimrSchemeOp::EOperationType::ESchemeOpDropTableIndex);
        auto operation = tableIndexDropping.MutableDrop();
        operation->SetName(index.Base()->Name);

        result.push_back(CreateDropTableIndex(NextPartId(nextId, result), tableIndexDropping));
    }

    if (!indexName.empty()) {
        TPath index = table.Child(indexName);
        Y_ABORT_UNLESS(index.Base()->GetChildren().size() >= 1);
        for (auto& indexChildItems : index.Base()->GetChildren()) {
            const auto partId = NextPartId(nextId, result);
            bool rejected = false;
            auto op = DropIndexImplTable(index, nextId, partId, indexChildItems.first, indexChildItems.second, tx.GetLockGuard(), rejected);
            if (rejected) {
                return {std::move(op)};
            }
            result.push_back(std::move(op));
        }
    }

    return result;
}

}
}
