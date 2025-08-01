#include "grpc_server.h"

#include <library/cpp/monlib/dynamic_counters/counters.h>
#include <library/cpp/time_provider/monotonic.h>

#include <util/string/join.h>
#include <util/generic/yexception.h>
#include <util/system/thread.h>
#include <util/generic/map.h>

#include <grpcpp/resource_quota.h>
#include <contrib/libs/grpc/src/core/lib/iomgr/socket_mutator.h>

#if !defined(_WIN32) && !defined(_WIN64)

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#endif

namespace NYdbGrpc {

static void PullEvents(grpc::ServerCompletionQueue* cq, TIntrusivePtr<::NMonitoring::TDynamicCounters> counters) {
    TThread::SetCurrentThreadName("grpc_server");
    auto okCounter = counters->GetCounter("RequestExecuted", true);
    auto errorCounter = counters->GetCounter("RequestDestroyed", true);
    auto cpuTime = counters->GetCounter("ThreadCPU", true);

    NMonotonic::TMonotonic lastCpuTimeTs = {};
    while (true) {
        void* tag; // uniquely identifies a request.
        bool ok;

        auto now = NMonotonic::TMonotonic::Now();
        if (now - lastCpuTimeTs >= TDuration::Seconds(1)) {
            lastCpuTimeTs = now;
            *cpuTime = ThreadCPUTime();
        }

        if (cq->Next(&tag, &ok)) {
            IQueueEvent* const ev(static_cast<IQueueEvent*>(tag));

            if (ev->Execute(ok)) {
                okCounter->Inc();
            } else {
                ev->DestroyRequest();
                errorCounter->Inc();
            }
        } else {
            break;
        }
    }
}

void TGrpcServiceProtectiable::StopService() noexcept {
    AtomicSet(ShuttingDown_, 1);

    for (auto& shard : Shards_) {
        with_lock(shard.Lock_) {
            // Send TryCansel to event (can be send after finishing).
            // Actual dtors will be called from grpc thread, so deadlock impossible
            for (auto* request : shard.Requests_) {
                request->Shutdown();
            }
        }
    }
}

size_t TGrpcServiceProtectiable::RequestsInProgress() const {
    size_t c = 0;
    for (auto& shard : Shards_) {
        with_lock(shard.Lock_) {
            c += shard.Requests_.size();
        }
    }
    return c;
}

bool TGrpcServiceProtectiable::RegisterRequestCtx(ICancelableContext* req) {
    if (Y_LIKELY(req->ShardIndex == size_t(-1))) {
        req->ShardIndex = NextShard_.fetch_add(1, std::memory_order_relaxed) % Shards_.size();
    }

    auto& shard = Shards_[req->ShardIndex];
    with_lock(shard.Lock_) {
        if (IsShuttingDown()) {
            return false;
        }

        auto r = shard.Requests_.emplace(req);
        Y_ABORT_UNLESS(r.second, "Ctx already registered");
    }

    return true;
}

void TGrpcServiceProtectiable::DeregisterRequestCtx(ICancelableContext* req) {
    Y_ABORT_UNLESS(req->ShardIndex != size_t(-1), "Ctx does not have an assigned shard index");

    auto& shard = Shards_[req->ShardIndex];
    with_lock(shard.Lock_) {
        Y_ABORT_UNLESS(shard.Requests_.erase(req), "Ctx is not registered");
    }
}

bool TGrpcServiceProtectiable::IncRequest() {
    if (Limiter_) {
        return Limiter_->Inc();
    }
    return true;
}

void TGrpcServiceProtectiable::DecRequest() {
    if (Limiter_) {
        Limiter_->Dec();
        Y_ASSERT(Limiter_->GetCurrentInFlight() >= 0);
    }
}

TGRpcServer::TGRpcServer(const TServerOptions& opts, TIntrusivePtr<::NMonitoring::TDynamicCounters> counters)
    : Options_(opts)
    , Counters_(std::move(counters))
    , Limiter_(Options_.MaxGlobalRequestInFlight)
{
    if (!Counters_) {
        // make a stub to simplify code
        Counters_.Reset(new ::NMonitoring::TDynamicCounters());
    }
}

TGRpcServer::~TGRpcServer() {
    Y_ABORT_UNLESS(Ts.empty());
    Services_.clear();
}

void TGRpcServer::AddService(IGRpcServicePtr service) {
    Services_.push_back(service);
}

void TGRpcServer::Start() {
    TString server_address(Join(":", Options_.Host, Options_.Port)); // https://st.yandex-team.ru/DTCC-695
    using grpc::ServerBuilder;
    using grpc::ResourceQuota;
    ServerBuilder builder;
    auto credentials = grpc::InsecureServerCredentials();
    if (Options_.SslData) {
        grpc::SslServerCredentialsOptions::PemKeyCertPair keycert;
        keycert.cert_chain = std::move(Options_.SslData->Cert);
        keycert.private_key = std::move(Options_.SslData->Key);
        grpc::SslServerCredentialsOptions sslOps;
        sslOps.pem_root_certs = std::move(Options_.SslData->Root);
        sslOps.pem_key_cert_pairs.push_back(keycert);

        if (Options_.SslData->DoRequestClientCertificate) {
            sslOps.client_certificate_request = GRPC_SSL_REQUEST_CLIENT_CERTIFICATE_AND_VERIFY;
        }

        credentials = grpc::SslServerCredentials(sslOps);
    }
    if (Options_.ExternalListener) {
        Options_.ExternalListener->Init(builder.experimental().AddExternalConnectionAcceptor(
            ServerBuilder::experimental_type::ExternalConnectionType::FROM_FD,
            credentials
        ));
    } else {
        builder.AddListeningPort(server_address, credentials);
    }
    builder.SetMaxReceiveMessageSize(Options_.MaxMessageSize);
    builder.SetMaxSendMessageSize(Options_.MaxMessageSize);
    for (IGRpcServicePtr service : Services_) {
        service->SetServerOptions(Options_);
        builder.RegisterService(service->GetService());
        service->SetGlobalLimiterHandle(&Limiter_);
    }

    class TKeepAliveOption: public grpc::ServerBuilderOption {
    public:
        TKeepAliveOption(int idle, int interval)
            : Idle(idle)
            , Interval(interval)
            , KeepAliveEnabled(true)
        {}

        TKeepAliveOption()
            : Idle(0)
            , Interval(0)
            , KeepAliveEnabled(false)
       {}

       void UpdateArguments(grpc::ChannelArguments *args) override {
            args->SetInt(GRPC_ARG_HTTP2_MAX_PING_STRIKES, 0);
            args->SetInt(GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS, 1000);
            if (KeepAliveEnabled) {
                args->SetInt(GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA, 0);
                args->SetInt(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
                args->SetInt(GRPC_ARG_HTTP2_MIN_SENT_PING_INTERVAL_WITHOUT_DATA_MS, Idle * 1000);
                args->SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, Idle * 1000);
                args->SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, Interval * 1000);
            }
        }

        void UpdatePlugins(std::vector<std::unique_ptr<grpc::ServerBuilderPlugin>>* /*plugins*/) override
        {}
    private:
        const int Idle;
        const int Interval;
        const bool KeepAliveEnabled;
    };

