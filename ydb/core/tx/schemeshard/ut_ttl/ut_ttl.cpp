#include <ydb/core/kqp/ut/common/kqp_ut_common.h>
#include <ydb/core/tx/datashard/datashard.h>
#include <ydb/core/tx/schemeshard/schemeshard_private.h>
#include <ydb/core/tx/schemeshard/ut_helpers/helpers.h>

using namespace NKikimr;
using namespace NSchemeShard;
using namespace NSchemeShardUT_Private;

namespace {

template <typename TTtlSettings>
void CheckTtlSettings(const TTtlSettings& ttl, const char* ttlColumnName, bool legacyTiering = false) {
    UNIT_ASSERT(ttl.HasEnabled());
    UNIT_ASSERT_VALUES_EQUAL(ttl.GetEnabled().GetColumnName(), ttlColumnName);
    UNIT_ASSERT_VALUES_EQUAL(ttl.GetEnabled().GetExpireAfterSeconds(), 3600);
    if (legacyTiering) {
        UNIT_ASSERT_VALUES_EQUAL(ttl.GetEnabled().TiersSize(), 0);
    } else {
        UNIT_ASSERT_VALUES_EQUAL(ttl.GetEnabled().TiersSize(), 1);
        UNIT_ASSERT(ttl.GetEnabled().GetTiers(0).HasDelete());
        UNIT_ASSERT_VALUES_EQUAL(ttl.GetEnabled().GetTiers(0).GetApplyAfterSeconds(), 3600);
    }
}

void LegacyOltpTtlChecker(const NKikimrScheme::TEvDescribeSchemeResult& record) {
    CheckTtlSettings(record.GetPathDescription().GetTable().GetTTLSettings(), "modified_at", true);
}

void OltpTtlChecker(const NKikimrScheme::TEvDescribeSchemeResult& record) {
    CheckTtlSettings(record.GetPathDescription().GetTable().GetTTLSettings(), "modified_at");
}

NLs::TCheckFunc OlapTtlChecker(const char* ttlColumnName = "modified_at") {
    return [=](const NKikimrScheme::TEvDescribeSchemeResult& record) {
        CheckTtlSettings(record.GetPathDescription().GetColumnTableDescription().GetTtlSettings(), ttlColumnName);
    };
}

void CheckTtlSettings(TTestActorRuntime& runtime, NLs::TCheckFunc func, const char* tableName = "TTLEnabledTable") {
    TestDescribeResult(
        DescribePath(runtime, Sprintf("/MyRoot/%s", tableName)), {
            NLs::PathExist,
            NLs::Finished,
            func,
        }
    );
}

}

