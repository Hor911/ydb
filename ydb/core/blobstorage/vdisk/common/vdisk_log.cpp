#include "vdisk_log.h"
#include "vdisk_context.h"
#include <ydb/library/actors/protos/services_common.pb.h>

namespace  NKikimr {

    ////////////////////////////////////////////////////////////////////////////
    // Log Prefix
    ////////////////////////////////////////////////////////////////////////////
    TString AppendVDiskLogPrefix(const TIntrusivePtr<TVDiskContext> &vctx, const char *c, ...) {
        TString formatted;
        va_list params;
        va_start(params, c);
        vsprintf(formatted, c, params);
        va_end(params);
        return vctx->VDiskLogPrefix + formatted;
    }

    TString AppendVDiskLogPrefix(const TString &prefix, const char *c, ...) {
        TString formatted;
        va_list params;
        va_start(params, c);
        vsprintf(formatted, c, params);
        va_end(params);
        return prefix + formatted;
    }

    TString GenerateVDiskMonitoringName(const TVDiskID &vDiskId, bool donorMode) {
        return donorMode
            ? Sprintf("VDISK%s(DONOR): ", vDiskId.ToString().data())
            : Sprintf("VDISK%s: ", vDiskId.ToStringWOGeneration().data());
    }

    TString GenerateVDiskLogPrefix(ui32 pDiskId, const TVDiskID &vDiskId, bool donorMode) {
        auto monName = GenerateVDiskMonitoringName(vDiskId, donorMode);
        return Sprintf("PDiskId# %" PRIu32 " %s(%" PRIu32 ") ",
                pDiskId, monName.data(), vDiskId.GroupID.GetRawId());
    }

    ////////////////////////////////////////////////////////////////////////////
    // TFakeLoggerCtx -- fake implementation of ILoggerCtx
    ////////////////////////////////////////////////////////////////////////////
    TFakeLoggerCtx::TFakeLoggerCtx()
        : Settings({}, NActorsServices::LOGGER, NActors::NLog::PRI_ERROR, NActors::NLog::PRI_DEBUG, 0)
    {
        Settings.Append(
            NActorsServices::EServiceCommon_MIN,
            NActorsServices::EServiceCommon_MAX,
            NActorsServices::EServiceCommon_Name
        );
        Settings.Append(
            NKikimrServices::EServiceKikimr_MIN,
            NKikimrServices::EServiceKikimr_MAX,
            NKikimrServices::EServiceKikimr_Name
        );
    }


} // NKikimr