    if (Options_.KeepAliveEnable) {
        builder.SetOption(std::make_unique<TKeepAliveOption>(
            Options_.KeepAliveIdleTimeoutTriggerSec,
            Options_.KeepAliveProbeIntervalSec));
    } else {
        builder.SetOption(std::make_unique<TKeepAliveOption>());
    }

    size_t completionQueueCount = 1;
    if (Options_.WorkersPerCompletionQueue) {
        size_t threadsPerQueue = Max(std::size_t{1}, Options_.WorkersPerCompletionQueue);
        completionQueueCount = (Options_.WorkerThreads + threadsPerQueue - 1)  / threadsPerQueue; // ceiling
    } else if (Options_.UseCompletionQueuePerThread) {
        completionQueueCount = Options_.WorkerThreads;
    }

    CQS_.reserve(completionQueueCount);
    for (size_t i = 0; i < completionQueueCount; ++i) {
        CQS_.push_back(builder.AddCompletionQueue());
    }

    if (Options_.GRpcMemoryQuotaBytes) {
        // See details KIKIMR-6932
        if (Options_.EnableGRpcMemoryQuota) {
            grpc::ResourceQuota quota("memory_bound");
            quota.Resize(Options_.GRpcMemoryQuotaBytes);

            builder.SetResourceQuota(quota);

            Cerr << "Set GRpc memory quota to: " << Options_.GRpcMemoryQuotaBytes << Endl;
        } else {
            Cerr << "GRpc memory quota was set but disabled due to issues with grpc quoter"
                ", to enable it use EnableGRpcMemoryQuota option" << Endl;
        }
    }
    Options_.ServerBuilderMutator(builder);
    builder.SetDefaultCompressionLevel(Options_.DefaultCompressionLevel);

