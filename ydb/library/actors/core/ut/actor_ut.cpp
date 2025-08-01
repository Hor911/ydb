#include "actor.h"
#include "events.h"
#include "actorsystem.h"
#include "executor_pool_basic.h"
#include "scheduler_basic.h"
#include "actor_bootstrapped.h"
#include "actor_benchmark_helper.h"

#include <ydb/library/actors/testlib/test_runtime.h>
#include <ydb/library/actors/util/threadparkpad.h>
#include <library/cpp/testing/unittest/registar.h>

#include <util/generic/algorithm.h>
#include <library/cpp/deprecated/atomic/atomic.h>
#include <util/system/rwlock.h>
#include <util/system/hp_timer.h>

using namespace NActors;
using namespace NActors::NTests;

Y_UNIT_TEST_SUITE(ActorBenchmark) {

    using TActorBenchmark = ::NActors::NTests::TActorBenchmark<>;
    using TSettings = TActorBenchmark::TSettings;
    using TSendReceiveActorParams = TActorBenchmark::TSendReceiveActorParams;

    class TLongRunningActor : public TActorBootstrapped<TLongRunningActor> {
        public:
            TLongRunningActor(std::atomic<bool>& stop, ui64 seconds, TThreadParkPad* startPad)
                : Seconds(seconds)
                , StartPad(startPad)
                , StopFlag(stop)
            {}

            void Bootstrap() {
                Cerr << "Unpark startPad" << Endl;
                StartPad->Unpark();
                constexpr ui64 BufferSize = 10000;
                TInstant start = TActivationContext::Now();
                ui64 counter = 0;
                Buffer.resize(BufferSize);
                while (!StopFlag && TActivationContext::Now() - start < TDuration::Seconds(Seconds)) {
                    counter++;
                    ui64 sum = counter;
                    for (ui64 i = 0; i < BufferSize; i++) {
                        sum += Buffer[i];
                    }
                    Buffer[counter % BufferSize] = sum;
                }
                PassAway();
            }
        private:
            TVector<ui64> Buffer;
            ui64 Seconds;
            TThreadParkPad* StartPad;
            std::atomic<bool>& StopFlag;
    };

    Y_UNIT_TEST(GetStatisticsWithLongRunningActor) {
        THolder<TActorSystemSetup> setup =  TActorBenchmark::GetActorSystemSetup();
        TActorBenchmark::AddBasicPool(setup, 1, true, false);

        TActorSystem actorSystem(setup);
        actorSystem.Start();

        TThreadParkPad startPad;
        TThreadParkPad stopPad;
        TAtomic actorsAlive = 0;
        std::atomic<bool> stop = false;

        THolder<IActor> longRunningActor{
            new TTestEndDecorator(THolder(new TLongRunningActor(stop, 1, &startPad)), &stopPad, &actorsAlive)
        };
        actorSystem.Register(longRunningActor.Release(), TMailboxType::HTSwap, 0);

        Cerr << "Park startPad" << Endl;
        startPad.Park();

        ui64 CpuUs = 0;
        ui64 ElapsedUs = 0;
        ui64 SafeElapsedUs= 0;
        for (ui32 i = 0; i < 10; ++i) {
            NanoSleep(1'000'000);
            TVector<TExecutorThreadStats> executorThreadStats;
            TExecutorPoolStats poolStats;
            actorSystem.GetPoolStats(0, poolStats, executorThreadStats);

            ui64 newCpuUs = 0;
            ui64 newElapsedUs = 0;
            ui64 newSafeElapsedUs = 0;
            for (auto &thread : executorThreadStats) {
                newCpuUs += thread.CpuUs;
                newElapsedUs += Ts2Us(thread.ElapsedTicks);
                newSafeElapsedUs += Ts2Us(thread.SafeElapsedTicks);
            }

            Cerr << "Iteration " << i << " completed" << Endl;
            Cerr << "CpuUs: " << CpuUs << " new: " << newCpuUs << Endl;
            Cerr << "ElapsedUs: " << ElapsedUs << " new: " << newElapsedUs << Endl;
            Cerr << "SafeElapsedUs: " << SafeElapsedUs << " new: " << newSafeElapsedUs << Endl;

            UNIT_ASSERT_VALUES_UNEQUAL(CpuUs, newCpuUs);
            UNIT_ASSERT_VALUES_UNEQUAL(ElapsedUs, newElapsedUs);
            UNIT_ASSERT_VALUES_UNEQUAL(SafeElapsedUs, newSafeElapsedUs);

            CpuUs = newCpuUs;
            ElapsedUs = newElapsedUs;
            SafeElapsedUs = newSafeElapsedUs;
        }
        stop = true;

        Cerr << "Park stopPad" << Endl;
        stopPad.Park();
        actorSystem.Stop();
    }

    Y_UNIT_TEST(WithOnlyOneSharedExecutors) {
        THolder<TActorSystemSetup> setup =  TActorBenchmark::GetActorSystemSetup();
        TActorBenchmark::AddBasicPool(setup, 1, 1, true);

        TActorSystem actorSystem(setup);
        actorSystem.Start();

        TThreadParkPad pad;
        TAtomic actorsAlive = 0;
        THPTimer Timer;

        ui64 eventsPerPair = 1000;

        Timer.Reset();
        ui32 followerPoolId = 0;
        ui32 leaderPoolId = 0;
        TActorId followerId = actorSystem.Register(
            new TActorBenchmark::TSendReceiveActor(
                TSendReceiveActorParams{.OtherEvents=eventsPerPair / 2, .Allocation=true, .ToSchedule=false}
            ),
            TMailboxType::HTSwap,
            followerPoolId
        );
        THolder<IActor> leader{
            new TTestEndDecorator(THolder(new TActorBenchmark::TSendReceiveActor(
                TSendReceiveActorParams{.OwnEvents=eventsPerPair / 2, .Receivers={followerId}, .Allocation=true}
            )),
            &pad,
            &actorsAlive)
        };
        actorSystem.Register(leader.Release(), TMailboxType::HTSwap, leaderPoolId);

        pad.Park();
        auto elapsedTime = Timer.Passed() /  (TSettings::TotalEventsAmountPerThread * 4);
        actorSystem.Stop();

        TExecutorThreadStats aggregated;
        TVector<TExecutorThreadStats> stats;
        TVector<TExecutorThreadStats> sharedStats;
        TExecutorPoolStats poolStats;
        actorSystem.GetPoolStats(0, poolStats, stats, sharedStats);
        // Sum all per-thread counters into the 0th element
        for (auto &stat : stats) {
            aggregated.Aggregate(stat);
        }
        for (auto &stat : sharedStats) {
            aggregated.Aggregate(stat);
        }

        Cerr << "Completed " << 1e9 * elapsedTime << Endl;
        Cerr << "Elapsed " << Ts2Us(aggregated.ElapsedTicks) << "us" << Endl;
    }


    Y_UNIT_TEST(WithOnlyOneNotSharedExecutors) {
        THolder<TActorSystemSetup> setup =  TActorBenchmark::GetActorSystemSetup();
        TActorBenchmark::AddBasicPool(setup, 1, 1, false);

        TActorSystem actorSystem(setup);
        actorSystem.Start();

        TThreadParkPad pad;
        TAtomic actorsAlive = 0;
        THPTimer Timer;

        ui64 eventsPerPair = 1000;
        //ui64 eventsPerPair = 100000000;

        Timer.Reset();
        ui32 followerPoolId = 0;
        ui32 leaderPoolId = 0;
        TActorId followerId = actorSystem.Register(
            new TActorBenchmark::TSendReceiveActor(
                TSendReceiveActorParams{.OtherEvents=eventsPerPair / 2, .Allocation=true}
            ),
            TMailboxType::HTSwap,
            followerPoolId
        );
        THolder<IActor> leader{
            new TTestEndDecorator(THolder(new TActorBenchmark::TSendReceiveActor(
                TSendReceiveActorParams{.OwnEvents=eventsPerPair / 2, .Receivers={followerId}, .Allocation=true}
            )),
            &pad,
            &actorsAlive)
        };
        actorSystem.Register(leader.Release(), TMailboxType::HTSwap, leaderPoolId);

        pad.Park();
        auto elapsedTime = Timer.Passed() /  (TSettings::TotalEventsAmountPerThread * 4);
        actorSystem.Stop();

        TExecutorThreadStats aggregated;
        TVector<TExecutorThreadStats> stats;
        TVector<TExecutorThreadStats> sharedStats;
        TExecutorPoolStats poolStats;
        actorSystem.GetPoolStats(0, poolStats, stats, sharedStats);
        // Sum all per-thread counters into the 0th element
        for (auto &stat : stats) {
            aggregated.Aggregate(stat);
        }
        for (auto &stat : sharedStats) {
            aggregated.Aggregate(stat);
        }

        Cerr << "Completed " << 1e9 * elapsedTime << Endl;
        Cerr << "Elapsed " << Ts2Us(aggregated.ElapsedTicks) << "us" << Endl;
    }

    Y_UNIT_TEST(WithOnlyOneSharedAndOneCommonExecutors) {
        THolder<TActorSystemSetup> setup =  TActorBenchmark::GetActorSystemSetup();
        TActorBenchmark::AddBasicPool(setup, 2, true, true);

        TActorSystem actorSystem(setup);
        actorSystem.Start();

        TThreadParkPad pad;
        TAtomic actorsAlive = 0;
        THPTimer Timer;

        ui64 eventsPerPair = 1000;

        Timer.Reset();
        ui32 followerPoolId = 0;
        ui32 leaderPoolId = 0;
        for (ui32 idx = 0; idx < 50; ++idx) {
            TActorId followerId = actorSystem.Register(
                new TActorBenchmark::TSendReceiveActor(
                    TSendReceiveActorParams{.OtherEvents=eventsPerPair / 2, .Allocation=true}
                ),
                TMailboxType::HTSwap,
                followerPoolId
            );
            THolder<IActor> leader{
                new TTestEndDecorator(THolder(new TActorBenchmark::TSendReceiveActor(
                    TSendReceiveActorParams{.OwnEvents=eventsPerPair / 2, .Receivers={followerId}, .Allocation=true}
                )),
                &pad,
                &actorsAlive)
            };
            actorSystem.Register(leader.Release(), TMailboxType::HTSwap, leaderPoolId);
        }

        pad.Park();
        auto elapsedTime = Timer.Passed() /  (TSettings::TotalEventsAmountPerThread * 4);
        actorSystem.Stop();

        Cerr << "Completed " << 1e9 * elapsedTime << Endl;
    }

    Y_UNIT_TEST(WithSharedExecutors) {
        THolder<TActorSystemSetup> setup =  TActorBenchmark::GetActorSystemSetup();
         TActorBenchmark::AddBasicPool(setup, 2, 1, false);
         TActorBenchmark::AddBasicPool(setup, 2, 1, true);

        TActorSystem actorSystem(setup);
        actorSystem.Start();

        TThreadParkPad pad;
        TAtomic actorsAlive = 0;
        THPTimer Timer;

        ui64 eventsPerPair = 100000 * 4 / 60;
        //ui64 eventsPerPair = TSettings::TotalEventsAmountPerThread * 4 / 60;

        Timer.Reset();
        for (ui32 i = 0; i < 50; ++i) {
            ui32 followerPoolId = 0;
            ui32 leaderPoolId = 0;
            TActorId followerId = actorSystem.Register(
                new TActorBenchmark::TSendReceiveActor(
                    TSendReceiveActorParams{.OtherEvents=eventsPerPair / 2, .Allocation=true}
                ),
                TMailboxType::HTSwap,
                followerPoolId
            );
            THolder<IActor> leader{
                new TTestEndDecorator(
                    THolder(new TActorBenchmark::TSendReceiveActor(
                        TSendReceiveActorParams{.OwnEvents=eventsPerPair / 2, .Receivers={followerId}, .Allocation=true}
                    )),
                    &pad,
                    &actorsAlive
                )
            };
            actorSystem.Register(leader.Release(), TMailboxType::HTSwap, leaderPoolId);
        }
        for (ui32 i = 0; i < 10; ++i) {
            ui32 followerPoolId = 1;
            ui32 leaderPoolId = 1;
            TActorId followerId = actorSystem.Register(
                new TActorBenchmark::TSendReceiveActor(
                    TSendReceiveActorParams{.OtherEvents=eventsPerPair / 2, .Allocation=true}
                ),
                TMailboxType::HTSwap,
                followerPoolId
            );
            THolder<IActor> leader{
                new TTestEndDecorator(
                    THolder(new TActorBenchmark::TSendReceiveActor(
                        TSendReceiveActorParams{.OwnEvents=eventsPerPair / 2, .Receivers={followerId}, .Allocation=true}
                    )),
                    &pad,
                    &actorsAlive
                )
            };
            actorSystem.Register(leader.Release(), TMailboxType::HTSwap, leaderPoolId);
        }

        pad.Park();
        auto elapsedTime = Timer.Passed() /  (4 * TSettings::TotalEventsAmountPerThread);
        actorSystem.Stop();

        TExecutorThreadStats aggregated;
        TVector<TExecutorThreadStats> stats;
        TVector<TExecutorThreadStats> sharedStats;
        TExecutorPoolStats poolStats;
        actorSystem.GetPoolStats(0, poolStats, stats, sharedStats);
        // Sum all per-thread counters into the 0th element
        for (auto &stat : stats) {
            aggregated.Aggregate(stat);
        }
        for (auto &stat : sharedStats) {
            aggregated.Aggregate(stat);
        }

        Cerr << "Completed " << 1e9 * elapsedTime << Endl;
        Cerr << "Elapsed " << Ts2Us(aggregated.ElapsedTicks) << "us" << Endl;
    }

    Y_UNIT_TEST(WithoutSharedExecutors) {
        THolder<TActorSystemSetup> setup =  TActorBenchmark::GetActorSystemSetup();
        TActorBenchmark::AddBasicPool(setup, 2, 1, 0);
        TActorBenchmark::AddBasicPool(setup, 2, 1, 0);

        TActorSystem actorSystem(setup);
        actorSystem.Start();

        TThreadParkPad pad;
        TAtomic actorsAlive = 0;
        THPTimer Timer;

        ui64 eventsPerPair = 100000 * 4 / 60;
        //ui64 eventsPerPair = TSettings::TotalEventsAmountPerThread * 4 / 60;

        Timer.Reset();
        for (ui32 i = 0; i < 50; ++i) {
            ui32 followerPoolId = 0;
            ui32 leaderPoolId = 0;
            TActorId followerId = actorSystem.Register(
                new TActorBenchmark::TSendReceiveActor(
                    TSendReceiveActorParams{.OtherEvents=eventsPerPair / 2, .Allocation=true}
                ),
                TMailboxType::HTSwap,
                followerPoolId
            );
            THolder<IActor> leader{
                new TTestEndDecorator(
                    THolder(new TActorBenchmark::TSendReceiveActor(
                        TSendReceiveActorParams{.OwnEvents=eventsPerPair / 2, .Receivers={followerId}, .Allocation=true}
                    )),
                    &pad,
                    &actorsAlive
                )
            };
            actorSystem.Register(leader.Release(), TMailboxType::HTSwap, leaderPoolId);
        }
        for (ui32 i = 0; i < 10; ++i) {
            ui32 followerPoolId = 1;
            ui32 leaderPoolId = 1;
            TActorId followerId = actorSystem.Register(
                new TActorBenchmark::TSendReceiveActor(
                    TSendReceiveActorParams{.OtherEvents=eventsPerPair / 2, .Allocation=true}
                ),
                TMailboxType::HTSwap,
                followerPoolId
            );
            THolder<IActor> leader{
                new TTestEndDecorator(
                    THolder(new TActorBenchmark::TSendReceiveActor(
                        TSendReceiveActorParams{.OwnEvents=eventsPerPair / 2, .Receivers={followerId}, .Allocation=true}
                    )),
                    &pad,
                    &actorsAlive
                )
            };
            actorSystem.Register(leader.Release(), TMailboxType::HTSwap, leaderPoolId);
        }

        pad.Park();
        auto elapsedTime = Timer.Passed() /  (4 * TSettings::TotalEventsAmountPerThread);
        actorSystem.Stop();

        TExecutorThreadStats aggregated;
        TVector<TExecutorThreadStats> stats;
        TVector<TExecutorThreadStats> sharedStats;
        TExecutorPoolStats poolStats;
        actorSystem.GetPoolStats(0, poolStats, stats, sharedStats);
        // Sum all per-thread counters into the 0th element
        for (auto &stat : stats) {
            aggregated.Aggregate(stat);
        }
        for (auto &stat : sharedStats) {
            aggregated.Aggregate(stat);
        }

        Cerr << "Completed " << 1e9 * elapsedTime << Endl;
        Cerr << "Elapsed " << Ts2Us(aggregated.ElapsedTicks) << "us" << Endl;
    }

    Y_UNIT_TEST(SendReceive1Pool1ThreadAlloc) {
        for (const auto& mType : TSettings::MailboxTypes) {
            auto stats =  TActorBenchmark::CountStats([mType] {
                return  TActorBenchmark::BenchSendReceive(true, mType, TActorBenchmark::EPoolType::Basic, ESendingType::Common);
            });
            Cerr << stats.ToString() << " " << mType << Endl;
            stats =  TActorBenchmark::CountStats([mType] {
                return  TActorBenchmark::BenchSendReceive(true, mType, TActorBenchmark::EPoolType::Basic, ESendingType::Lazy);
            });
            Cerr << stats.ToString() << " " << mType << " Lazy" << Endl;
            stats =  TActorBenchmark::CountStats([mType] {
                return  TActorBenchmark::BenchSendReceive(true, mType, TActorBenchmark::EPoolType::Basic, ESendingType::Tail);
            });
            Cerr << stats.ToString() << " " << mType << " Tail" << Endl;
        }
    }

    Y_UNIT_TEST(SendReceive1Pool1ThreadNoAlloc) {
        for (const auto& mType : TSettings::MailboxTypes) {
            auto stats =  TActorBenchmark::CountStats([mType] {
                return  TActorBenchmark::BenchSendReceive(false, mType, TActorBenchmark::EPoolType::Basic, ESendingType::Common);
            });
            Cerr << stats.ToString() << " " << mType << Endl;
            stats =  TActorBenchmark::CountStats([mType] {
                return  TActorBenchmark::BenchSendReceive(false, mType, TActorBenchmark::EPoolType::Basic, ESendingType::Lazy);
            });
            Cerr << stats.ToString() << " " << mType << " Lazy" << Endl;
            stats =  TActorBenchmark::CountStats([mType] {
                return  TActorBenchmark::BenchSendReceive(false, mType, TActorBenchmark::EPoolType::Basic, ESendingType::Tail);
            });
            Cerr << stats.ToString() << " " << mType << " Tail" << Endl;
        }
    }

    Y_UNIT_TEST(SendActivateReceive1Pool1ThreadAlloc) {
         TActorBenchmark::RunBenchSendActivateReceive(1, 1, true, TActorBenchmark::EPoolType::Basic);
    }

    Y_UNIT_TEST(SendActivateReceive1Pool1ThreadNoAlloc) {
         TActorBenchmark::RunBenchSendActivateReceive(1, 1, false, TActorBenchmark::EPoolType::Basic);
    }

    Y_UNIT_TEST(SendActivateReceive1Pool2ThreadsAlloc) {
         TActorBenchmark::RunBenchSendActivateReceive(1, 2, true, TActorBenchmark::EPoolType::Basic);
    }

    Y_UNIT_TEST(SendActivateReceive1Pool2ThreadsNoAlloc) {
         TActorBenchmark::RunBenchSendActivateReceive(1, 2, false, TActorBenchmark::EPoolType::Basic);
    }

    Y_UNIT_TEST(SendActivateReceive2Pool1ThreadAlloc) {
         TActorBenchmark::RunBenchSendActivateReceive(2, 1, true, TActorBenchmark::EPoolType::Basic);
    }

    Y_UNIT_TEST(SendActivateReceive2Pool1ThreadNoAlloc) {
         TActorBenchmark::RunBenchSendActivateReceive(2, 1, false, TActorBenchmark::EPoolType::Basic);
    }

    Y_UNIT_TEST(SendActivateReceive1Pool1Threads)       {  TActorBenchmark::RunBenchContentedThreads(1, TActorBenchmark::EPoolType::Basic);  }
    Y_UNIT_TEST(SendActivateReceive1Pool2Threads)       {  TActorBenchmark::RunBenchContentedThreads(2, TActorBenchmark::EPoolType::Basic);  }
    Y_UNIT_TEST(SendActivateReceive1Pool3Threads)       {  TActorBenchmark::RunBenchContentedThreads(3, TActorBenchmark::EPoolType::Basic);  }
    Y_UNIT_TEST(SendActivateReceive1Pool4Threads)       {  TActorBenchmark::RunBenchContentedThreads(4, TActorBenchmark::EPoolType::Basic);  }
    Y_UNIT_TEST(SendActivateReceive1Pool5Threads)       {  TActorBenchmark::RunBenchContentedThreads(5, TActorBenchmark::EPoolType::Basic);  }
    Y_UNIT_TEST(SendActivateReceive1Pool6Threads)       {  TActorBenchmark::RunBenchContentedThreads(6, TActorBenchmark::EPoolType::Basic);  }
    Y_UNIT_TEST(SendActivateReceive1Pool7Threads)       {  TActorBenchmark::RunBenchContentedThreads(7, TActorBenchmark::EPoolType::Basic);  }
    Y_UNIT_TEST(SendActivateReceive1Pool8Threads)       {  TActorBenchmark::RunBenchContentedThreads(8, TActorBenchmark::EPoolType::Basic);  }

    Y_UNIT_TEST(SendActivateReceiveCSV) {
        std::vector<ui32> threadsList;
        for (ui32 threads = 1; threads <= 32; threads *= 2) {
            threadsList.push_back(threads);
        }
        std::vector<ui32> actorPairsList;
        for (ui32 actorPairs = 1; actorPairs <= 2 * 32; actorPairs *= 2) {
            actorPairsList.push_back(actorPairs);
        }
        TActorBenchmark::RunSendActivateReceiveCSV(threadsList, actorPairsList, {1}, TDuration::MilliSeconds(100));
    }

    Y_UNIT_TEST(SendActivateReceiveWithMailboxNeighbours) {
        TVector<ui32> NeighbourActors = {0, 1, 2, 3, 4, 5, 6, 7, 8, 16, 32, 64, 128, 256};
        for (const auto& neighbour : NeighbourActors) {
            auto stats =  TActorBenchmark::CountStats([neighbour] {
                return  TActorBenchmark::BenchSendActivateReceiveWithMailboxNeighbours(neighbour, TActorBenchmark::EPoolType::Basic, ESendingType::Common);
            });
            Cerr << stats.ToString() << " neighbourActors: " << neighbour << Endl;
            stats =  TActorBenchmark::CountStats([neighbour] {
                return  TActorBenchmark::BenchSendActivateReceiveWithMailboxNeighbours(neighbour, TActorBenchmark::EPoolType::Basic, ESendingType::Lazy);
            });
            Cerr << stats.ToString() << " neighbourActors: " << neighbour << " Lazy" << Endl;
            stats =  TActorBenchmark::CountStats([neighbour] {
                return  TActorBenchmark::BenchSendActivateReceiveWithMailboxNeighbours(neighbour, TActorBenchmark::EPoolType::Basic, ESendingType::Tail);
            });
            Cerr << stats.ToString() << " neighbourActors: " << neighbour << " Tail" << Endl;
        }
    }
}

