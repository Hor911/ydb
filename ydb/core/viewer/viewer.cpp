#include "viewer.h"
#include "counters_hosts.h"
#include "viewer_healthcheck.h"
#include "json_handlers.h"
#include "log.h"
#include "viewer_request.h"
#include <library/cpp/mime/types/mime.h>
#include <library/cpp/monlib/service/pages/templates.h>
#include <library/cpp/protobuf/json/proto2json.h>
#include <util/generic/algorithm.h>
#include <util/stream/file.h>
#include <util/system/fstat.h>
#include <ydb/core/base/appdata.h>
#include <ydb/core/base/auth.h>
#include <ydb/core/base/path.h>
#include <ydb/core/base/statestorage.h>
#include <ydb/core/base/tablet_types.h>
#include <ydb/core/mon/mon.h>
#include <ydb/core/node_whiteboard/node_whiteboard.h>
#include <ydb/core/util/wildcard.h>
#include <ydb/library/aclib/aclib.h>
#include <ydb/library/actors/core/actor_bootstrapped.h>
#include <util/system/hostname.h>
#include <ydb/library/actors/core/mon.h>
#include <yql/essentials/public/issue/yql_issue_message.h>
#include <ydb/public/api/protos/ydb_monitoring.pb.h>

template<>
void Out<std::vector<ui32>>(IOutputStream& o, const std::vector<ui32>& v) {
    for (size_t i = 0; i < v.size(); ++i) {
        if (i > 0) {
            o << ',';
        }
        o << v[i];
    }
}

namespace NKikimr::NViewer {

using namespace NNodeWhiteboard;

extern void InitViewerJsonHandlers(TJsonHandlers& jsonHandlers);
extern void InitViewerBrowseJsonHandlers(TJsonHandlers& jsonHandlers);
extern void InitPDiskJsonHandlers(TJsonHandlers& jsonHandlers);
extern void InitVDiskJsonHandlers(TJsonHandlers& jsonHandlers);
extern void InitOperationJsonHandlers(TJsonHandlers& jsonHandlers);
extern void InitQueryJsonHandlers(TJsonHandlers& jsonHandlers);
extern void InitSchemeJsonHandlers(TJsonHandlers& jsonHandlers);
extern void InitStorageJsonHandlers(TJsonHandlers& jsonHandlers);

class TViewer : public TActorBootstrapped<TViewer>, public IViewer {
public:
    static constexpr NKikimrServices::TActivity::EType ActorActivityType() {
        return NKikimrServices::TActivity::TABLET_MONITORING_PROXY;
    }

    TViewer(const TKikimrRunConfig& kikimrRunConfig)
        : KikimrRunConfig(kikimrRunConfig)
    {
        CurrentMonitoringPort = KikimrRunConfig.AppConfig.GetMonitoringConfig().GetMonitoringPort();
        CurrentWorkerName = TStringBuilder() << FQDNHostName() << ":" << CurrentMonitoringPort;
    }