Y_UNIT_TEST_SUITE(TSchemeShardTTLTests) {
    void CreateTableShouldSucceed(const char* name, const char* ttlColumnType, bool enableTablePgTypes, const char* unit = "UNIT_AUTO") {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().EnableTablePgTypes(enableTablePgTypes));
        ui64 txId = 100;

        TestCreateTable(runtime, ++txId, "/MyRoot", Sprintf(R"(
            Name: "%s"
            Columns { Name: "key" Type: "Uint64" }
            Columns { Name: "modified_at" Type: "%s" }
            KeyColumnNames: ["key"]
            TTLSettings {
              Enabled {
                ColumnName: "modified_at"
                ExpireAfterSeconds: 3600
                ColumnUnit: %s
                Tiers: {
                  ApplyAfterSeconds: 3600
                  Delete: {}
                }
              }
            }
        )", name, ttlColumnType, unit));
        env.TestWaitNotification(runtime, txId);
        CheckTtlSettings(runtime, OltpTtlChecker, name);
    }

    Y_UNIT_TEST_FLAG(CreateTableShouldSucceed, EnableTablePgTypes) {
        const auto& datetimeTypes = EnableTablePgTypes
            ? TVector<const char*>{"Date", "Datetime", "Timestamp", "Date32", "Datetime64", "Timestamp64", "pgdate", "pgtimestamp"}
            : TVector<const char*>{"Date", "Datetime", "Timestamp", "Date32", "Datetime64", "Timestamp64"};
        for (const auto& ct : datetimeTypes) {
            CreateTableShouldSucceed(Sprintf("TTLTableWith%sColumn", ct).data(), ct, EnableTablePgTypes);
        }

        const auto& intTypes = EnableTablePgTypes
            ? TVector<const char*>{"Uint32", "Uint64", "DyNumber", "pgint4", "pgint8"}
            : TVector<const char*>{"Uint32", "Uint64", "DyNumber"};
        for (const auto& ct : intTypes) {
            for (auto unit : {"UNIT_SECONDS", "UNIT_MILLISECONDS", "UNIT_MICROSECONDS", "UNIT_NANOSECONDS"}) {
                CreateTableShouldSucceed(Sprintf("TTLTableWith%sColumn_%s", ct, unit).data(), ct, EnableTablePgTypes, unit);
            }
        }
    }

    Y_UNIT_TEST(CreateTableShouldFailOnUnknownColumn) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "TTLEnabledTable"
            Columns { Name: "key" Type: "Uint64" }
            Columns { Name: "modified_at" Type: "Timestamp" }
            KeyColumnNames: ["key"]
            TTLSettings {
              Enabled {
                ColumnName: "created_at"
              }
            }
        )", {{NKikimrScheme::StatusSchemeError, "Cannot enable TTL on unknown column: 'created_at'"}});
    }

    Y_UNIT_TEST(CreateTableShouldFailOnWrongColumnType) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "TTLEnabledTable"
            Columns { Name: "key" Type: "Uint64" }
            Columns { Name: "modified_at" Type: "String" }
            KeyColumnNames: ["key"]
            TTLSettings {
              Enabled {
                ColumnName: "modified_at"
              }
            }
        )", {{NKikimrScheme::StatusSchemeError, "Unsupported column type"}});
    }

    void CreateTableShouldFailOnWrongUnit(const char* ttlColumnType, bool enableTablePgTypes, const char* unit) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().EnableTablePgTypes(enableTablePgTypes));
        ui64 txId = 100;

        TestCreateTable(runtime, ++txId, "/MyRoot",
            Sprintf(R"(
            Name: "TTLEnabledTable"
            Columns { Name: "key" Type: "Uint64" }
            Columns { Name: "modified_at" Type: "%s" }
            KeyColumnNames: ["key"]
            TTLSettings {
              Enabled {
                ColumnName: "modified_at"
                ColumnUnit: %s
              }
            }
        )",
        ttlColumnType, unit), {
            { NKikimrScheme::StatusSchemeError, "To enable TTL on date PG type column 'DateTypeColumnModeSettings' should be specified" },
            { NKikimrScheme::StatusSchemeError, "To enable TTL on date type column 'DateTypeColumnModeSettings' should be specified" },
            { NKikimrScheme::StatusSchemeError, "To enable TTL on integral type column 'ValueSinceUnixEpochModeSettings' should be specified" },
            { NKikimrScheme::StatusSchemeError, "To enable TTL on integral PG type column 'ValueSinceUnixEpochModeSettings' should be specified" }
        });
    }

    Y_UNIT_TEST_FLAG(CreateTableShouldFailOnWrongUnit, EnableTablePgTypes) {
        const auto& datetimeTypes = EnableTablePgTypes
            ? TVector<const char*>{"Date", "Datetime", "Timestamp", "Date32", "Datetime64", "Timestamp64", "pgdate", "pgtimestamp"}
            : TVector<const char*>{"Date", "Datetime", "Timestamp", "Date32", "Datetime64", "Timestamp64"};
        for (auto ct : datetimeTypes) {
            for (auto unit : {"UNIT_SECONDS", "UNIT_MILLISECONDS", "UNIT_MICROSECONDS", "UNIT_NANOSECONDS"}) {
                CreateTableShouldFailOnWrongUnit(ct, EnableTablePgTypes, unit);
            }
        }

        const auto& intTypes = EnableTablePgTypes
            ? TVector<const char*>{"Uint32", "Uint64", "DyNumber", "pgint4", "pgint8"}
            : TVector<const char*>{"Uint32", "Uint64", "DyNumber"};
        for (auto ct : intTypes) {
            CreateTableShouldFailOnWrongUnit(ct, EnableTablePgTypes, "UNIT_AUTO");
        }
    }

    Y_UNIT_TEST(CreateTableShouldFailOnUnspecifiedTTL) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "TTLEnabledTable"
            Columns { Name: "key" Type: "Uint64" }
            Columns { Name: "modified_at" Type: "Timestamp" }
            KeyColumnNames: ["key"]
            TTLSettings {
            }
        )", {{NKikimrScheme::StatusSchemeError, "TTL status must be specified"}});
    }

    Y_UNIT_TEST(CreateTableShouldFailOnBeforeEpochTTL) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        // An attempt to create 100-year TTL.
        // The TTL behaviour is undefined before 1970,
        // so it's forbidden.

        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "TTLEnabledTable"
            Columns { Name: "key" Type: "Uint64" }
            Columns { Name: "modified_at" Type: "Timestamp" }
            KeyColumnNames: ["key"]
            TTLSettings {
              Enabled {
                ColumnName: "modified_at"
                ExpireAfterSeconds: 3153600000
                Tiers: {
                  ApplyAfterSeconds: 3153600000
                  Delete: {}
                }
              }
            }
        )", {{NKikimrScheme::StatusSchemeError, "TTL should be less than"}});
    }    

    void CreateTableOnIndexedTable(NKikimrSchemeOp::EIndexType indexType) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TestCreateIndexedTable(runtime, ++txId, "/MyRoot", Sprintf(R"(
            TableDescription {
              Name: "TTLEnabledTable"
              Columns { Name: "key" Type: "Uint64" }
              Columns { Name: "modified_at" Type: "Timestamp" }
              KeyColumnNames: ["key"]
              TTLSettings {
                Enabled {
                  ColumnName: "modified_at"
                  ExpireAfterSeconds: 3600
                  Tiers: {
                    ApplyAfterSeconds: 3600
                    Delete: {}
                  }
                }
              }
            }
            IndexDescription {
              Name: "UserDefinedIndexByExpireAt"
              KeyColumnNames: ["modified_at"]
              Type: %s
            }
        )", NKikimrSchemeOp::EIndexType_Name(indexType).c_str()));

        env.TestWaitNotification(runtime, txId);
        CheckTtlSettings(runtime, OltpTtlChecker);
    }

    Y_UNIT_TEST(CreateTableShouldSucceedOnIndexedTable) {
        CreateTableOnIndexedTable(NKikimrSchemeOp::EIndexTypeGlobal);
    }

    Y_UNIT_TEST(CreateTableShouldSucceedAsyncOnIndexedTable) {
        CreateTableOnIndexedTable(NKikimrSchemeOp::EIndexTypeGlobalAsync);
    }

    Y_UNIT_TEST(AlterTableShouldSuccess) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "TTLEnabledTable"
            Columns { Name: "key" Type: "Uint64" }
            Columns { Name: "modified_at" Type: "Timestamp" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        TestAlterTable(runtime, ++txId, "/MyRoot", R"(
            Name: "TTLEnabledTable"
            TTLSettings {
              Enabled {
                ColumnName: "modified_at"
                ExpireAfterSeconds: 3600
                Tiers: {
                  ApplyAfterSeconds: 3600
                  Delete: {}
                }
              }
            }
        )");
        env.TestWaitNotification(runtime, txId);
        CheckTtlSettings(runtime, OltpTtlChecker);

        TestAlterTable(runtime, ++txId, "/MyRoot", R"(
            Name: "TTLEnabledTable"
            DropColumns { Name: "modified_at" }
        )", {{NKikimrScheme::StatusInvalidParameter, "Can't drop TTL column: 'modified_at', disable TTL first"}});

        TestAlterTable(runtime, ++txId, "/MyRoot", R"(
            Name: "TTLEnabledTable"
            TTLSettings {
              Disabled {
              }
            }
        )");
        env.TestWaitNotification(runtime, txId);
        TestDescribeResult(
            DescribePath(runtime, "/MyRoot/TTLEnabledTable"), {
                NLs::PathExist,
                NLs::Finished, [=] (const NKikimrScheme::TEvDescribeSchemeResult& record) {
                    const auto& table = record.GetPathDescription().GetTable();
                    UNIT_ASSERT(table.HasTTLSettings());
                    UNIT_ASSERT(table.GetTTLSettings().HasDisabled());
                }
            }
        );
    }

    Y_UNIT_TEST(AlterTableShouldSuccessOnSimultaneousAddColumnAndEnableTTL) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "TTLEnabledTable"
            Columns { Name: "key" Type: "Uint64" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        TestAlterTable(runtime, ++txId, "/MyRoot", R"(
            Name: "TTLEnabledTable"
            Columns { Name: "modified_at" Type: "Timestamp" }
            TTLSettings {
              Enabled {
                ColumnName: "modified_at"
                ExpireAfterSeconds: 3600
                Tiers: {
                  ApplyAfterSeconds: 3600
                  Delete: {}
                }
              }
            }
        )");
        env.TestWaitNotification(runtime, txId);
        CheckTtlSettings(runtime, OltpTtlChecker);
    }

    Y_UNIT_TEST(AlterTableShouldFailOnSimultaneousDropColumnAndEnableTTL) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "TTLEnabledTable"
            Columns { Name: "key" Type: "Uint64" }
            Columns { Name: "modified_at" Type: "Timestamp" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        TestAlterTable(runtime, ++txId, "/MyRoot", R"(
            Name: "TTLEnabledTable"
            DropColumns { Name: "modified_at" }
            TTLSettings {
              Enabled {
                ColumnName: "modified_at"
              }
            }
        )", {{NKikimrScheme::StatusInvalidParameter, "Cannot enable TTL on dropped column: 'modified_at'"}});
    }

    void AlterTableOnIndexedTable(NKikimrSchemeOp::EIndexType indexType) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TestCreateIndexedTable(runtime, ++txId, "/MyRoot", Sprintf(R"(
            TableDescription {
              Name: "TTLEnabledTable"
              Columns { Name: "key" Type: "Uint64" }
              Columns { Name: "modified_at" Type: "Timestamp" }
              KeyColumnNames: ["key"]
            }
            IndexDescription {
              Name: "UserDefinedIndexByExpireAt"
              KeyColumnNames: ["modified_at"]
              Type: %s
            }
        )", NKikimrSchemeOp::EIndexType_Name(indexType).c_str()));
        env.TestWaitNotification(runtime, txId);

        TestAlterTable(runtime, ++txId, "/MyRoot", R"(
            Name: "TTLEnabledTable"
            TTLSettings {
              Enabled {
                ColumnName: "modified_at"
                ExpireAfterSeconds: 3600
                Tiers: {
                  ApplyAfterSeconds: 3600
                  Delete: {}
                }
              }
            }
        )");

        env.TestWaitNotification(runtime, txId);
        CheckTtlSettings(runtime, OltpTtlChecker);
    }

    Y_UNIT_TEST(AlterTableShouldSucceedOnIndexedTable) {
        AlterTableOnIndexedTable(NKikimrSchemeOp::EIndexTypeGlobal);
    }

    Y_UNIT_TEST(AlterTableShouldSucceedOnAsyncIndexedTable) {
        AlterTableOnIndexedTable(NKikimrSchemeOp::EIndexTypeGlobalAsync);
    }

    void BuildIndex(NKikimrSchemeOp::EIndexType indexType) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "TTLEnabledTable"
            Columns { Name: "key" Type: "Uint64" }
            Columns { Name: "value" Type: "Uint64" }
            Columns { Name: "modified_at" Type: "Timestamp" }
            KeyColumnNames: ["key"]
            TTLSettings {
              Enabled {
                ColumnName: "modified_at"
                ExpireAfterSeconds: 3600
                Tiers: {
                  ApplyAfterSeconds: 3600
                  Delete: {}
                }
              }
            }
        )");
        env.TestWaitNotification(runtime, txId);
        CheckTtlSettings(runtime, OltpTtlChecker);

        TestBuildIndex(runtime, ++txId, TTestTxConfig::SchemeShard, "/MyRoot", "/MyRoot/TTLEnabledTable",
            TBuildIndexConfig{"UserDefinedIndexByValue", indexType, {"value"}, {}});

        env.TestWaitNotification(runtime, txId);
        TestDescribeResult(DescribePath(runtime, "/MyRoot/TTLEnabledTable"), {
            NLs::PathExist,
            NLs::Finished,
            NLs::IndexesCount(1),
        });
    }

    Y_UNIT_TEST(BuildIndexShouldSucceed) {
        BuildIndex(NKikimrSchemeOp::EIndexTypeGlobal);
    }

    Y_UNIT_TEST(BuildAsyncIndexShouldSucceed) {
        BuildIndex(NKikimrSchemeOp::EIndexTypeGlobalAsync);
    }

    using TEvCondEraseReq = TEvDataShard::TEvConditionalEraseRowsRequest;
    using TEvCondEraseResp = TEvDataShard::TEvConditionalEraseRowsResponse;

    void WaitForCondErase(TTestActorRuntimeBase& runtime, TEvCondEraseResp::ProtoRecordType::EStatus status = TEvCondEraseResp::ProtoRecordType::OK) {
        TDispatchOptions opts;
        opts.FinalEvents.emplace_back([status](IEventHandle& ev) -> bool {
            if (ev.GetTypeRewrite() != TEvCondEraseResp::EventType) {
                return false;
            }

            auto resp = ev.Get<TEvCondEraseResp>();
            if (resp->Record.GetStatus() == TEvCondEraseResp::ProtoRecordType::ACCEPTED) {
                return false;
            }

            UNIT_ASSERT_VALUES_EQUAL_C(resp->Record.GetStatus(), status, resp->Record.GetErrorDescription());
            return true;
        });

        runtime.DispatchEvents(opts);
    }

    Y_UNIT_TEST(ConditionalErase) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);

        auto writeRow = [&](ui64 tabletId, ui64 key, TInstant ts, const char* table, const char* ct = "Timestamp") {
            NKikimrMiniKQL::TResult result;
            TString error;
            NKikimrProto::EReplyStatus status = LocalMiniKQL(runtime, tabletId, Sprintf(R"(
                (
                    (let key   '( '('key (Uint64 '%lu ) ) ) )
                    (let row   '( '('ts (%s '%lu) ) ) )
                    (return (AsList (UpdateRow '__user__%s key row) ))
                )
            )", key, ct, ts.GetValue(), table), result, error);

            UNIT_ASSERT_VALUES_EQUAL_C(status, NKikimrProto::EReplyStatus::OK, error);
            UNIT_ASSERT_VALUES_EQUAL(error, "");
        };

        auto readTable = [&](ui64 tabletId, const char* table) {
            NKikimrMiniKQL::TResult result;
            TString error;
            NKikimrProto::EReplyStatus status = LocalMiniKQL(runtime, tabletId, Sprintf(R"(
                (
                    (let range '( '('key (Uint64 '0) (Void) )))
                    (let columns '('key) )
                    (let result (SelectRange '__user__%s range columns '()))
                    (return (AsList (SetResult 'Result result) ))
                )
            )", table), result, error);

            UNIT_ASSERT_VALUES_EQUAL_C(status, NKikimrProto::EReplyStatus::OK, error);
            UNIT_ASSERT_VALUES_EQUAL(error, "");

            return result;
        };

        auto waitForScheduleCondErase = [&]() {
            TDispatchOptions options;
            options.FinalEvents.push_back(TDispatchOptions::TFinalEventCondition(TEvPrivate::EvRunConditionalErase));
            runtime.DispatchEvents(options);
        };

        auto setAllowConditionalEraseOperations = [&](bool value) {
            TAtomic unused;
            runtime.GetAppData().Icb->SetValue("SchemeShard_AllowConditionalEraseOperations", value, unused);
        };

        const TInstant now = TInstant::ParseIso8601("2020-09-18T18:00:00.000000Z");
        runtime.UpdateCurrentTime(now);

        ui64 tabletId = TTestTxConfig::FakeHiveTablets;
        ui64 txId = 100;

        {
            TestCreateTable(runtime, ++txId, "/MyRoot", R"(
                Name: "TTLEnabledTable1"
                Columns { Name: "key" Type: "Uint64" }
                Columns { Name: "ts" Type: "Timestamp" }
                KeyColumnNames: ["key"]
                TTLSettings {
                  Enabled {
                    ColumnName: "ts"
                  }
                }
            )");
            env.TestWaitNotification(runtime, txId);

            writeRow(tabletId, 1, now, "TTLEnabledTable1");
            {
                auto result = readTable(tabletId, "TTLEnabledTable1");
                NKqp::CompareYson(R"([[[[[["1"]]];%false]]])", result);
            }

            runtime.AdvanceCurrentTime(TDuration::Minutes(1));
            WaitForCondErase(runtime);
            {
                auto result = readTable(tabletId, "TTLEnabledTable1");
                NKqp::CompareYson(R"([[[[];%false]]])", result);
            }
        }

        {
            ++tabletId;
            TestCreateTable(runtime, ++txId, "/MyRoot", R"(
                Name: "TTLEnabledTable2"
                Columns { Name: "key" Type: "Uint64" }
                Columns { Name: "ts" Type: "Timestamp" }
                KeyColumnNames: ["key"]
                TTLSettings {
                  Enabled {
                    ColumnName: "ts"
                    ExpireAfterSeconds: 3600
                    Tiers: {
                      ApplyAfterSeconds: 3600
                      Delete: {}
                    }
                  }
                }
            )");
            env.TestWaitNotification(runtime, txId);

            writeRow(tabletId, 1, now - TDuration::Hours(1), "TTLEnabledTable2");
            writeRow(tabletId, 2, now, "TTLEnabledTable2");
            {
                auto result = readTable(tabletId, "TTLEnabledTable2");
                NKqp::CompareYson(R"([[[[[["1"]];[["2"]]];%false]]])", result);
            }

            runtime.AdvanceCurrentTime(TDuration::Minutes(1));
            WaitForCondErase(runtime);
            {
                auto result = readTable(tabletId, "TTLEnabledTable2");
                NKqp::CompareYson(R"([[[[[["2"]]];%false]]])", result);
            }

            runtime.AdvanceCurrentTime(TDuration::Hours(1));
            WaitForCondErase(runtime);
            {
                auto result = readTable(tabletId, "TTLEnabledTable2");
                NKqp::CompareYson(R"([[[[];%false]]])", result);
            }
        }

        {
            setAllowConditionalEraseOperations(false);

            ++tabletId;
            TestCreateTable(runtime, ++txId, "/MyRoot", R"(
                Name: "TTLEnabledTable3"
                Columns { Name: "key" Type: "Uint64" }
                Columns { Name: "ts" Type: "Timestamp" }
                KeyColumnNames: ["key"]
                TTLSettings {
                  Enabled {
                    ColumnName: "ts"
                  }
                }
            )");
            env.TestWaitNotification(runtime, txId);

            writeRow(tabletId, 1, now, "TTLEnabledTable3");
            {
                auto result = readTable(tabletId, "TTLEnabledTable3");
                NKqp::CompareYson(R"([[[[[["1"]]];%false]]])", result);
            }

            runtime.AdvanceCurrentTime(TDuration::Minutes(1));
            waitForScheduleCondErase();
            {
                auto result = readTable(tabletId, "TTLEnabledTable3");
                NKqp::CompareYson(R"([[[[[["1"]]];%false]]])", result);
            }

            runtime.AdvanceCurrentTime(TDuration::Hours(1));
            waitForScheduleCondErase();
            {
                auto result = readTable(tabletId, "TTLEnabledTable3");
                NKqp::CompareYson(R"([[[[[["1"]]];%false]]])", result);
            }
        }

        {
            setAllowConditionalEraseOperations(true);

            ++tabletId;
            TestCreateTable(runtime, ++txId, "/MyRoot", R"(
                Name: "TTLEnabledTable4"
                Columns { Name: "key" Type: "Uint64" }
                Columns { Name: "ts" Type: "Uint64" }
                KeyColumnNames: ["key"]
                TTLSettings {
                  Enabled {
                    ColumnName: "ts"
                    ColumnUnit: UNIT_MICROSECONDS
                  }
                }
            )");
            env.TestWaitNotification(runtime, txId);

            writeRow(tabletId, 1, now, "TTLEnabledTable4", "Uint64");
            writeRow(tabletId, 2, now + TDuration::Days(1), "TTLEnabledTable4", "Uint64");
            {
                auto result = readTable(tabletId, "TTLEnabledTable4");
                NKqp::CompareYson(R"([[[[[["1"]];[["2"]]];%false]]])", result);
            }

            runtime.AdvanceCurrentTime(TDuration::Minutes(1));
            WaitForCondErase(runtime);
            {
                auto result = readTable(tabletId, "TTLEnabledTable4");
                NKqp::CompareYson(R"([[[[[["2"]]];%false]]])", result);
            }
        }

        {
            ++tabletId;
            TestConsistentCopyTables(runtime, ++txId, "/", R"(
                CopyTableDescriptions {
                  SrcPath: "/MyRoot/TTLEnabledTable4"
                  DstPath: "/MyRoot/TTLEnabledTable5"
                }
            )");
            env.TestWaitNotification(runtime, txId);

            writeRow(tabletId, 1, now, "TTLEnabledTable5", "Uint64");
            {
                auto result = readTable(tabletId, "TTLEnabledTable5");
                NKqp::CompareYson(R"([[[[[["1"]];[["2"]]];%false]]])", result);
            }

            runtime.AdvanceCurrentTime(TDuration::Hours(1));
            WaitForCondErase(runtime);
            {
                auto result = readTable(tabletId, "TTLEnabledTable5");
                NKqp::CompareYson(R"([[[[[["2"]]];%false]]])", result);
            }
        }

        {
            ++tabletId;
            TestCopyTable(runtime, ++txId, "/MyRoot", "TTLEnabledTable6", "/MyRoot/TTLEnabledTable5");
            env.TestWaitNotification(runtime, txId);

            writeRow(tabletId, 1, now, "TTLEnabledTable6", "Uint64");
            {
                auto result = readTable(tabletId, "TTLEnabledTable6");
                NKqp::CompareYson(R"([[[[[["1"]];[["2"]]];%false]]])", result);
            }

            runtime.AdvanceCurrentTime(TDuration::Hours(1));
            WaitForCondErase(runtime);
            {
                auto result = readTable(tabletId, "TTLEnabledTable6");
                NKqp::CompareYson(R"([[[[[["2"]]];%false]]])", result);
            }
        }
    }

    Y_UNIT_TEST(BackupCopyHasNoTtlSettings) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "TTLEnabledTable"
            Columns { Name: "key" Type: "Uint64" }
            Columns { Name: "ts" Type: "Timestamp" }
            KeyColumnNames: ["key"]
            TTLSettings {
              Enabled {
                ColumnName: "ts"
              }
            }
        )");
        env.TestWaitNotification(runtime, txId);

        TestConsistentCopyTables(runtime, ++txId, "/", R"(
            CopyTableDescriptions {
              SrcPath: "/MyRoot/TTLEnabledTable"
              DstPath: "/MyRoot/TTLEnabledTableCopy"
              IsBackup: true
            }
        )");
        env.TestWaitNotification(runtime, txId);

        TestDescribeResult(
            DescribePath(runtime, "/MyRoot/TTLEnabledTableCopy"), {
                NLs::PathExist,
                NLs::Finished, [=] (const NKikimrScheme::TEvDescribeSchemeResult& record) {
                    UNIT_ASSERT(!record.GetPathDescription().GetTable().HasTTLSettings());
                }
            }
        );
    }

    Y_UNIT_TEST(RacyAlterTableAndConditionalErase) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        auto delayConditionalErase = [&]() -> THolder<IEventHandle> {
            THolder<IEventHandle> delayed;

            auto prevObserver = runtime.SetObserverFunc([&](TAutoPtr<IEventHandle>& ev) {
                switch (ev->GetTypeRewrite()) {
                case TEvCondEraseReq::EventType:
                    delayed.Reset(ev.Release());
                    return TTestActorRuntime::EEventAction::DROP;
                default:
                    return TTestActorRuntime::EEventAction::PROCESS;
                }
            });

            if (!delayed) {
                TDispatchOptions opts;
                opts.FinalEvents.emplace_back([&delayed](IEventHandle&) -> bool {
                    return bool(delayed);
                });
                runtime.DispatchEvents(opts);
            }

            runtime.SetObserverFunc(prevObserver);
            return delayed;
        };

        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "TTLEnabledTable"
            Columns { Name: "key" Type: "Uint64" }
            Columns { Name: "ts" Type: "Timestamp" }
            KeyColumnNames: ["key"]
            TTLSettings {
              Enabled {
                ColumnName: "ts"
              }
            }
        )");
        env.TestWaitNotification(runtime, txId);

        auto delayed = delayConditionalErase();

        TestAlterTable(runtime, ++txId, "/MyRoot", R"(
            Name: "TTLEnabledTable"
            Columns { Name: "value" Type: "String" }
        )");
        env.TestWaitNotification(runtime, txId);

        runtime.Send(delayed.Release(), 0, true);
        WaitForCondErase(runtime, TEvCondEraseResp::ProtoRecordType::SCHEME_ERROR);
    }

    Y_UNIT_TEST(ShouldCheckQuotas) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TestCreateSubDomain(runtime, ++txId, "/MyRoot", R"(
            Name: "SubDomain"
            PlanResolution: 50
            Coordinators: 1
            Mediators: 1
            TimeCastBucketsPerMediator: 2
            DatabaseQuotas {
              ttl_min_run_internal_seconds: 1800
            }
        )");
        env.TestWaitNotification(runtime, txId);

        // ok without sys settings
        TestCreateTable(runtime, ++txId, "/MyRoot/SubDomain", R"(
            Name: "Table1"
            Columns { Name: "key" Type: "Uint64" }
            Columns { Name: "modified_at" Type: "Timestamp" }
            KeyColumnNames: ["key"]
            TTLSettings {
              Enabled {
                ColumnName: "modified_at"
                ExpireAfterSeconds: 3600
                Tiers: {
                  ApplyAfterSeconds: 3600
                  Delete: {}
                }
              }
            }
        )");
        env.TestWaitNotification(runtime, txId);

        // ok (RunInterval >= limit)
        TestCreateTable(runtime, ++txId, "/MyRoot/SubDomain", R"(
            Name: "Table3"
            Columns { Name: "key" Type: "Uint64" }
            Columns { Name: "modified_at" Type: "Timestamp" }
            KeyColumnNames: ["key"]
            TTLSettings {
              Enabled {
                ColumnName: "modified_at"
                ExpireAfterSeconds: 3600
                SysSettings {
                  RunInterval: 1800000000
                }
                Tiers: {
                  ApplyAfterSeconds: 3600
                  Delete: {}
                }
              }
            }
        )");
        env.TestWaitNotification(runtime, txId);

        // error (RunInterval < limit)
        TestCreateTable(runtime, ++txId, "/MyRoot/SubDomain", R"(
            Name: "Table4"
            Columns { Name: "key" Type: "Uint64" }
            Columns { Name: "modified_at" Type: "Timestamp" }
            KeyColumnNames: ["key"]
            TTLSettings {
              Enabled {
                ColumnName: "modified_at"
                ExpireAfterSeconds: 3600
                SysSettings {
                  RunInterval: 1799999999
                }
                Tiers: {
                  ApplyAfterSeconds: 3600
                  Delete: {}
                }
              }
            }
        )", {{NKikimrScheme::StatusSchemeError, "TTL run interval cannot be less than limit"}});
    }

    Y_UNIT_TEST(ShouldSkipDroppedColumn) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TestCreateIndexedTable(runtime, ++txId, "/MyRoot", R"(
            TableDescription {
              Name: "TTLEnabledTable"
              Columns { Name: "key" Type: "Uint64" }
              Columns { Name: "ts" Type: "Timestamp" }
              Columns { Name: "indexed" Type: "Uint64" }
              Columns { Name: "extra" Type: "Uint64" }
              KeyColumnNames: ["key"]
              TTLSettings {
                Enabled {
                  ColumnName: "ts"
                }
              }
            }
            IndexDescription {
              Name: "Index"
              KeyColumnNames: ["indexed"]
            }
        )");
        env.TestWaitNotification(runtime, txId);

        TestAlterTable(runtime, ++txId, "/MyRoot", R"(
            Name: "TTLEnabledTable"
            DropColumns { Name: "extra" }
        )");
        env.TestWaitNotification(runtime, txId);

        TestAlterTable(runtime, ++txId, "/MyRoot", R"(
            Name: "TTLEnabledTable"
            Columns { Name: "extra" Type: "Uint64" }
        )");
        env.TestWaitNotification(runtime, txId);

        WaitForCondErase(runtime);
    }

    Y_UNIT_TEST(LegacyTtlSettingsNoTiers) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TestCreateTable(runtime, ++txId, "/MyRoot", Sprintf(R"(
            Name: "TTLEnabledTable"
            Columns { Name: "key" Type: "Uint64" }
            Columns { Name: "modified_at" Type: "Timestamp" }
            KeyColumnNames: ["key"]
            TTLSettings {
              Enabled {
                ColumnName: "modified_at"
                ExpireAfterSeconds: 3600
              }
            }
        )"));
        env.TestWaitNotification(runtime, txId);
        CheckTtlSettings(runtime, LegacyOltpTtlChecker);
    }

    Y_UNIT_TEST(LegacyTtlSettingsNoTiersAlterTable) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TestCreateTable(runtime, ++txId, "/MyRoot", Sprintf(R"(
            Name: "TTLEnabledTable"
            Columns { Name: "key" Type: "Uint64" }
            Columns { Name: "modified_at" Type: "Timestamp" }
            KeyColumnNames: ["key"]
        )"));
        env.TestWaitNotification(runtime, txId);

        TestAlterTable(runtime, ++txId, "/MyRoot", R"(
            Name: "TTLEnabledTable"
            TTLSettings {
              Enabled {
                ColumnName: "modified_at"
                ExpireAfterSeconds: 3600
              }
            }
        )");
        env.TestWaitNotification(runtime, txId);
        CheckTtlSettings(runtime, LegacyOltpTtlChecker);
    }

    NKikimrTabletBase::TEvGetCountersResponse GetCounters(TTestBasicRuntime& runtime) {
        const auto sender = runtime.AllocateEdgeActor();
        runtime.SendToPipe(TTestTxConfig::SchemeShard, sender, new TEvTablet::TEvGetCounters);
        auto ev = runtime.GrabEdgeEvent<TEvTablet::TEvGetCountersResponse>(sender);

        UNIT_ASSERT(ev);
        return ev->Get()->Record;
    }

    ui64 GetSimpleCounter(TTestBasicRuntime& runtime, const TString& name) {
        const auto counters = GetCounters(runtime);
        for (const auto& counter : counters.GetTabletCounters().GetAppCounters().GetSimpleCounters()) {
            if (name != counter.GetName()) {
                continue;
            }

            return counter.GetValue();
        }

        UNIT_ASSERT_C(false, "Counter not found: " << name);
        return 0; // unreachable
    }

    void CheckSimpleCounter(TTestBasicRuntime& runtime, const TString& name, ui64 value) {
        UNIT_ASSERT_VALUES_EQUAL(value, GetSimpleCounter(runtime, name));
    }

    auto GetPercentileCounters(TTestBasicRuntime& runtime, const TString& name) {
        const auto counters = GetCounters(runtime);
        for (const auto& counter : counters.GetTabletCounters().GetAppCounters().GetPercentileCounters()) {
            if (name != counter.GetName()) {
                continue;
            }

            return counter;
        }

        Y_FAIL("Counter not found: %s", name.c_str());
    }

    ui64 GetPercentileCounter(const auto& counters, const TString& range) {
        for (ui32 i = 0; i < counters.RangesSize(); ++i) {
            if (range != counters.GetRanges(i)) {
                continue;
            }

            UNIT_ASSERT(i < counters.ValuesSize());
            return counters.GetValues(i);
        }

        Y_FAIL("Range not found: %s", range.c_str());
    }

    ui64 GetPercentileCounter(TTestBasicRuntime& runtime, const TString& name, const TString& range) {
        auto counters = GetPercentileCounters(runtime, name);
        return GetPercentileCounter(counters, range);
    }

    void CheckPercentileCounter(TTestBasicRuntime& runtime, const TString& name, const THashMap<TString, ui64>& rangeValues) {
        auto counters = GetPercentileCounters(runtime, name);
        Cerr << counters.DebugString();
        for (const auto& [range, value] : rangeValues) {
            const auto v = GetPercentileCounter(counters, range);
            UNIT_ASSERT_VALUES_EQUAL_C(v, value, "Unexpected value in range"
                << ": range# " << range
                << ", expected# " << value
                << ", got# " << v);
        }
    }

    void WaitForStats(TTestActorRuntimeBase& runtime, ui32 count) {
        TDispatchOptions opts;
        opts.FinalEvents.emplace_back(TEvDataShard::EvPeriodicTableStats, count);
        runtime.DispatchEvents(opts);
    }

    Y_UNIT_TEST(CheckCounters) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions()
            .EnablePersistentQueryStats(false)
            .DisableStatsBatching(true));
        ui64 txId = 100;

        runtime.UpdateCurrentTime(TInstant::Now());
        CheckSimpleCounter(runtime, "SchemeShard/TTLEnabledTables", 0);
        CheckPercentileCounter(runtime, "SchemeShard/NumShardsByTtlLag", {{"900", 0}, {"1800", 0}, {"inf", 0}});

        // create
        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "TTLEnabledTable"
            Columns { Name: "key" Type: "Uint64" }
            Columns { Name: "ts" Type: "Timestamp" }
            KeyColumnNames: ["key"]
            TTLSettings {
              Enabled {
                ColumnName: "ts"
              }
            }
        )");
        env.TestWaitNotification(runtime, txId);

        // just after create
        CheckSimpleCounter(runtime, "SchemeShard/TTLEnabledTables", 1);
        CheckPercentileCounter(runtime, "SchemeShard/NumShardsByTtlLag", {{"900", 0}, {"1800", 0}, {"inf", 1}});

        // after erase
        WaitForCondErase(runtime);
        WaitForStats(runtime, 1);
        if (GetPercentileCounter(runtime, "SchemeShard/NumShardsByTtlLag", "0") > 0) {
            // Sometimes stats arrive too quickly?
            WaitForStats(runtime, 1);
        }
        CheckPercentileCounter(runtime, "SchemeShard/NumShardsByTtlLag", {{"900", 1}, {"1800", 0}, {"inf", 0}});

        // after a little more time
        runtime.AdvanceCurrentTime(TDuration::Minutes(20));
        WaitForStats(runtime, 1);
        CheckPercentileCounter(runtime, "SchemeShard/NumShardsByTtlLag", {{"900", 0}, {"1800", 1}, {"inf", 0}});

        // copy table
        TestCopyTable(runtime, ++txId, "/MyRoot", "TTLEnabledTableCopy", "/MyRoot/TTLEnabledTable");
        env.TestWaitNotification(runtime, txId);

        // just after copy
        CheckSimpleCounter(runtime, "SchemeShard/TTLEnabledTables", 2);
        CheckPercentileCounter(runtime, "SchemeShard/NumShardsByTtlLag", {{"900", 0}, {"1800", 2}, {"inf", 0}});

        // after erase
        runtime.AdvanceCurrentTime(TDuration::Hours(1));
        WaitForCondErase(runtime);
        WaitForStats(runtime, 2);
        CheckPercentileCounter(runtime, "SchemeShard/NumShardsByTtlLag", {{"900", 2}, {"1800", 0}, {"inf", 0}});

        // alter (disable ttl)
        TestAlterTable(runtime, ++txId, "/MyRoot", R"(
            Name: "TTLEnabledTable"
            TTLSettings {
              Disabled {
              }
            }
        )");
        env.TestWaitNotification(runtime, txId);

        // just after alter
        CheckSimpleCounter(runtime, "SchemeShard/TTLEnabledTables", 1);
        CheckPercentileCounter(runtime, "SchemeShard/NumShardsByTtlLag", {{"900", 1}, {"1800", 0}, {"inf", 0}});

        // drop
        TestDropTable(runtime, ++txId, "/MyRoot", "TTLEnabledTableCopy");
        env.TestWaitNotification(runtime, txId);

        // just after drop
        CheckSimpleCounter(runtime, "SchemeShard/TTLEnabledTables", 0);
        CheckPercentileCounter(runtime, "SchemeShard/NumShardsByTtlLag", {{"900", 0}, {"1800", 0}, {"inf", 0}});

        // alter (enable ttl)
        TestAlterTable(runtime, ++txId, "/MyRoot", R"(
            Name: "TTLEnabledTable"
            TTLSettings {
              Enabled {
                ColumnName: "ts"
              }
            }
        )");
        env.TestWaitNotification(runtime, txId);

        // just after alter
        CheckSimpleCounter(runtime, "SchemeShard/TTLEnabledTables", 1);
        CheckPercentileCounter(runtime, "SchemeShard/NumShardsByTtlLag", {{"900", 1}, {"1800", 0}, {"inf", 0}});

        // after a little more time
        runtime.AdvanceCurrentTime(TDuration::Minutes(20));
        WaitForStats(runtime, 1);
        CheckPercentileCounter(runtime, "SchemeShard/NumShardsByTtlLag", {{"900", 0}, {"1800", 1}, {"inf", 0}});

        // split
        TestSplitTable(runtime, ++txId, "/MyRoot/TTLEnabledTable", Sprintf(R"(
            SourceTabletId: %lu
            SplitBoundary {
              KeyPrefix {
                Tuple { Optional { Uint64: 100 } }
              }
            }
        )", TTestTxConfig::FakeHiveTablets));
        env.TestWaitNotification(runtime, txId);

        // after erase
        runtime.AdvanceCurrentTime(TDuration::Hours(1));
        WaitForCondErase(runtime);
        WaitForStats(runtime, 2);
        CheckPercentileCounter(runtime, "SchemeShard/NumShardsByTtlLag", {{"900", 2}, {"1800", 0}, {"inf", 0}});

        // move table
        TestMoveTable(runtime, ++txId, "/MyRoot/TTLEnabledTable", "/MyRoot/TTLEnabledTableMoved");
        env.TestWaitNotification(runtime, txId);

        // just after move
        CheckSimpleCounter(runtime, "SchemeShard/TTLEnabledTables", 1);
        CheckPercentileCounter(runtime, "SchemeShard/NumShardsByTtlLag", {{"900", 2}, {"1800", 0}, {"inf", 0}});

        // after a while
        runtime.AdvanceCurrentTime(TDuration::Minutes(20));
        WaitForStats(runtime, 2);
        CheckPercentileCounter(runtime, "SchemeShard/NumShardsByTtlLag", {{"900", 0}, {"1800", 2}, {"inf", 0}});

        // after erase
        runtime.AdvanceCurrentTime(TDuration::Minutes(40));
        WaitForCondErase(runtime);
        WaitForStats(runtime, 2);
        CheckPercentileCounter(runtime, "SchemeShard/NumShardsByTtlLag", {{"0", 2}, {"900", 0}, {"1800", 0}, {"inf", 0}});

        // after a while
        runtime.AdvanceCurrentTime(TDuration::Minutes(10));
        WaitForStats(runtime, 2);
        CheckPercentileCounter(runtime, "SchemeShard/NumShardsByTtlLag", {{"900", 2}, {"1800", 0}, {"inf", 0}});
    }

    Y_UNIT_TEST(TtlTiersValidation) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TestCreateTable(runtime, ++txId, "/MyRoot", Sprintf(R"(
            Name: "TTLEnabledTable"
            Columns { Name: "key" Type: "Uint64" }
            Columns { Name: "modified_at" Type: "Timestamp" }
            KeyColumnNames: ["key"]
        )"));
        env.TestWaitNotification(runtime, txId);

        TestAlterTable(runtime, ++txId, "/MyRoot", R"(
            Name: "TTLEnabledTable"
            TTLSettings {
              Enabled {
                ColumnName: "modified_at"
                Tiers {
                    Delete {}
                    ApplyAfterSeconds: 3600
                }
                Tiers {
                    Delete {}
                    ApplyAfterSeconds: 7200
                }
              }
            }
        )", {{NKikimrScheme::StatusInvalidParameter, "Tier 0: only the last tier in TTL settings can have Delete action"}});

        TestAlterTable(runtime, ++txId, "/MyRoot", R"(
            Name: "TTLEnabledTable"
            TTLSettings {
              Enabled {
                ColumnName: "modified_at"
                Tiers {
                    EvictToExternalStorage {
                        Storage: "/Root/abc"
                    }
                    ApplyAfterSeconds: 3600
                }
                Tiers {
                    Delete {}
                    ApplyAfterSeconds: 7200
                }
              }
            }
        )", {{NKikimrScheme::StatusInvalidParameter, "Only DELETE via TTL is allowed for row-oriented tables"}});
    }
}