Y_UNIT_TEST_SUITE(TestDecorator) {
    struct TPingDecorator : TDecorator {
        TAutoPtr<IEventHandle> SavedEvent = nullptr;
        ui64* Counter;

        TPingDecorator(THolder<IActor>&& actor, ui64* counter)
            : TDecorator(std::move(actor))
            , Counter(counter)
        {
        }

        bool DoBeforeReceiving(TAutoPtr<IEventHandle>& ev, const TActorContext&) override {
            *Counter += 1;
            if (ev->Type != TEvents::THelloWorld::Pong) {
                TAutoPtr<IEventHandle> pingEv = new IEventHandle(SelfId(), SelfId(), new TEvents::TEvPing());
                SavedEvent = ev;
                Actor->Receive(pingEv);
            } else {
                Actor->Receive(SavedEvent);
            }
            return false;
        }
    };

    struct TPongDecorator : TDecorator {
        ui64* Counter;

        TPongDecorator(THolder<IActor>&& actor, ui64* counter)
            : TDecorator(std::move(actor))
            , Counter(counter)
        {
        }

        bool DoBeforeReceiving(TAutoPtr<IEventHandle>& ev, const TActorContext&) override {
            *Counter += 1;
            if (ev->Type == TEvents::THelloWorld::Ping) {
                TAutoPtr<IEventHandle> pongEv = new IEventHandle(SelfId(), SelfId(), new TEvents::TEvPong());
                Send(SelfId(), new TEvents::TEvPong());
                return false;
            }
            return true;
        }
    };

    struct TTestActor : TActorBootstrapped<TTestActor> {
        static constexpr char ActorName[] = "TestActor";

        void Bootstrap()
        {
            auto activityTypeIndex = GetActivityType().GetIndex();
            Y_ENSURE(activityTypeIndex < GetActivityTypeCount());
            Y_ENSURE(GetActivityTypeName(activityTypeIndex) == "TestActor");
            PassAway();
        }
    };

    Y_UNIT_TEST(Basic) {
        THolder<TActorSystemSetup> setup = MakeHolder<TActorSystemSetup>();
        setup->NodeId = 0;
        setup->ExecutorsCount = 1;
        setup->Executors.Reset(new TAutoPtr<IExecutorPool>[setup->ExecutorsCount]);

        ui64 ts = GetCycleCountFast();
        std::unique_ptr<IHarmonizer> harmonizer = MakeHarmonizer(ts);
        for (ui32 i = 0; i < setup->ExecutorsCount; ++i) {
            setup->Executors[i] = new TBasicExecutorPool(i, 1, 10, "basic", harmonizer.get());
            harmonizer->AddPool(setup->Executors[i].Get());
        }
        setup->Scheduler = new TBasicSchedulerThread;

        TActorSystem actorSystem(setup);
        actorSystem.Start();

        THolder<IActor> innerActor = MakeHolder<TTestActor>();
        ui64 pongCounter = 0;
        THolder<IActor> pongActor = MakeHolder<TPongDecorator>(std::move(innerActor), &pongCounter);
        ui64 pingCounter = 0;
        THolder<IActor> pingActor = MakeHolder<TPingDecorator>(std::move(pongActor), &pingCounter);

        TThreadParkPad pad;
        TAtomic actorsAlive = 0;

        THolder<IActor> endActor = MakeHolder<TTestEndDecorator>(std::move(pingActor), &pad, &actorsAlive);
        actorSystem.Register(endActor.Release(), TMailboxType::HTSwap);

        pad.Park();
        actorSystem.Stop();
        UNIT_ASSERT(pongCounter == 2 && pingCounter == 2);
    }

    Y_UNIT_TEST(LocalProcessKey) {
        static constexpr char ActorName[] = "TestActor";

        UNIT_ASSERT((TEnumProcessKey<TActorActivityTag, IActor::EActorActivity>::GetName(IActor::EActivityType::INTERCONNECT_PROXY_TCP) == "INTERCONNECT_PROXY_TCP"));
        UNIT_ASSERT((TLocalProcessKey<TActorActivityTag, ActorName>::GetName() == ActorName));
    }
}