    void Bootstrap(const TActorContext& ctx) {
        Become(&TThis::StateWork);
        NActors::TMon* mon = AppData(ctx)->Mon;
        if (mon) {
            TVector<TString> databaseAllowedSIDs;
            TVector<TString> viewerAllowedSIDs;
            TVector<TString> monitoringAllowedSIDs;
            {
                const auto& protoAllowedSIDs = KikimrRunConfig.AppConfig.GetDomainsConfig().GetSecurityConfig().GetDatabaseAllowedSIDs();
                for (const auto& sid : protoAllowedSIDs) {
                    databaseAllowedSIDs.emplace_back(sid);
                }
            }
            {
                const auto& protoAllowedSIDs = KikimrRunConfig.AppConfig.GetDomainsConfig().GetSecurityConfig().GetViewerAllowedSIDs();
                for (const auto& sid : protoAllowedSIDs) {
                    databaseAllowedSIDs.emplace_back(sid);
                    viewerAllowedSIDs.emplace_back(sid);
                }
            }
            {
                const auto& protoAllowedSIDs = KikimrRunConfig.AppConfig.GetDomainsConfig().GetSecurityConfig().GetMonitoringAllowedSIDs();
                for (const auto& sid : protoAllowedSIDs) {
                    databaseAllowedSIDs.emplace_back(sid);
                    viewerAllowedSIDs.emplace_back(sid);
                    monitoringAllowedSIDs.emplace_back(sid);
                }
            }
            {
                const auto& protoAllowedSIDs = KikimrRunConfig.AppConfig.GetDomainsConfig().GetSecurityConfig().GetAdministrationAllowedSIDs();
                for (const auto& sid : protoAllowedSIDs) {
                    databaseAllowedSIDs.emplace_back(sid);
                    viewerAllowedSIDs.emplace_back(sid);
                    monitoringAllowedSIDs.emplace_back(sid);
                }
            }
            mon->RegisterActorPage({
                .RelPath = "viewer",
                .ActorSystem = ctx.ActorSystem(),
                .ActorId = ctx.SelfID,
                .UseAuth = true,
                .AllowedSIDs = databaseAllowedSIDs,
            });
            mon->RegisterActorPage({
                .RelPath = "viewer/capabilities",
                .ActorSystem = ctx.ActorSystem(),
                .ActorId = ctx.SelfID,
                .UseAuth = false,
            });
            mon->RegisterActorPage({
                .Title = "Viewer",
                .RelPath = "viewer/v2",
                .ActorSystem = ctx.ActorSystem(),
                .ActorId = ctx.SelfID,
            });
            mon->RegisterActorPage({
                .Title = "Monitoring",
                .RelPath = "monitoring",
                .ActorSystem = ctx.ActorSystem(),
                .ActorId = ctx.SelfID,
                .UseAuth = false,
            });
            mon->RegisterActorPage({
                .RelPath = "counters/hosts",
                .ActorSystem = ctx.ActorSystem(),
                .ActorId = ctx.SelfID,
                .UseAuth = false,
            });
            mon->RegisterActorPage({
                .RelPath = "healthcheck",
                .ActorSystem = ctx.ActorSystem(),
                .ActorId = ctx.SelfID,
                .UseAuth = false,
            });
            mon->RegisterActorPage({
                .RelPath = "vdisk",
                .ActorSystem = ctx.ActorSystem(),
                .ActorId = ctx.SelfID,
                .UseAuth = true,
                .AllowedSIDs = databaseAllowedSIDs,
            });
            mon->RegisterActorPage({
                .RelPath = "pdisk",
                .ActorSystem = ctx.ActorSystem(),
                .ActorId = ctx.SelfID,
                .UseAuth = true,
                .AllowedSIDs = monitoringAllowedSIDs,
            });
            mon->RegisterActorPage({
                .RelPath = "operation",
                .ActorSystem = ctx.ActorSystem(),
                .ActorId = ctx.SelfID,
                .UseAuth = true,
                .AllowedSIDs = databaseAllowedSIDs,
            });
            mon->RegisterActorPage({
                .RelPath = "query",
                .ActorSystem = ctx.ActorSystem(),
                .ActorId = ctx.SelfID,
                .UseAuth = true,
                .AllowedSIDs = databaseAllowedSIDs,
            });
            mon->RegisterActorPage({
                .RelPath = "scheme",
                .ActorSystem = ctx.ActorSystem(),
                .ActorId = ctx.SelfID,
                .UseAuth = true,
                .AllowedSIDs = databaseAllowedSIDs,
            });
            mon->RegisterActorPage({
                .RelPath = "storage",
                .ActorSystem = ctx.ActorSystem(),
                .ActorId = ctx.SelfID,
                .UseAuth = true,
                .AllowedSIDs = databaseAllowedSIDs,
            });
            if (!KikimrRunConfig.AppConfig.GetMonitoringConfig().GetHideHttpEndpoint()) {
                auto whiteboardServiceId = NNodeWhiteboard::MakeNodeWhiteboardServiceId(ctx.SelfID.NodeId());
                ctx.Send(whiteboardServiceId, new NNodeWhiteboard::TEvWhiteboard::TEvSystemStateAddEndpoint(
                    "http-mon", Sprintf(":%d", KikimrRunConfig.AppConfig.GetMonitoringConfig().GetMonitoringPort())));
            }

            AllowOrigin = KikimrRunConfig.AppConfig.GetMonitoringConfig().GetAllowOrigin();

            InitViewerJsonHandlers(JsonHandlers);
            InitPDiskJsonHandlers(JsonHandlers);
            InitVDiskJsonHandlers(JsonHandlers);
            InitStorageJsonHandlers(JsonHandlers);
            InitOperationJsonHandlers(JsonHandlers);
            InitQueryJsonHandlers(JsonHandlers);
            InitSchemeJsonHandlers(JsonHandlers);
            InitViewerBrowseJsonHandlers(JsonHandlers);

            for (const auto& handler : JsonHandlers.JsonHandlersList) {
                // temporary handling of old paths
                TStringBuf newPath(handler);
                TString oldPath = "/" + TString(newPath.After('/').Before('/')) + "/json/" + TString(newPath.After('/').After('/'));
                JsonHandlers.JsonHandlersIndex[oldPath] = JsonHandlers.JsonHandlersIndex[newPath];
            }

            // TODO: redirect of very old paths
            JsonHandlers.JsonHandlersIndex["/viewer/v2/json/config"] = JsonHandlers.JsonHandlersIndex["/viewer/config"];
            JsonHandlers.JsonHandlersIndex["/viewer/v2/json/sysinfo"] = JsonHandlers.JsonHandlersIndex["/viewer/sysinfo"];
            JsonHandlers.JsonHandlersIndex["/viewer/v2/json/pdiskinfo"] = JsonHandlers.JsonHandlersIndex["/viewer/pdiskinfo"];
            JsonHandlers.JsonHandlersIndex["/viewer/v2/json/vdiskinfo"] = JsonHandlers.JsonHandlersIndex["/viewer/vdiskinfo"];
            JsonHandlers.JsonHandlersIndex["/viewer/v2/json/storage"] = JsonHandlers.JsonHandlersIndex["/viewer/storage"];
            JsonHandlers.JsonHandlersIndex["/viewer/v2/json/nodelist"] = JsonHandlers.JsonHandlersIndex["/viewer/nodelist"];
            JsonHandlers.JsonHandlersIndex["/viewer/v2/json/tabletinfo"] = JsonHandlers.JsonHandlersIndex["/viewer/tabletinfo"];
            JsonHandlers.JsonHandlersIndex["/viewer/v2/json/nodeinfo"] = JsonHandlers.JsonHandlersIndex["/viewer/nodeinfo"];

            for (const auto& [name, handler] : JsonHandlers.JsonHandlersIndex) {
                // temporary handling of new handlers
                if (handler->IsHttpEvent()) {
                    mon->RegisterActorHandler({
                        .Path = name,
                        .Handler = ctx.SelfID,
                        .UseAuth = true,
                        .AllowedSIDs = databaseAllowedSIDs,
                    });
                }
            }
        }
        Schedule(TDuration::Seconds(10), new TEvents::TEvWakeup());
    }

    const TKikimrRunConfig& GetKikimrRunConfig() const override {
        return KikimrRunConfig;
    }

    void FillCORS(TStringBuilder& stream, const TRequestState& request);
    void FillTraceId(TStringBuilder& stream, const TRequestState& request);
    TString GetHTTPOK(const TRequestState& request, TString type, TString response, TInstant lastModified) override;
    TString GetChunkedHTTPOK(const TRequestState& request, TString contentType = {}) override;
    TString GetHTTPGATEWAYTIMEOUT(const TRequestState& request, TString type, TString response) override;
    TString GetHTTPBADREQUEST(const TRequestState& request, TString type, TString response) override;
    TString GetHTTPFORBIDDEN(const TRequestState& request, TString type, TString response) override;
    TString GetHTTPNOTFOUND(const TRequestState& request) override;
    TString GetHTTPINTERNALERROR(const TRequestState& request, TString contentType = {}, TString response = {}) override;
    TString GetHTTPFORWARD(const TRequestState& request, const TString& location, const TString& candidates) override;

    bool CheckAccessAdministration(const TRequestState& request) override {
        auto userTokenObject = request.GetUserTokenObject();
        if (!KikimrRunConfig.AppConfig.GetDomainsConfig().GetSecurityConfig().GetEnforceUserTokenRequirement()) {
            if (!KikimrRunConfig.AppConfig.GetDomainsConfig().GetSecurityConfig().GetEnforceUserTokenCheckRequirement() || userTokenObject.empty()) {
                return true;
            }
        }
        return IsTokenAllowed(userTokenObject, AppData()->DomainsConfig.GetSecurityConfig().GetAdministrationAllowedSIDs());
    }

    bool CheckAccessViewer(const TRequestState& request) override {
        auto userTokenObject = request.GetUserTokenObject();
        if (!KikimrRunConfig.AppConfig.GetDomainsConfig().GetSecurityConfig().GetEnforceUserTokenRequirement()) {
            if (!KikimrRunConfig.AppConfig.GetDomainsConfig().GetSecurityConfig().GetEnforceUserTokenCheckRequirement() || userTokenObject.empty()) {
                return true;
            }
        }
        return IsTokenAllowed(userTokenObject, AppData()->DomainsConfig.GetSecurityConfig().GetViewerAllowedSIDs())
            || IsTokenAllowed(userTokenObject, AppData()->DomainsConfig.GetSecurityConfig().GetMonitoringAllowedSIDs())
            || IsTokenAllowed(userTokenObject, AppData()->DomainsConfig.GetSecurityConfig().GetAdministrationAllowedSIDs());
    }

