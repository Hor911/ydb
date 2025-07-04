#pragma once
#include "defs.h"
#include <ydb/core/base/blobstorage.h>
#include <ydb/core/blobstorage/vdisk/common/vdisk_pdiskctx.h>
#include <ydb/core/blobstorage/vdisk/syncer/blobstorage_syncer_localwriter.h>
#include <ydb/core/blobstorage/vdisk/anubis_osiris/blobstorage_anubis_osiris.h>
#include <ydb/core/control/lib/immediate_control_board_impl.h>

namespace NKikimr {

    ///////////////////////////////////////////////////////////////////////////////////////////////////
    // TEvKickEmergencyPutQueue
    ///////////////////////////////////////////////////////////////////////////////////////////////////
    class TEvKickEmergencyPutQueue : public TEventLocal<
        TEvKickEmergencyPutQueue,
        TEvBlobStorage::EvKickEmergencyPutQueue>
    {};

    ///////////////////////////////////////////////////////////////////////////////////////////////////
    // TEvWakeupEmergencyPutQueue
    ///////////////////////////////////////////////////////////////////////////////////////////////////
    class TEvWakeupEmergencyPutQueue : public TEventLocal<
                    TEvWakeupEmergencyPutQueue,
                    TEvBlobStorage::EvWakeupEmergencyPutQueue>
    {};


    ///////////////////////////////////////////////////////////////////////////////////////////////////
    // TEmergencyQueue -- forward declaration for a queue that is used to implement TOverloadHandler.
    // and other declarations
    ///////////////////////////////////////////////////////////////////////////////////////////////////
    class TEmergencyQueue;
    class THull;
    class TDynamicPDiskWeightsManager;
    class TPDiskCtx;
    class TThrottlingController;

    ///////////////////////////////////////////////////////////////////////////////////////////////////
    // Handlers for postponed events
    ///////////////////////////////////////////////////////////////////////////////////////////////////
    using TVMovedPatchHandler = std::function<void(const TActorContext &ctx,
            TEvBlobStorage::TEvVMovedPatch::TPtr ev)>;
    using TVPatchStartHandler = std::function<void(const TActorContext &ctx,
            TEvBlobStorage::TEvVPatchStart::TPtr ev)>;
    using TVPutHandler = std::function<void(const TActorContext &ctx,
            TEvBlobStorage::TEvVPut::TPtr ev)>;
    using TVMultiPutHandler = std::function<void(const TActorContext &ctx,
            TEvBlobStorage::TEvVMultiPut::TPtr ev)>;

    ///////////////////////////////////////////////////////////////////////////////////////////////////
    // TOverloadHandler is used to postpone or throttle events if message put rate is too high.
    // Message put rate can be very high, so that we don't have enough time to compact Hull database.
    // We provide two mechanisms to handle this problem:
    // 1. Dynamic PDisk Scheduler weights -- compaction weights are changed by the system based on
    //    compaction satisfaction rank
    // 2. Emergency queue -- we stop processing put messages until compaction if satisfaction rank
    //    becomes acceptable
    ///////////////////////////////////////////////////////////////////////////////////////////////////
    class TOverloadHandler {
    public:
        TOverloadHandler(
                const TIntrusivePtr<TVDiskConfig> &vcfg,
                const TIntrusivePtr<TVDiskContext> &vctx,
                const TPDiskCtxPtr &pdiskCtx,
                std::shared_ptr<THull> hull,
                NMonGroup::TSkeletonOverloadGroup &&mon,
                TVMovedPatchHandler &&vMovedPatch,
                TVPatchStartHandler &&vPatchStart,
                TVPutHandler &&vput,
                TVMultiPutHandler &&vMultiPut);
        ~TOverloadHandler();

        // call it when Hull database is changed (puts, etc) -- it recalculates ranks
        void ActualizeWeights(const TActorContext &ctx, ui32 mask, bool actualizeLevels = false);
        // call it when Hull database is changed after compaction -- it recalculates rank and starts
        // processing of postponed events; it returns true when we need to continue event processing
        // also it is called periodically and scheduled according to current throttling speed
        bool ProcessPostponedEvents(const TActorContext &ctx, int batchSize, bool actualizeLevels);

        // Postpone event in case of overload
        template<typename TEv>
        bool PostponeEvent(TAutoPtr<TEventHandle<TEv>> &ev);

        static void ToWhiteboard(const TOverloadHandler *this_, NKikimrWhiteboard::TVDiskSatisfactionRank &v);
        ui32 GetIntegralRankPercent() const;
        void Feedback(const NPDisk::TEvConfigureSchedulerResult &res, const TActorContext &ctx);
        void RenderHtml(IOutputStream &str);

        void OnKickEmergencyPutQueue();

        void SetLogChunkCount(ui32 logChunkCount);
        bool IsThrottling() const;
        ui32 GetThrottlingRate() const; // permille

    private:
        TIntrusivePtr<TVDiskContext> VCtx;
        std::shared_ptr<THull> Hull;
        NMonGroup::TSkeletonOverloadGroup Mon;
        std::unique_ptr<TEmergencyQueue> EmergencyQueue;
        std::shared_ptr<TDynamicPDiskWeightsManager> DynamicPDiskWeightsManager;
        std::unique_ptr<TThrottlingController> ThrottlingController;
        bool KickInFlight = false;
        ui32 LogChunkCount = 0;
    };

} // NKikimr