Y_UNIT_TEST_SUITE(TSchemeShardColumnTableTTL) {
    static void CreateColumnTableShouldSucceed(const char* name, const char* ttlColumnType, const char* unit = "UNIT_AUTO") {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TestCreateColumnTable(runtime, ++txId, "/MyRoot", Sprintf(R"(
            Name: "%s"
            Schema {
                Columns { Name: "key" Type: "Uint64" NotNull: true }
                Columns { Name: "modified_at" Type: "%s" NotNull: true }
                KeyColumnNames: ["modified_at"]
            }
            TtlSettings {
                Enabled {
                    ColumnName: "modified_at"
                    ExpireAfterSeconds: 3600
                    ColumnUnit: %s
                    Tiers: {
                      ApplyAfterSeconds: 3600
                      Delete: {}
                    }
                }
            }
        )", name, ttlColumnType, unit));
        env.TestWaitNotification(runtime, txId);
        CheckTtlSettings(runtime, OlapTtlChecker(), name);
    }

    Y_UNIT_TEST(CreateColumnTable) {
        for (auto ct : {/*"Date",*/ "Datetime", "Timestamp"}) {
            CreateColumnTableShouldSucceed("TTLEnabledTable", ct);
        }

        for (auto ct : {"Uint32", "Uint64"/*, "DyNumber"*/}) {
            for (auto unit : {"UNIT_SECONDS"/*, "UNIT_MILLISECONDS", "UNIT_MICROSECONDS", "UNIT_NANOSECONDS"*/}) {
                CreateColumnTableShouldSucceed("TTLEnabledTable", ct, unit);
            }
        }
    }

    Y_UNIT_TEST(CreateColumnTableNegative_ColumnType) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        for (auto ct : {"String", "DyNumber"}) {
            TestCreateColumnTable(runtime, ++txId, "/MyRoot",
                Sprintf(R"(
                Name: "TTLEnabledTable"
                Schema {
                    Columns { Name: "key" Type: "Uint64" NotNull: true }
                    Columns { Name: "modified_at" Type: "%s" }
                    KeyColumnNames: ["key"]
                }
                TtlSettings {
                    Enabled {
                        ColumnName: "modified_at"
                        ExpireAfterSeconds: 3600
                        Tiers: {
                            ApplyAfterSeconds: 3600
                            Delete: {}
                        }
                    }
                }
            )",
            ct), {
                { NKikimrScheme::StatusSchemeError, "Type 'DyNumber' specified for column 'modified_at' is not supported" },
                { NKikimrScheme::StatusSchemeError, "Unsupported column type"
            } });
        }
    }

    Y_UNIT_TEST(CreateColumnTableNegative_UnknownColumn) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TestCreateColumnTable(runtime, ++txId, "/MyRoot", R"(
            Name: "TTLEnabledTable"
            Schema {
                Columns { Name: "key" Type: "Uint64" NotNull: true }
                Columns { Name: "modified_at" Type: "Timestamp" }
                KeyColumnNames: ["key"]
            }
            TtlSettings {
              Enabled {
                ColumnName: "created_at"
              }
            }
        )", {{NKikimrScheme::StatusSchemeError, "Incorrect ttl column - not found in scheme"}});
    }

    Y_UNIT_TEST(AlterColumnTable) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TestCreateColumnTable(runtime, ++txId, "/MyRoot", R"(
            Name: "TTLEnabledTable"
            Schema {
                Columns { Name: "key" Type: "Uint64" NotNull: true }
                Columns { Name: "modified_at" Type: "Timestamp" NotNull: true }
                Columns { Name: "saved_at" Type: "Datetime" }
                Columns { Name: "data" Type: "Utf8" }
                KeyColumnNames: ["modified_at"]
            }
        )");
        env.TestWaitNotification(runtime, txId);
        TestDescribeResult(
            DescribePath(runtime, "/MyRoot/TTLEnabledTable"), {
                NLs::PathExist,
                NLs::Finished, [=] (const NKikimrScheme::TEvDescribeSchemeResult& record) {
                    const auto& table = record.GetPathDescription().GetColumnTableDescription();
                    UNIT_ASSERT(!table.HasTtlSettings());
                }
            }
        );

        TestAlterColumnTable(runtime, ++txId, "/MyRoot", R"(
            Name: "TTLEnabledTable"
            AlterTtlSettings {
              Enabled {
                ColumnName: "modified_at"
                ExpireAfterSeconds: 3600
                Tiers: {
                  ApplyAfterSeconds: 3600
                  Delete: {}
                }
              }
            }
        )");
        env.TestWaitNotification(runtime, txId);
        CheckTtlSettings(runtime, OlapTtlChecker());

        TestAlterColumnTable(runtime, ++txId, "/MyRoot", R"(
            Name: "TTLEnabledTable"
            AlterTtlSettings {
              Disabled {
              }
            }
        )");
        env.TestWaitNotification(runtime, txId);
        TestDescribeResult(
            DescribePath(runtime, "/MyRoot/TTLEnabledTable"), {
                NLs::PathExist,
                NLs::Finished, [=] (const NKikimrScheme::TEvDescribeSchemeResult& record) {
                    const auto& table = record.GetPathDescription().GetColumnTableDescription();
                    UNIT_ASSERT(table.HasTtlSettings());
                    UNIT_ASSERT(table.GetTtlSettings().HasDisabled());
                }
            }
        );
        TestAlterColumnTable(runtime, ++txId, "/MyRoot", R"(
            Name: "TTLEnabledTable"
            AlterSchema {
                AlterColumns {Name: "data" DefaultValue: "10"}
            }
        )", {{NKikimrScheme::StatusSchemeError, "sparsed columns are disabled"}});
        env.TestWaitNotification(runtime, txId);
    }

    Y_UNIT_TEST(AlterColumnTable_Negative) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TestCreateColumnTable(runtime, ++txId, "/MyRoot", R"(
            Name: "TTLEnabledTable"
            Schema {
                Columns { Name: "key" Type: "Uint64" NotNull: true }
                Columns { Name: "modified_at" Type: "Timestamp" }
                Columns { Name: "str" Type: "String" }
                KeyColumnNames: ["key"]
            }
        )");
        env.TestWaitNotification(runtime, txId);
        TestDescribeResult(
            DescribePath(runtime, "/MyRoot/TTLEnabledTable"), {
                NLs::PathExist,
                NLs::Finished, [=] (const NKikimrScheme::TEvDescribeSchemeResult& record) {
                    const auto& table = record.GetPathDescription().GetColumnTableDescription();
                    UNIT_ASSERT(!table.HasTtlSettings());
                }
            }
        );

        TestAlterColumnTable(runtime, ++txId, "/MyRoot", R"(
            Name: "TTLEnabledTable"
            AlterTtlSettings {
              Enabled {
                ColumnName: "str"
                ExpireAfterSeconds: 3600
                Tiers: {
                  ApplyAfterSeconds: 3600
                  Delete: {}
                }
              }
            }
        )", {{NKikimrScheme::StatusSchemeError, "Unsupported column type"}});
    }
}