    static bool IsStaticGroup(ui32 groupId) {
        return groupId & 0x80000000 == 0;
    }

    TString GetGroupList(const auto& groups) {
        std::vector<ui32> groupIds;
        for (auto group : groups) {
            groupIds.push_back(group);
        }
        std::sort(groupIds.begin(), groupIds.end());
        TStringBuilder result;
        if (groups.empty()) {
            return result << "something";
        }
        result << "at least " << groups.size();
        if (groups.size() > 1) {
            result << " groups (";
        } else {
            result << " group (";
        }
        auto was_groups = 0;
        auto max_groups = 3;
        for (auto group : groupIds) {
            if (was_groups > 0) {
                result << ", ";
            }
            if (was_groups >= max_groups) {
                result << "...";
            }
            if (IsStaticGroup(group)) {
                result << "static ";
            }
            result << group;
            ++was_groups;
        }
        result << ")";
        return result;
    }

    void TranslateFromBSC2Human(const NKikimrBlobStorage::TConfigResponse& response, TString& bscError, bool& forceRetryPossible) override {
        forceRetryPossible = false;
        if (response.GroupsGetDisintegratedByExpectedStatusSize()) {
            bscError = TStringBuilder() << "Calling this operation could cause " << GetGroupList(response.GetGroupsGetDisintegratedByExpectedStatus()) << " to go into a dead state";
        } else if (response.GroupsGetDisintegratedSize()) {
            bscError = TStringBuilder() << "Calling this operation will cause " << GetGroupList(response.GetGroupsGetDisintegrated()) << " to go into a dead state";
        } else if (response.GroupsGetDegradedSize()) {
            bscError = TStringBuilder() << "Calling this operation will cause " << GetGroupList(response.GetGroupsGetDegraded()) << " to go into a degraded state";
            forceRetryPossible = true;
        } else if (response.StatusSize()) {
            const auto& lastStatus = response.GetStatus(response.StatusSize() - 1);
            TVector<ui32> groups;
            for (auto& failParam: lastStatus.GetFailParam()) {
                if (failParam.HasGroupId()) {
                    groups.emplace_back(failParam.GetGroupId());
                }
            }
            if (lastStatus.GetFailReason() == NKikimrBlobStorage::TConfigResponse::TStatus::kMayGetDegraded) {
                bscError = TStringBuilder() << "Calling this operation will cause " << GetGroupList(groups) << " to go into a degraded state";
                forceRetryPossible = true;
            } else if (lastStatus.GetFailReason() == NKikimrBlobStorage::TConfigResponse::TStatus::kMayLoseData) {
                bscError = TStringBuilder() << "Calling this operation may result in data loss for " << GetGroupList(groups);
            } else if (lastStatus.GetErrorDescription().find("failed to allocate group: no group options") != TString::npos) {
                bscError = "Failed to allocate group";
            }
        }
        if (bscError.empty()) {
            bscError = response.GetErrorDescription();
        }
    }

    static TString GetAddressWithoutPort(const TString& address) {
        auto pos = address.rfind(':');
        if (pos != TString::npos) {
            return address.substr(0, pos);
        }
        return address;
    }

    TString MakeForward(const TRequestState& request, const std::vector<ui32>& nodes) override {
        if (nodes.empty()) {
            return GetHTTPBADREQUEST(request, "text/plain", "Couldn't resolve database nodes");
        }
        if (!request.GetHeader("X-Forwarded-From-Node").empty()) {
            return GetHTTPBADREQUEST(request, "text/plain", "Can't do double forward");
        }
        // we expect that nodes order is the same for all requests
        TString address = GetAddressWithoutPort(request.GetRemoteAddr());
        ui64 hash = std::hash<TString>()(address);
        auto it = std::next(nodes.begin(), hash % nodes.size());

        TStringBuilder candidates;
        if (FromStringWithDefault<bool>(request.GetHeader("X-Get-Forwarded-Candidates"), false)) {
            candidates << "X-Forwarded-Remote-Addr: " << address << "\r\n";
            candidates << "X-Forwarded-Candidates: " << nodes << "\r\n";
            candidates << "X-Forwarded-Hash: " << hash << "\r\n";
        }
        TStringBuilder redirect;
        redirect << "/node/";
        redirect << *it;
        redirect << request.GetUri();
        return GetHTTPFORWARD(request, redirect, candidates);
    }

    std::unordered_map<TString, TActorId> RunningQueries;
    std::mutex RunningQueriesMutex;

    void AddRunningQuery(const TString& queryId, const TActorId& actorId) override {
        std::lock_guard guard(RunningQueriesMutex);
        RunningQueries[queryId] = actorId;
    }

    void EndRunningQuery(const TString& queryId, const TActorId& actorId) override {
        std::lock_guard guard(RunningQueriesMutex);
        auto it = RunningQueries.find(queryId);
        if (it != RunningQueries.end() && it->second == actorId) {
            RunningQueries.erase(it);
        }
    }

    TActorId FindRunningQuery(const TString& queryId) override {
        std::lock_guard guard(RunningQueriesMutex);
        auto it = RunningQueries.find(queryId);
        if (it != RunningQueries.end()) {
            return it->second;
        }
        return {};
    }

    NJson::TJsonValue GetCapabilities() override {
        std::lock_guard guard(JsonHandlersMutex);
        NJson::TJsonValue capabilities(NJson::JSON_MAP);
        for (const auto& [name, version] : JsonHandlers.Capabilities) {
            capabilities[name] = version;
        }
        return capabilities;
    }

    int GetCapabilityVersion(const TString& name) override {
        std::lock_guard guard(JsonHandlersMutex);
        return JsonHandlers.GetCapabilityVersion(name);
    }

    void RegisterVirtualHandler(
            NKikimrViewer::EObjectType parentObjectType,
            TVirtualHandlerType handler) override {
        VirtualHandlersByParentType.insert(std::make_pair(parentObjectType, TVirtualHandler(handler)));
    }

    TVector<const TVirtualHandler*> GetVirtualHandlers(NKikimrViewer::EObjectType type, const TString&/* path*/) const override {
        TVector<const TVirtualHandler*> handlers;
        auto its = VirtualHandlersByParentType.equal_range(type);
        for (auto it = its.first; it != its.second; ++it) {
            handlers.push_back(&it->second);
        }
        return handlers;
    }

    TContentHandler GetContentHandler(NKikimrViewer::EObjectType objectType) const override {
        auto rec = ContentHandlers.find(objectType);
        return (rec != ContentHandlers.end()) ? rec->second : (TContentHandler)nullptr;
    }