Y_UNIT_TEST_SUITE(TestAliases) {
    using namespace NThreading;

    struct TSendPingActor : TActorBootstrapped<TSendPingActor> {
        const TActorId Target;
        const ui64 Cookie;
        const TActorId NotifyTo;

        TSendPingActor(const TActorId& target, ui64 cookie, const TActorId& notifyTo)
            : Target(target)
            , Cookie(cookie)
            , NotifyTo(notifyTo)
        {}

        void Bootstrap() {
            Cerr << "Actor " << SelfId() << " sending TEvPing to " << Target << Endl;
            Send(Target, new TEvents::TEvPing, IEventHandle::FlagTrackDelivery, Cookie);
            Become(&TThis::StateWork);
        }

        STFUNC(StateWork) {
            switch (ev->GetTypeRewrite()) {
                hFunc(TEvents::TEvPong, Handle);
                hFunc(TEvents::TEvUndelivered, Handle);
            }
        }

        void Handle(TEvents::TEvPong::TPtr& ev) {
            Cerr << "Actor " << SelfId() << " received TEvPong from " << ev->Sender << Endl;
            Send(NotifyTo, new TEvents::TEvGone, 0, Cookie);
            PassAway();
        }

        void Handle(TEvents::TEvUndelivered::TPtr& ev) {
            Cerr << "Actor " << SelfId() << " received TEvUndelivered from " << ev->Sender << Endl;
            Y_ABORT_UNLESS(ev->Sender == Target);
            Send(NotifyTo, new TEvents::TEvGone, 0, Cookie);
            PassAway();
        }
    };

    struct TTestActor : TActorBootstrapped<TTestActor> {
        TPromise<void> Done;
        TActorId MyAlias;
        std::set<ui64> ReceivedPing;
        std::set<ui64> ReceivedGone;

        TTestActor(TPromise<void> done)
            : Done(std::move(done))
        {}

        void Bootstrap() {
            MyAlias = RegisterAlias();
            Cerr << "Actor " << SelfId() << " registered alias " << MyAlias << Endl;
            Register(new TSendPingActor(MyAlias, 1, SelfId()));
            Register(new TSendPingActor(MyAlias, 2, SelfId()));
            Become(&TThis::StateWork);
        }

        STFUNC(StateWork) {
            switch (ev->GetTypeRewrite()) {
                hFunc(TEvents::TEvPing, Handle);
                hFunc(TEvents::TEvGone, Handle);
            }
        }

        void Handle(TEvents::TEvPing::TPtr& ev) {
            Cerr << "Actor " << SelfId() << " received TEvPing from " << ev->Sender << Endl;
            Y_ABORT_UNLESS(ev->Recipient == MyAlias);
            Y_ABORT_UNLESS(ev->GetRecipientRewrite() == SelfId());
            Y_ABORT_UNLESS(TActivationContext::AsActorContext().SelfID == SelfId());

            Y_ABORT_UNLESS(ReceivedPing.empty());
            ReceivedPing.insert(ev->Cookie);

            Send(ev->Sender, new TEvents::TEvPong, 0, ev->Cookie);
            UnregisterAlias(MyAlias);
        }

        void Handle(TEvents::TEvGone::TPtr& ev) {
            Cerr << "Actor " << SelfId() << " received TEvGone from " << ev->Sender << Endl;
            ReceivedGone.insert(ev->Cookie);
            if (ReceivedGone.size() >= 2) {
                Y_ABORT_UNLESS(ReceivedPing.size() == 1);
                Done.SetValue();
                PassAway();
            }
        }
    };

    Y_UNIT_TEST(AliasEventDelivery) {
        THolder<TActorSystemSetup> setup = MakeHolder<TActorSystemSetup>();
        setup->NodeId = 1;
        setup->ExecutorsCount = 1;
        setup->Executors.Reset(new TAutoPtr<IExecutorPool>[setup->ExecutorsCount]);

        ui64 ts = GetCycleCountFast();
        std::unique_ptr<IHarmonizer> harmonizer = MakeHarmonizer(ts);
        for (ui32 i = 0; i < setup->ExecutorsCount; ++i) {
            setup->Executors[i] = new TBasicExecutorPool(i, 1, 10, "basic", harmonizer.get());
            harmonizer->AddPool(setup->Executors[i].Get());
        }
        setup->Scheduler = new TBasicSchedulerThread;

        TActorSystem actorSystem(setup);
        actorSystem.Start();

        TPromise<void> donePromise = NewPromise<void>();
        TFuture<void> done = donePromise.GetFuture();
        actorSystem.Register(new TTestActor(std::move(donePromise)));

        done.GetValueSync();
        actorSystem.Stop();
    }

    Y_UNIT_TEST(AliasEventDeliveryInTestRuntime) {
        TTestActorRuntimeBase runtime;
        runtime.Initialize();

        TPromise<void> donePromise = NewPromise<void>();
        TFuture<void> done = donePromise.GetFuture();
        runtime.Register(new TTestActor(std::move(donePromise)));

        TDispatchOptions options;
        options.CustomFinalCondition = [&]() {
            return done.HasValue();
        };
        options.FinalEvents.emplace_back([](IEventHandle&) { return false; });
        runtime.DispatchEvents(options);

        done.GetValueSync();
    }
}