Y_UNIT_TEST_SUITE(TSchemeShardTTLTestsWithReboots) {
    Y_UNIT_TEST(CreateTable) {
        TTestWithReboots t;
        t.Run([&](TTestActorRuntime& runtime, bool& activeZone) {
            TestCreateTable(runtime, ++t.TxId, "/MyRoot", R"(
                Name: "TTLEnabledTable"
                Columns { Name: "key" Type: "Uint64" }
                Columns { Name: "modified_at" Type: "Timestamp" }
                KeyColumnNames: ["key"]
                TTLSettings {
                  Enabled {
                    ColumnName: "modified_at"
                    ExpireAfterSeconds: 3600
                    Tiers: {
                      ApplyAfterSeconds: 3600
                      Delete: {}
                    }
                  }
                }
            )");
            t.TestEnv->TestWaitNotification(runtime, t.TxId);

            {
                TInactiveZone inactive(activeZone);
                CheckTtlSettings(runtime, OltpTtlChecker);
            }
        });
    }

    Y_UNIT_TEST(AlterTable) {
        TTestWithReboots t;
        t.Run([&](TTestActorRuntime& runtime, bool& activeZone) {
            {
                TInactiveZone inactive(activeZone);
                TestCreateTable(runtime, ++t.TxId, "/MyRoot", R"(
                    Name: "TTLEnabledTable"
                    Columns { Name: "key" Type: "Uint64" }
                    Columns { Name: "modified_at" Type: "Timestamp" }
                    KeyColumnNames: ["key"]
                )");
                t.TestEnv->TestWaitNotification(runtime, t.TxId);
            }

            TestAlterTable(runtime, ++t.TxId, "/MyRoot", R"(
                Name: "TTLEnabledTable"
                TTLSettings {
                  Enabled {
                    ColumnName: "modified_at"
                    ExpireAfterSeconds: 3600
                    Tiers: {
                      ApplyAfterSeconds: 3600
                      Delete: {}
                    }
                  }
                }
            )");
            t.TestEnv->TestWaitNotification(runtime, t.TxId);

            {
                TInactiveZone inactive(activeZone);
                CheckTtlSettings(runtime, OltpTtlChecker);
            }
        });
    }

    Y_UNIT_TEST(CopyTable) {
        TTestWithReboots t;
        t.Run([&](TTestActorRuntime& runtime, bool& activeZone) {
            {
                TInactiveZone inactive(activeZone);
                TestCreateTable(runtime, ++t.TxId, "/MyRoot", R"(
                    Name: "TTLEnabledTable"
                    Columns { Name: "key" Type: "Uint64" }
                    Columns { Name: "modified_at" Type: "Timestamp" }
                    KeyColumnNames: ["key"]
                    TTLSettings {
                      Enabled {
                        ColumnName: "modified_at"
                        ExpireAfterSeconds: 3600
                        Tiers: {
                          ApplyAfterSeconds: 3600
                          Delete: {}
                        }
                      }
                    }
                )");
                t.TestEnv->TestWaitNotification(runtime, t.TxId);
            }

            TestCopyTable(runtime, ++t.TxId, "/MyRoot", "TTLEnabledTableCopy", "/MyRoot/TTLEnabledTable");
            t.TestEnv->TestWaitNotification(runtime, t.TxId);

            {
                TInactiveZone inactive(activeZone);
                CheckTtlSettings(runtime, OltpTtlChecker, "TTLEnabledTableCopy");
            }
        });
    }

    Y_UNIT_TEST(MoveTable) {
        TTestWithReboots t;
        t.Run([&](TTestActorRuntime& runtime, bool& activeZone) {
            {
                TInactiveZone inactive(activeZone);
                TestCreateTable(runtime, ++t.TxId, "/MyRoot", R"(
                    Name: "TTLEnabledTable"
                    Columns { Name: "key" Type: "Uint64" }
                    Columns { Name: "modified_at" Type: "Timestamp" }
                    KeyColumnNames: ["key"]
                    TTLSettings {
                      Enabled {
                        ColumnName: "modified_at"
                        ExpireAfterSeconds: 3600
                        Tiers: {
                          ApplyAfterSeconds: 3600
                          Delete: {}
                        }
                      }
                    }
                )");
                t.TestEnv->TestWaitNotification(runtime, t.TxId);
            }

            TestMoveTable(runtime, ++t.TxId, "/MyRoot/TTLEnabledTable", "/MyRoot/TTLEnabledTableMoved");
            t.TestEnv->TestWaitNotification(runtime, t.TxId);

            {
                TInactiveZone inactive(activeZone);
                CheckTtlSettings(runtime, OltpTtlChecker, "TTLEnabledTableMoved");
            }
        });
    }
}