    void RegisterContentHandler(
        NKikimrViewer::EObjectType objectType,
        const TContentHandler& handler) override {
        if (handler) {
            ContentHandlers.emplace(objectType, handler);
        }
    }

private:
    TJsonHandlers JsonHandlers;
    std::mutex JsonHandlersMutex;
    std::unordered_map<TString, TString> Redirect307;
    const TKikimrRunConfig KikimrRunConfig;
    std::unordered_multimap<NKikimrViewer::EObjectType, TVirtualHandler> VirtualHandlersByParentType;
    std::unordered_map<NKikimrViewer::EObjectType, TContentHandler> ContentHandlers;
    TString AllowOrigin;
    ui32 CurrentMonitoringPort;
    TString CurrentWorkerName;

    void Handle(TEvents::TEvWakeup::TPtr&) {
        DeleteOldSharedCacheData();
        Schedule(TDuration::Seconds(10), new TEvents::TEvWakeup());
    }

    std::unordered_map<TTabletId, TActorId> TabletPipes;

    static NTabletPipe::TClientConfig GetPipeClientConfig() {
        return {
            .RetryPolicy = NTabletPipe::TClientRetryPolicy::WithRetries()
        };
    }

    void Handle(TEvTabletPipe::TEvClientConnected::TPtr& ev) {
        if (ev->Get()->Status != NKikimrProto::OK) {
            TabletPipes.erase(ev->Get()->TabletId);
        }
    }

    void Handle(TEvTabletPipe::TEvClientDestroyed::TPtr& ev) {
        TabletPipes.erase(ev->Get()->TabletId);
    }

    void Handle(TEvViewer::TEvUpdateSharedCacheTabletRequest::TPtr& ev) {
        auto itPipe = TabletPipes.find(ev->Get()->TabletId);
        if (itPipe == TabletPipes.end()) {
            auto pipe = NTabletPipe::CreateClient(SelfId(), ev->Get()->TabletId, GetPipeClientConfig());
            itPipe = TabletPipes.emplace(ev->Get()->TabletId, RegisterWithSameMailbox(pipe)).first;
        }
        NTabletPipe::SendData(SelfId(), itPipe->second, ev->Get()->Request.release());
    }

    void Handle(TEvViewer::TEvUpdateSharedCacheTabletResponse::TPtr& ev) {
        UpdateSharedCacheData(std::unique_ptr<TEvViewer::TEvUpdateSharedCacheTabletResponse>(ev->Release().Release()));
    }

    template<typename TEvent>
    void HandleForUpdateSharedCacheData(TAutoPtr<TEventHandle<TEvent>>& ev) {
        UpdateSharedCacheData(std::make_unique<TEvViewer::TEvUpdateSharedCacheTabletResponse>(std::shared_ptr<TEvent>(ev->Release().Release())));
    }

    void PassAway() override {
        for (const auto& [tabletId, pipe] : TabletPipes) {
            NTabletPipe::CloseClient(SelfId(), pipe);
        }
    }

    STFUNC(StateWork) {
        switch (ev->GetTypeRewrite()) {
            HFunc(NMon::TEvHttpInfo, Handle);
            hFunc(NHttp::TEvHttpProxy::TEvHttpIncomingRequest, Handle);
            hFunc(TEvViewer::TEvViewerRequest, Handle);
            hFunc(TEvViewer::TEvUpdateSharedCacheTabletRequest, Handle);
            hFunc(TEvViewer::TEvUpdateSharedCacheTabletResponse, Handle);
            hFunc(TEvTabletPipe::TEvClientConnected, Handle);
            hFunc(TEvTabletPipe::TEvClientDestroyed, Handle);
            hFunc(NSysView::TEvSysView::TEvGetStoragePoolsResponse, HandleForUpdateSharedCacheData);
            hFunc(NSysView::TEvSysView::TEvGetGroupsResponse, HandleForUpdateSharedCacheData);
            hFunc(NSysView::TEvSysView::TEvGetVSlotsResponse, HandleForUpdateSharedCacheData);
            hFunc(NSysView::TEvSysView::TEvGetPDisksResponse, HandleForUpdateSharedCacheData);
            hFunc(NSysView::TEvSysView::TEvGetStorageStatsResponse, HandleForUpdateSharedCacheData);
            hFunc(TEvents::TEvWakeup, Handle);
        }
    }

    YAML::Node GetSwaggerPathsYaml() {
        YAML::Node paths;
        for (const TString& name : JsonHandlers.JsonHandlersList) {
            const auto& handler = JsonHandlers.JsonHandlersIndex[name];
            auto swagger = handler->GetRequestSwagger();
            if (!swagger.IsNull()) {
                paths[name] = swagger;
            }
        }
        return paths;
    }

    YAML::Node GetSwaggerYaml() {
        YAML::Node yaml;
        yaml["openapi"] = "3.0.0";
        {
            auto info = yaml["info"];
            info["version"] = "1.0.0";
            info["title"] = "YDB Viewer";
            info["description"] = "YDB API for external introspection";
        }
        yaml["paths"] = GetSwaggerPathsYaml();

        return yaml;
    }

    static time_t GetCompileTimeSeconds() {
        tm compileTime = {};
        strptime(__DATE__ " " __TIME__, "%B %d %Y %H:%M:%S", &compileTime);
        return mktime(&compileTime);
    }

    static TInstant GetCompileTime() {
        static TInstant instantTime(TInstant::Seconds(GetCompileTimeSeconds()));
        return instantTime;
    }

    bool ReplyWithFile(NMon::TEvHttpInfo::TPtr& ev, const TString& name) {
        if (name == "/api/viewer.yaml") {
            Send(ev->Sender, new NMon::TEvHttpInfoRes(GetHTTPOKYAML(ev->Get(), Dump(GetSwaggerYaml()), GetCompileTime()), 0, NMon::IEvHttpInfoRes::EContentType::Custom));
            return true;
        }
        TString filename("content" + name);
        TString blob;
        TString type;
        TString lastModified;
        TFileStat fstat(filename);
        if (fstat.IsFile()) {
            blob = TUnbufferedFileInput(filename).ReadAll();
            if (!blob.empty()) {
                type = mimetypeByExt(filename.c_str());
                if (fstat.MTime) {
                    lastModified = TInstant::Seconds(fstat.MTime).ToRfc822String();
                }
            }
        }
        if (blob.empty()) {
            filename = TString("viewer") + name;
            if (NResource::FindExact(filename, &blob)) {
                type = mimetypeByExt(filename.c_str());
            } else {
                filename = name;
                if (filename.StartsWith("/")) {
                    filename.erase(0, 1);
                }
                if (NResource::FindExact(filename, &blob)) {
                    type = mimetypeByExt(filename.c_str());
                }
            }
            lastModified = GetCompileTime().ToRfc822String();
        }
        if (!blob.empty()) {
            if (name == "/index.html" || name == "/v2/index.html") { // we send root's index in such format that it could be embedded into existing web interface
                Send(ev->Sender, new NMon::TEvHttpInfoRes(TString(static_cast<const char*>(blob.data()), blob.size())));
            } else {
                TStringStream response;
                response << "HTTP/1.1 200 Ok\r\n";
                response << "Content-Type: " << type << "\r\n";
                response << "Content-Length: " << blob.size() << "\r\n";
                if (name == "/monitoring/index.html") {
                    response << "Cache-Control: no-store,max-age=0\r\n"; // do not cache
                } else {
                    response << "Date: " << TInstant::Now().ToRfc822String() << "\r\n";
                    if (lastModified) {
                        response << "Last-Modified: " << lastModified << "\r\n";
                    }
                    response << "Cache-Control: max-age=604800\r\n"; // one week
                }
                response << "\r\n";
                response.Write(blob.data(), blob.size());
                Send(ev->Sender, new NMon::TEvHttpInfoRes(response.Str(), 0, NMon::IEvHttpInfoRes::EContentType::Custom));
            }
            return true;
        }
        return false;
    }