Y_UNIT_TEST_SUITE(TestStateFunc) {
    struct TTestActorWithExceptionsStateFunc : TActor<TTestActorWithExceptionsStateFunc> {
        static constexpr char ActorName[] = "TestActorWithExceptionsStateFunc";

        TTestActorWithExceptionsStateFunc()
            : TActor<TTestActorWithExceptionsStateFunc>(&TTestActorWithExceptionsStateFunc::StateFunc)
        {
        }

        STRICT_STFUNC_EXC(StateFunc,
            hFunc(TEvents::TEvWakeup, Handle),
            ExceptionFunc(yexception, HandleException)
            ExceptionFuncEv(std::exception, HandleException)
            AnyExceptionFunc(HandleException)
        )

        void Handle(TEvents::TEvWakeup::TPtr& ev) {
            Owner = ev->Sender;
            switch (ev->Get()->Tag) {
            case ETag::NoException:
                SendResponse(ETag::NoException);
                break;
            case ETag::YException:
                Cerr << "Throw yexception" << Endl;
                throw yexception();
            case ETag::StdException:
                Cerr << "Throw std::exception" << Endl;
                throw std::runtime_error("trololo");
            case ETag::OtherException:
                Cerr << "Throw trash" << Endl;
                throw TString("1");
            default:
                UNIT_ASSERT(false);
            }
        }

        void HandleException(const yexception&) {
            Cerr << "Handle yexception" << Endl;
            SendResponse(ETag::YException);
        }

        void HandleException(const std::exception&, TAutoPtr<::NActors::IEventHandle>& ev) {
            Cerr << "Handle std::exception from event with type " << ev->Type << Endl;
            SendResponse(ETag::StdException);
        }

        void HandleException() {
            Cerr << "Handle trash" << Endl;
            SendResponse(ETag::OtherException);
        }

        enum ETag : ui64 {
            NoException,
            YException,
            StdException,
            OtherException,
        };

        void SendResponse(ETag tag) {
            Send(Owner, new TEvents::TEvWakeup(tag));
        }

        TActorId Owner;
    };

    Y_UNIT_TEST(StateFuncWithExceptions) {
        TTestActorRuntimeBase runtime;
        runtime.Initialize();
        auto sender = runtime.AllocateEdgeActor();
        auto testActor = runtime.Register(new TTestActorWithExceptionsStateFunc());
        for (ui64 tag = 0; tag < 4; ++tag) {
            runtime.Send(new IEventHandle(testActor, sender, new TEvents::TEvWakeup(tag)), 0, true);
            auto ev = runtime.GrabEdgeEventRethrow<TEvents::TEvWakeup>(sender);
            UNIT_ASSERT_VALUES_EQUAL(ev->Get()->Tag, tag);
        }
    }
}