    Server_ = builder.BuildAndStart();
    if (!Server_) {
        ythrow yexception() << "can't start grpc server on " << server_address;
    }

    size_t index = 0;
    for (IGRpcServicePtr service : Services_) {
        // TODO: provide something else for services instead of ServerCompletionQueue
        service->InitService(CQS_, Options_.Logger, index++);
    }

    Ts.reserve(Options_.WorkerThreads);
    auto grpcCounters = Counters_->GetSubgroup("counters", "grpc");
    for (size_t i = 0; i < Options_.WorkerThreads; ++i) {
        auto* cq = &CQS_[i % CQS_.size()];
        auto workerCounters = grpcCounters->GetSubgroup("worker", ToString(i));
        Ts.push_back(SystemThreadFactory()->Run([cq, workerCounters] {
            PullEvents(cq->get(), std::move(workerCounters));
        }));
    }

    if (Options_.ExternalListener) {
        Options_.ExternalListener->Start();
    }
}

void TGRpcServer::Stop() {
    NYdbGrpc::GrpcDead = 1;
    for (auto& service : Services_) {
        service->StopService();
    }

    auto now = TInstant::Now();

    if (Server_) {
        i64 sec = Options_.GRpcShutdownDeadline.Seconds();
        Y_ABORT_UNLESS(Options_.GRpcShutdownDeadline.NanoSecondsOfSecond() <= Max<i32>());
        i32 nanosecOfSec =  Options_.GRpcShutdownDeadline.NanoSecondsOfSecond();
        Server_->Shutdown(gpr_timespec{sec, nanosecOfSec, GPR_TIMESPAN});
    }

    for (ui64 attempt = 0; ; ++attempt) {
        bool unsafe = false;
        size_t infly = 0;
        for (auto& service : Services_) {
            unsafe |= service->IsUnsafeToShutdown();
            infly += service->RequestsInProgress();
        }

        if (!unsafe && !infly)
            break;

        auto spent = (TInstant::Now() - now).SecondsFloat();
        if ((attempt + 1) % 300 == 0) {
            // don't log too much
            Cerr << "GRpc shutdown warning: left infly: " << infly << ", spent: " << spent << " sec. GRpcShutdownDeadline: "
                 << Options_.GRpcShutdownDeadline.SecondsFloat() << Endl;
        }

        if (!unsafe && spent > Options_.GRpcShutdownDeadline.SecondsFloat()) {
            Cerr << "GRpc shutdown warning: failed to shutdown all connections, left infly: " << infly << ", spent: " << spent
                 << " sec. GRpcShutdownDeadline: " << Options_.GRpcShutdownDeadline.SecondsFloat() << Endl;
            break;
        }
        Sleep(TDuration::MilliSeconds(10));
    }

    // Always shutdown the completion queue after the server.
    for (auto& cq : CQS_) {
        cq->Shutdown();
    }

    for (auto ti = Ts.begin(); ti != Ts.end(); ++ti) {
        (*ti)->Join();
    }

    Ts.clear();

    if (Options_.ExternalListener) {
        Options_.ExternalListener->Stop();
    }
}

ui16 TGRpcServer::GetPort() const {
    return Options_.Port;
}

TString TGRpcServer::GetHost() const {
    return Options_.Host;
}

const TVector<TGRpcServer::IGRpcServicePtr>& TGRpcServer::GetServices() const {
    return Services_;
}

} // namespace NYdbGrpc