    void Handle(TEvViewer::TEvViewerRequest::TPtr& ev) {
        IActor* actor = CreateViewerRequestHandler(ev);
        if (actor) {
            Register(actor);
        } else {
            BLOG_ERROR("Unable to process EvViewerRequest");
            Send(ev->Sender, new TEvViewer::TEvViewerResponse(), 0, ev->Cookie);
        }
    }

    void Handle(NMon::TEvHttpInfo::TPtr &ev, const TActorContext &ctx) {
        NMon::TEvHttpInfo* msg = ev->Get();
        if (msg->Request.GetMethod() == HTTP_METHOD_OPTIONS) {
            TString url(msg->Request.GetPathInfo());
            TString type = mimetypeByExt(url.c_str());
            if (type.empty()) {
                type = "application/json";
            }
            TStringBuilder response;
            response << "HTTP/1.1 204 No Content\r\n";
            FillCORS(response, msg);
            response << "Content-Type: " << type << "\r\n"
                        "Connection: Keep-Alive\r\n\r\n";
            Send(ev->Sender, new NMon::TEvHttpInfoRes(response, 0, NMon::IEvHttpInfoRes::EContentType::Custom));
            return;
        }
        TString path("/" + msg->Request.GetPage()->Path + msg->Request.GetPathInfo());
        auto itRedirect307 = Redirect307.find(path);
        if (itRedirect307 != Redirect307.end()) {
            TString redirect(msg->Request.GetUri());
            redirect.erase(0, itRedirect307->first.size());
            redirect.insert(0, itRedirect307->second);
            Send(ev->Sender, new NMon::TEvHttpInfoRes("HTTP/1.1 307 Temporary Redirect\r\nLocation: " + redirect + "\r\n\r\n", 0, NMon::IEvHttpInfoRes::EContentType::Custom));
            return;
        }
        auto handler = JsonHandlers.FindHandler(path);
        if (handler) {
            auto sender(ev->Sender);
            try {
                IActor* requestActor = handler->CreateRequestActor(this, ev);
                if (requestActor == nullptr) {
                    Send(sender, new NMon::TEvHttpInfoRes(GetHTTPINTERNALERROR(ev->Get(), "text/plain", "Wrong type of the handler (not evinfo)"), 0, NMon::IEvHttpInfoRes::EContentType::Custom));
                } else {
                    Register(requestActor);
                }
                return;
            }
            catch (const std::exception& e) {
                Send(sender, new NMon::TEvHttpInfoRes(TString("HTTP/1.1 400 Bad Request\r\n\r\n") + e.what(), 0, NMon::IEvHttpInfoRes::EContentType::Custom));
                return;
            }
        }
        if (path.StartsWith("/counters/hosts")) {
            Register(new TCountersHostsList(this, ev));
            return;
        }
        if (path.StartsWith("/healthcheck")) { // healthcheck no auth scrapping
            Register(new TJsonHealthCheck(this, ev));
            return;
        }
        // TODO: check path validity
        // TODO: cache
        if (msg->Request.GetPathInfo().StartsWith('/')) {
            if (path.StartsWith("/viewer")) {
                path.erase(0, 7);
            }
            if (IsMatchesWildcard(path, "/monitoring*/static/js/*")
            || IsMatchesWildcard(path, "/monitoring*/static/css/*")
            || IsMatchesWildcard(path, "/monitoring*/static/media/*")
            || IsMatchesWildcard(path, "/monitoring*/static/assets/fonts/*")
            || IsMatchesWildcard(path, "/monitoring*/static/favicon.png")) {
                auto resPos = path.find("/static/");
                if (resPos != TString::npos) {
                    path = "/monitoring" + path.substr(resPos);
                }
            } else if (path.StartsWith("/monitoring") && path != "/monitoring/index.html") {
                 path = "/monitoring/index.html";
            }
            if (path.EndsWith('/')) {
                path += "index.html";
            }
            if (ReplyWithFile(ev, path)) {
                return;
            }
        }
        if (msg->Request.GetPathInfo() == "/tablet") {
            ui64 id = FromStringWithDefault<ui64>(ev->Get()->Request.GetParams().Get("id"), 0);
            ctx.Send(ev->Sender, new NMon::TEvHttpInfoRes("HTTP/1.1 302 Found\r\nLocation: ../tablets?TabletID=" + ToString(id) + "\r\n\r\n", 0, NMon::IEvHttpInfoRes::EContentType::Custom));
            return;
        }
        if (msg->Request.GetPathInfo().empty()) {
            ctx.Send(ev->Sender, new NMon::TEvHttpInfoRes("HTTP/1.1 302 Found\r\nLocation: " + SplitPath(msg->Request.GetPage()->Path).back() + "/\r\n\r\n", 0, NMon::IEvHttpInfoRes::EContentType::Custom));
            return;
        }
        Send(ev->Sender, new NMon::TEvHttpInfoRes(GetHTTPNOTFOUND(ev->Get()), 0, NMon::IEvHttpInfoRes::EContentType::Custom));
    }

    void Handle(NHttp::TEvHttpProxy::TEvHttpIncomingRequest::TPtr& ev) {
        if (ev->Get()->Request->Method == "OPTIONS") {
            TString uri(ev->Get()->Request->GetURI());
            TString type = mimetypeByExt(uri.c_str());
            if (type.empty()) {
                type = "application/json";
            }
            TStringBuilder response;
            response << "HTTP/1.1 204 No Content\r\n";
            FillCORS(response, ev->Get());
            response << "Content-Type: " << type << "\r\n"
                        "Connection: Keep-Alive\r\n\r\n";
            Send(ev->Sender, new NHttp::TEvHttpProxy::TEvHttpOutgoingResponse(ev->Get()->Request->CreateResponseString(response)));
            return;
        }
        TString path(ev->Get()->Request->GetURI());
        auto itRedirect307 = Redirect307.find(path);
        if (itRedirect307 != Redirect307.end()) {
            TString redirect(ev->Get()->Request->URL);
            redirect.erase(0, itRedirect307->first.size());
            redirect.insert(0, itRedirect307->second);
            TStringBuilder response;
            response << "HTTP/1.1 307 Temporary Redirect\r\n";
            FillCORS(response, ev->Get());
            response << "Location: " + redirect + "\r\n"
                        "Connection: Keep-Alive\r\n\r\n";
            Send(ev->Sender, new NHttp::TEvHttpProxy::TEvHttpOutgoingResponse(ev->Get()->Request->CreateResponseString(response)));
            return;
        }
        auto handler = JsonHandlers.FindHandler(path);
        if (handler) {
            auto sender(ev->Sender);
            try {
                IActor* requestActor = handler->CreateRequestActor(this, ev);
                if (requestActor == nullptr) {
                    Send(ev->Sender, new NHttp::TEvHttpProxy::TEvHttpOutgoingResponse(ev->Get()->Request->CreateResponseString(
                        GetHTTPINTERNALERROR(ev->Get(), "text/plain", "Wrong type of the handler (not http)"))));
                } else {
                    Register(requestActor);
                }
                return;
            }
            catch (const std::exception& e) {
                Send(sender, new NHttp::TEvHttpProxy::TEvHttpOutgoingResponse(ev->Get()->Request->CreateResponseBadRequest()));
                return;
            }
        }
        Send(ev->Sender, new NHttp::TEvHttpProxy::TEvHttpOutgoingResponse(ev->Get()->Request->CreateResponseString(GetHTTPNOTFOUND(ev->Get()))));
    }
};

TString IViewer::TContentRequestContext::Dump() const
{
    auto typeToString = [] (int type) -> TString {
        using namespace NKikimrViewer;
        if (EObjectType_IsValid(type)) {
            return EObjectType_Name((EObjectType)type);
        } else {
            return TStringBuilder() << "unknown (" << type << ")";
        }
    };

    TStringBuilder result;
    result << "Path = " << Path << Endl;
    result << "Name = " << ObjectName << Endl;
    result << "Type = " << typeToString(Type) << Endl;

    result << "Limit = " << Limit << Endl;
    result << "Offset = " << Offset << Endl;
    result << "Key = " << Key << Endl;

    result << "JsonSettings.EnumAsNumbers = " << JsonSettings.EnumAsNumbers << Endl;
    result << "JsonSettings.UI64AsString = " << JsonSettings.UI64AsString << Endl;
    result << "Timeout = " << Timeout.MilliSeconds() << " ms" << Endl;

    return result;
}

IActor* CreateViewer(const TKikimrRunConfig& kikimrRunConfig) {
    return new TViewer(kikimrRunConfig);
}

void TViewer::FillCORS(TStringBuilder& stream, const TRequestState& request) {
    TString requestOrigin = request && request.HasHeader("Origin") ? request.GetHeader("Origin") : TString();
    TString origin;
    if (AllowOrigin) {
        if (IsMatchesWildcards(requestOrigin, AllowOrigin)) {
            origin = requestOrigin;
        } else {
            return; // no CORS headers - no access
        }
    } else if (requestOrigin) {
        origin = requestOrigin;
    }
    if (origin.empty()) {
        origin = "*";
    }
    if (origin) {
        stream << "Access-Control-Allow-Origin: " << origin << "\r\n"
               << "Access-Control-Allow-Credentials: true\r\n"
               << "Access-Control-Allow-Headers: Content-Type,Authorization,Origin,Accept,X-Trace-Verbosity,X-Want-Trace,traceparent\r\n"
               << "Access-Control-Expose-Headers: traceresponse,X-Worker-Name\r\n"
               << "Access-Control-Allow-Methods: OPTIONS,GET,POST,PUT,DELETE\r\n"
               << "Allow: OPTIONS,GET,POST,DELETE\r\n";
    }
}

void TViewer::FillTraceId(TStringBuilder& stream, const TRequestState& request) {
    if (request.TraceId) {
        stream << "traceresponse: " << request.TraceId.ToTraceresponseHeader() << "\r\n";
    }
}

TString TViewer::GetHTTPGATEWAYTIMEOUT(const TRequestState& request, TString contentType, TString response) {
    TStringBuilder res;
    res << "HTTP/1.1 504 Gateway Timeout\r\n"
        << "Connection: Close\r\n"
        << "X-Worker-Name: " << CurrentWorkerName << "\r\n";
    if (contentType) {
        res << "Content-Type: " << contentType << "\r\n";
    }
    if (response) {
        res << "Content-Length: " << response.size() << "\r\n";
    }
    FillCORS(res, request);
    FillTraceId(res, request);
    res << "\r\n";
    if (response) {
        res << response << "\r\n";
    }
    return res;
}

TString TViewer::GetHTTPBADREQUEST(const TRequestState& request, TString contentType, TString response) {
    TStringBuilder res;
    res << "HTTP/1.1 400 Bad Request\r\n"
        << "Connection: Close\r\n"
        << "X-Worker-Name: " << CurrentWorkerName << "\r\n";
    if (contentType) {
        res << "Content-Type: " << contentType << "\r\n";
    }
    if (response) {
        res << "Content-Length: " << response.size() << "\r\n";
    }
    FillCORS(res, request);
    FillTraceId(res, request);
    res << "\r\n";
    if (response) {
        res << response;
    }
    return res;
}

TString TViewer::GetHTTPFORBIDDEN(const TRequestState& request, TString contentType, TString response) {
    TStringBuilder res;
    res << "HTTP/1.1 403 Forbidden\r\n"
        << "Connection: Close\r\n";
    if (contentType) {
        res << "Content-Type: " << contentType << "\r\n";
    }
    if (response) {
        res << "Content-Length: " << response.size() << "\r\n";
    }
    FillCORS(res, request);
    FillTraceId(res, request);
    res << "\r\n";
    if (response) {
        res << response;
    }
    return res;
}

TString TViewer::GetHTTPNOTFOUND(const TRequestState& request) {
    TStringBuilder res;
    res << "HTTP/1.1 404 Not Found\r\n"
        << "X-Worker-Name: " << CurrentWorkerName << "\r\n"
        << "Connection: Close\r\n"
        << "Content-Length: 0\r\n";
    FillCORS(res, request);
    FillTraceId(res, request);
    res << "\r\n";
    return res;
}

TString TViewer::GetHTTPOK(const TRequestState& request, TString contentType, TString response, TInstant lastModified) {
    TStringBuilder res;
    res << "HTTP/1.1 200 Ok\r\n"
        << "X-Worker-Name: " << CurrentWorkerName << "\r\n";
    FillCORS(res, request);
    FillTraceId(res, request);
    if (response) {
        res << "Content-Type: " << contentType << "\r\n";
        res << "Content-Length: " << response.size() << "\r\n";
        if (lastModified != TInstant()) {
            res << "Date: " << TInstant::Now().ToRfc822String() << "\r\n";
            res << "Last-Modified: " << lastModified.ToRfc822String() << "\r\n";
            res << "Cache-Control: max-age=604800\r\n"; // one week
        }
    } else {
        res << "Content-Length: 0\r\n";
    }
    res << "\r\n";
    if (response) {
        res << response;
    }
    return res;
}

TString TViewer::GetChunkedHTTPOK(const TRequestState& request, TString contentType) {
    TStringBuilder res;
    res << "HTTP/1.1 200 Ok\r\n"
        << "X-Worker-Name: " << CurrentWorkerName << "\r\n";
    FillCORS(res, request);
    FillTraceId(res, request);
    if (contentType) {
        res << "Content-Type: " << contentType << "\r\n";
    }
    res << "Transfer-Encoding: chunked\r\n";
    res << "\r\n";
    return res;
}

TString TViewer::GetHTTPINTERNALERROR(const TRequestState& request, TString contentType, TString response) {
    TStringBuilder res;
    res << "HTTP/1.1 500 Internal Server Error\r\n"
        << "X-Worker-Name: " << CurrentWorkerName << "\r\n";
    FillCORS(res, request);
    FillTraceId(res, request);
    if (response) {
        res << "Content-Type: " << contentType << "\r\n";
        res << "Content-Length: " << response.size() << "\r\n";
    } else {
        res << "Content-Length: 0\r\n";
    }
    res << "\r\n";
    if (response) {
        res << response;
    }
    return res;
}

TString TViewer::GetHTTPFORWARD(const TRequestState& request, const TString& location, const TString& candidates) {
    TStringBuilder res;
    res << "HTTP/1.1 307 Temporary Redirect\r\n"
        << "Location: " << location << "\r\n"
        << candidates
        << "Connection: Keep-Alive\r\n"
        << "Content-Length: 0\r\n";
    FillCORS(res, request);
    FillTraceId(res, request);
    res << "\r\n";
    return res;
}

NKikimrViewer::EFlag GetFlagFromTabletState(NKikimrWhiteboard::TTabletStateInfo::ETabletState state) {
    NKikimrViewer::EFlag flag = NKikimrViewer::EFlag::Grey;
    switch (state) {
    case NKikimrWhiteboard::TTabletStateInfo::Dead:
        flag = NKikimrViewer::EFlag::Red;
        break;
    case NKikimrWhiteboard::TTabletStateInfo::Created:
    case NKikimrWhiteboard::TTabletStateInfo::ResolveStateStorage:
    case NKikimrWhiteboard::TTabletStateInfo::Candidate:
    case NKikimrWhiteboard::TTabletStateInfo::BlockBlobStorage:
    case NKikimrWhiteboard::TTabletStateInfo::WriteZeroEntry:
    case NKikimrWhiteboard::TTabletStateInfo::Restored:
    case NKikimrWhiteboard::TTabletStateInfo::Discover:
    case NKikimrWhiteboard::TTabletStateInfo::Lock:
    case NKikimrWhiteboard::TTabletStateInfo::RebuildGraph:
    case NKikimrWhiteboard::TTabletStateInfo::ResolveLeader:
        flag = NKikimrViewer::EFlag::Yellow;
        break;
    case NKikimrWhiteboard::TTabletStateInfo::Deleted:
    case NKikimrWhiteboard::TTabletStateInfo::Active:
        flag = NKikimrViewer::EFlag::Green;
        break;
    default:
        break;
    }
    return flag;
}

NKikimrViewer::EFlag GetFlagFromTabletState(NKikimrHive::ETabletVolatileState state) {
    NKikimrViewer::EFlag flag = NKikimrViewer::EFlag::Grey;
    switch (state) {
        case NKikimrHive::ETabletVolatileState::TABLET_VOLATILE_STATE_STOPPED:
            flag = NKikimrViewer::EFlag::Red;
            break;
        case NKikimrHive::ETabletVolatileState::TABLET_VOLATILE_STATE_BOOTING:
            flag = NKikimrViewer::EFlag::Orange;
            break;
        case NKikimrHive::ETabletVolatileState::TABLET_VOLATILE_STATE_STARTING:
            flag = NKikimrViewer::EFlag::Yellow;
            break;
        case NKikimrHive::ETabletVolatileState::TABLET_VOLATILE_STATE_RUNNING:
            flag = NKikimrViewer::EFlag::Green;
            break;
        default:
            flag = NKikimrViewer::EFlag::Red;
            break;
    }
    return flag;
}

NKikimrViewer::EFlag GetFlagFromUsage(double usage) {
    NKikimrViewer::EFlag flag = NKikimrViewer::EFlag::Grey;
    if (usage >= 0.94) {
        flag = NKikimrViewer::EFlag::Red;
    } else if (usage >= 0.92) {
        flag = NKikimrViewer::EFlag::Orange;
    } else if (usage >= 0.85) {
        flag = NKikimrViewer::EFlag::Yellow;
    } else  {
        flag = NKikimrViewer::EFlag::Green;
    }
    return flag;
}

NKikimrViewer::EFlag GetPDiskStateFlag(const NKikimrWhiteboard::TPDiskStateInfo& info) {
    NKikimrViewer::EFlag flag = NKikimrViewer::EFlag::Grey;
    switch (info.GetState()) {
        case NKikimrBlobStorage::TPDiskState::Normal:
            flag = NKikimrViewer::EFlag::Green;
            break;
        case NKikimrBlobStorage::TPDiskState::Initial:
        case NKikimrBlobStorage::TPDiskState::InitialFormatRead:
        case NKikimrBlobStorage::TPDiskState::InitialSysLogRead:
        case NKikimrBlobStorage::TPDiskState::InitialCommonLogRead:
            flag = NKikimrViewer::EFlag::Yellow;
            break;
        case NKikimrBlobStorage::TPDiskState::InitialFormatReadError:
        case NKikimrBlobStorage::TPDiskState::InitialSysLogReadError:
        case NKikimrBlobStorage::TPDiskState::InitialSysLogParseError:
        case NKikimrBlobStorage::TPDiskState::InitialCommonLogReadError:
        case NKikimrBlobStorage::TPDiskState::InitialCommonLogParseError:
        case NKikimrBlobStorage::TPDiskState::CommonLoggerInitError:
        case NKikimrBlobStorage::TPDiskState::OpenFileError:
        case NKikimrBlobStorage::TPDiskState::Stopped:
            flag = NKikimrViewer::EFlag::Red;
            break;
        default:
            flag = NKikimrViewer::EFlag::Red;
            break;
    }
    return flag;
}

NKikimrViewer::EFlag GetPDiskOverallFlag(const NKikimrWhiteboard::TPDiskStateInfo& info) {
    NKikimrViewer::EFlag flag = NKikimrViewer::EFlag::Grey;
    flag = Max(flag, GetPDiskStateFlag(info));
    if (info.HasDevice()) {
        flag = Max(flag, Min(NKikimrViewer::EFlag::Orange, GetViewerFlag(info.GetDevice())));
    }
    if (info.HasRealtime()) {
        flag = Max(flag, Min(NKikimrViewer::EFlag::Orange, GetViewerFlag(info.GetRealtime())));
    }
    if (info.HasAvailableSize() && info.GetTotalSize() != 0) {
        double avail = (double)info.GetAvailableSize() / info.GetTotalSize();
        if (avail <= 0.06) {
            flag = Max(flag, NKikimrViewer::EFlag::Red);
        } else if (avail <= 0.08) {
            flag = Max(flag, NKikimrViewer::EFlag::Orange);
        } else if (avail <= 0.15) {
            flag = Max(flag, NKikimrViewer::EFlag::Yellow);
        }
    }
    return flag;
}

NKikimrViewer::EFlag GetVDiskOverallFlag(const NKikimrWhiteboard::TVDiskStateInfo& info) {
    NKikimrViewer::EFlag flag = NKikimrViewer::EFlag::Grey;
    switch (info.GetVDiskState()) {
        case NKikimrWhiteboard::EVDiskState::Initial:
        case NKikimrWhiteboard::EVDiskState::SyncGuidRecovery:
            flag = NKikimrViewer::EFlag::Yellow;
            break;
        case NKikimrWhiteboard::EVDiskState::LocalRecoveryError:
        case NKikimrWhiteboard::EVDiskState::SyncGuidRecoveryError:
        case NKikimrWhiteboard::EVDiskState::PDiskError:
            flag = NKikimrViewer::EFlag::Red;
            break;
        case NKikimrWhiteboard::EVDiskState::OK:
            flag = NKikimrViewer::EFlag::Green;
            break;
        default:
            flag = NKikimrViewer::EFlag::Red;
            break;
    }
    if (info.HasDiskSpace()) {
        flag = Max(flag, GetViewerFlag(info.GetDiskSpace()));
    }
    if (info.HasSatisfactionRank()
            && info.GetSatisfactionRank().HasFreshRank()
            && info.GetSatisfactionRank().GetFreshRank().HasFlag()) {
        flag = Max(flag, Min(NKikimrViewer::EFlag::Orange, GetViewerFlag(info.GetSatisfactionRank().GetFreshRank().GetFlag())));
    }
    if (info.HasSatisfactionRank()
            && info.GetSatisfactionRank().HasLevelRank()
            && info.GetSatisfactionRank().GetLevelRank().HasFlag()) {
        flag = Max(flag, Min(NKikimrViewer::EFlag::Orange, GetViewerFlag(info.GetSatisfactionRank().GetLevelRank().GetFlag())));
    }
    if (info.HasFrontQueues()) {
        flag = Max(flag, Min(NKikimrViewer::EFlag::Orange, GetViewerFlag(info.GetFrontQueues())));
    }
    if (info.HasReplicated() && !info.GetReplicated()) {
        flag = Max(flag, NKikimrViewer::EFlag::Blue);
    }
    return flag;
}

NKikimrViewer::EFlag GetViewerFlag(Ydb::Monitoring::StatusFlag::Status flag) {
    switch (flag) {
    case Ydb::Monitoring::StatusFlag::GREY:
    case Ydb::Monitoring::StatusFlag::UNSPECIFIED:
    case Ydb::Monitoring::StatusFlag_Status_StatusFlag_Status_INT_MIN_SENTINEL_DO_NOT_USE_:
    case Ydb::Monitoring::StatusFlag_Status_StatusFlag_Status_INT_MAX_SENTINEL_DO_NOT_USE_:
        return NKikimrViewer::EFlag::Grey;
    case Ydb::Monitoring::StatusFlag::GREEN:
        return NKikimrViewer::EFlag::Green;
    case Ydb::Monitoring::StatusFlag::BLUE:
        return NKikimrViewer::EFlag::Green;
    case Ydb::Monitoring::StatusFlag::YELLOW:
        return NKikimrViewer::EFlag::Yellow;
    case Ydb::Monitoring::StatusFlag::ORANGE:
        return NKikimrViewer::EFlag::Orange;
    case Ydb::Monitoring::StatusFlag::RED:
        return NKikimrViewer::EFlag::Red;
    }
}

NKikimrWhiteboard::EFlag GetWhiteboardFlag(NKikimrViewer::EFlag flag) {
    switch (flag) {
    case NKikimrViewer::EFlag::Grey:
    case NKikimrViewer::EFlag::EFlag_INT_MIN_SENTINEL_DO_NOT_USE_:
    case NKikimrViewer::EFlag::EFlag_INT_MAX_SENTINEL_DO_NOT_USE_:
        return NKikimrWhiteboard::EFlag::Grey;
    case NKikimrViewer::EFlag::Green:
        return NKikimrWhiteboard::EFlag::Green;
    case NKikimrViewer::EFlag::Blue:
        return NKikimrWhiteboard::EFlag::Green;
    case NKikimrViewer::EFlag::Yellow:
        return NKikimrWhiteboard::EFlag::Yellow;
    case NKikimrViewer::EFlag::Orange:
        return NKikimrWhiteboard::EFlag::Orange;
    case NKikimrViewer::EFlag::Red:
        return NKikimrWhiteboard::EFlag::Red;
    }
}

NKikimrViewer::EFlag GetViewerFlag(NKikimrWhiteboard::EFlag flag) {
    switch (flag) {
    case NKikimrWhiteboard::EFlag::Grey:
        return NKikimrViewer::EFlag::Grey;
    case NKikimrWhiteboard::EFlag::Green:
        return NKikimrViewer::EFlag::Green;
    case NKikimrWhiteboard::EFlag::Yellow:
        return NKikimrViewer::EFlag::Yellow;
    case NKikimrWhiteboard::EFlag::Orange:
        return NKikimrViewer::EFlag::Orange;
    case NKikimrWhiteboard::EFlag::Red:
        return NKikimrViewer::EFlag::Red;
    }
    return static_cast<NKikimrViewer::EFlag>((int)flag);
}

}
