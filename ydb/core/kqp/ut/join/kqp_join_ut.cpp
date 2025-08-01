#include <ydb/core/kqp/ut/common/kqp_ut_common.h>

#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/proto/accessor.h>

namespace NKikimr {
namespace NKqp {

using namespace NYdb;
using namespace NYdb::NTable;

static TParams BuildPureTableParams(TTableClient& client) {
    return client.GetParamsBuilder()
        .AddParam("$rows")
            .BeginList()
            .AddListItem()
                .BeginStruct()
                    .AddMember("Row").Uint32(1)
                    .AddMember("Value").String("Value1")
                .EndStruct()
            .AddListItem()
                .BeginStruct()
                    .AddMember("Row").Uint32(2)
                    .AddMember("Value").String("Value4")
                .EndStruct()
            .AddListItem()
                .BeginStruct()
                    .AddMember("Row").Uint32(3)
                    .AddMember("Value").String("Value4")
                .EndStruct()
            .AddListItem()
                .BeginStruct()
                    .AddMember("Row").Uint32(4)
                    .AddMember("Value").String("Value10")
                .EndStruct()
            .EndList()
        .Build()
    .Build();
}

static void CreateSampleTables(TSession session) {
    UNIT_ASSERT(session.ExecuteSchemeQuery(R"(
        CREATE TABLE `/Root/Join1_1` (
            Key Int32,
            Fk21 Int32,
            Fk22 String,
            Value String,
            PRIMARY KEY (Key)
        );
        CREATE TABLE `/Root/Join1_2` (
            Key1 Int32,
            Key2 String,
            Fk3 String,
            Value String,
            PRIMARY KEY (Key1, Key2)
        );
        CREATE TABLE `/Root/Join1_3` (
            Key String,
            Value Int32,
            PRIMARY KEY (Key)
        );
    )").GetValueSync().IsSuccess());

     UNIT_ASSERT(session.ExecuteDataQuery(R"(

        REPLACE INTO `/Root/Join1_1` (Key, Fk21, Fk22, Value) VALUES
            (1, 101, "One", "Value1"),
            (2, 102, "Two", "Value1"),
            (3, 103, "One", "Value2"),
            (4, 104, "Two", "Value2"),
            (5, 105, "One", "Value3"),
            (6, 106, "Two", "Value3"),
            (7, 107, "One", "Value4"),
            (8, 108, "One", "Value5");

        REPLACE INTO `/Root/Join1_2` (Key1, Key2, Fk3, Value) VALUES
            (101, "One",   "Name1", "Value21"),
            (101, "Two",   "Name1", "Value22"),
            (101, "Three", "Name3", "Value23"),
            (102, "One",   "Name2", "Value24"),
            (103, "One",   "Name1", "Value25"),
            (104, "One",   "Name3", "Value26"),
            (105, "One",   "Name2", "Value27"),
            (105, "Two",   "Name4", "Value28"),
            (106, "One",   "Name3", "Value29"),
            (108, "One",    NULL,   "Value31"),
            (109, "Four",   NULL,   "Value41");

        REPLACE INTO `/Root/Join1_3` (Key, Value) VALUES
            ("Name1", 1001),
            ("Name2", 1002),
            ("Name4", 1004);

    )", TTxControl::BeginTx().CommitTx()).GetValueSync().IsSuccess());
}


static void CreateLeftJoinSampleTables(TSession session) {
    auto res = session.ExecuteSchemeQuery(R"(
        CREATE TABLE `/Root/Join1_1` (
            Key1 Int32,
            Key2 String,
            Fk1 String,
            Value String,
            PRIMARY KEY (Key1, Key2)
        );
        CREATE TABLE `/Root/Join1_2` (
            Key String,
            Value Int32,
            PRIMARY KEY (Key)
        );

        CREATE TABLE `/Root/Join1_3` (
            Key String, Value Int32, PRIMARY KEY (Key)
        )
    )").ExtractValueSync();
    UNIT_ASSERT_C(res.IsSuccess(), res.GetIssues().ToString());

    auto res2 = session.ExecuteDataQuery(R"(
        REPLACE INTO `/Root/Join1_1` (Key1, Key2, Fk1, Value) VALUES
            (101, "One",    NULL,   "no_right_key_1"),
            (102, "Two",   NULL,   "no_right_key_2"),
            (103, "Three", "Name1", "Value1"),
            (104, "One", "Name2", "Value2"),
            (104, "Two", "Name2", "Value3"),
            (105, "One", "Name3", "no_right_key_3"),
            (106, "One", "Name4", NULL),
            (106, "Two", "Name4", "Value4");

        REPLACE INTO `/Root/Join1_2` (Key, Value) VALUES
            ("Name1", 1001),
            ("Name2", 1002),
            ("Name4", NULL);

        REPLACE INTO `/Root/Join1_3` (Key, Value) VALUES
            ("Name2", 12345);

    )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
    UNIT_ASSERT_C(res2.IsSuccess(), res2.GetIssues().ToString());
}

static void CreateRightSemiJoinSampleTables(TSession& session) {
    UNIT_ASSERT(session.ExecuteSchemeQuery(R"(
        CREATE TABLE `/Root/RSJ_SimpleKey_1` (
            Key Int32,
            Value String,
            PRIMARY KEY (Key)
        );
        CREATE TABLE `/Root/RSJ_SimpleKey_2` (
            Key Int32,
            Value String,
            PRIMARY KEY (Key)
        );
        CREATE TABLE `/Root/RSJ_CompositeKey_1` (
            Key1 Int32,
            Key2 String,
            Value String,
            PRIMARY KEY (Key1, Key2)
        );
        CREATE TABLE `/Root/RSJ_CompositeKey_2` (
            Key1 Int32,
            Key2 String,
            Value String,
            PRIMARY KEY (Key1, Key2)
        );
        CREATE TABLE `/Root/RSJ_SecondaryKeys_1` (
            Key Int32, SubKey1 Int32, SubKey2 String, Value String,
            PRIMARY KEY (Key),
            INDEX Index GLOBAL ON (SubKey1, SubKey2)
        )
    )").GetValueSync().IsSuccess());

    UNIT_ASSERT(session.ExecuteDataQuery(R"(

        REPLACE INTO `/Root/RSJ_SimpleKey_1` (Key, Value) VALUES
            (1,    "1.One"),
            (2,    "1.Two"),
            (3,    "1.Three"),
            (4,    "1.Four"),
            (NULL, "1.Null");

        REPLACE INTO `/Root/RSJ_SimpleKey_2` (Key, Value) VALUES
            (1,    "2.One"),
            (2,    "2.Two"),
            (5,    "2.Five"),
            (6,    "2.Six"),
            (NULL, NULL);

        REPLACE INTO `/Root/RSJ_CompositeKey_1` (Key1, Key2, Value) VALUES
            (1,    "One",   "1.1.One"),
            (2,    "Two",   "1.2.Two"),
            (3,    "Three", "1.3.Three"),
            (6,    "Six",   "1.6.Six"),
            (7,    NULL,    "1.7.Null"),
            (NULL, "Eight", "1.Null.Eight"),
            (NULL, NULL,    "1.Null.Null");

        REPLACE INTO `/Root/RSJ_CompositeKey_2` (Key1, Key2, Value) VALUES
            (1,    "One",   "2.1.One"),
            (6,    "Six",   "2.6.Six"),
            (NULL, "Null",  "2.Null.Null");

        REPLACE INTO `/Root/RSJ_SecondaryKeys_1` (Key, SubKey1, SubKey2, Value) VALUES
            (1,    1,    "2.One",   "Payload1"), -- SubKey contains in the `/Root/RSJ_SimpleKey_2`.Value
            (5,    5,    "2.Five",  "Payload2"), -- SubKey contains in the `/Root/RSJ_SimpleKey_2`.Value
            (7,    7,    "2.Seven", "Payload3"),
            (8,    8,    "2.Eight", "Payload4"),
            (NULL, NULL, NULL,      "Payload5")
    )", TTxControl::BeginTx().CommitTx()).GetValueSync().IsSuccess());
}

static TDataQueryResult ExecQuery(TSession& session, const TString& query, const TParams& params,
    const TString& expected, bool checkRewrite = true)
{
    auto result = ExecQueryAndTestResult(session, query, params, expected);

    if (checkRewrite) {
        auto explain = session.ExplainDataQuery(query).GetValueSync();
        UNIT_ASSERT_C(explain.GetAst().contains("PartitionByKey"), explain.GetAst());
    }

    return result;
}

static TParams NoParams = TParamsBuilder().Build();

Y_UNIT_TEST_SUITE(KqpJoin) {
    Y_UNIT_TEST(IdxLookupLeftPredicate) {
        TKikimrSettings settings;
        TKikimrRunner kikimr(settings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateSampleTables(session);

        NYdb::NTable::TExecDataQuerySettings execSettings;
        execSettings.CollectQueryStats(ECollectQueryStatsMode::Profile);

        auto result = session.ExecuteDataQuery(Q_(R"(
            PRAGMA DisableSimpleColumns;
            SELECT * FROM `/Root/Join1_1` AS t1
            INNER JOIN `/Root/Join1_2` AS t2
            ON t1.Fk21 = t2.Key1 AND t1.Fk22 = t2.Key2
            WHERE t1.Value = 'Value3' AND t2.Value IS NOT NULL
        )"), TTxControl::BeginTx().CommitTx(), execSettings).ExtractValueSync();
        UNIT_ASSERT(result.IsSuccess());

        CompareYson(R"([[[105];["One"];[5];["Value3"];["Name2"];[105];["One"];["Value27"]]])",
            FormatResultSetYson(result.GetResultSet(0)));

        auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());

        if (settings.AppConfig.GetTableServiceConfig().GetEnableKqpDataQueryStreamIdxLookupJoin()) {
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access().size(), 2);

            for (const auto& tableStat : stats.query_phases(0).table_access()) {
                if (tableStat.name() == "/Root/Join1_2") {
                    UNIT_ASSERT_VALUES_EQUAL(tableStat.reads().rows(), 1);
                } else {
                    UNIT_ASSERT_VALUES_EQUAL(tableStat.name(), "/Root/Join1_1");
                    UNIT_ASSERT_VALUES_EQUAL(tableStat.reads().rows(), 8);
                }
            }
        } else {
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), 2);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).name(), "/Root/Join1_1");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).reads().rows(), 8);

            ui32 index = 1;
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(index).table_access().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(index).table_access(0).name(), "/Root/Join1_2");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(index).table_access(0).reads().rows(), 1);
        }
    }

    Y_UNIT_TEST_TWIN(IndexLoookupJoinStructJoin, StreamLookupJoin) {
        TKikimrSettings settings;
        settings.AppConfig.MutableTableServiceConfig()->SetEnableKqpDataQueryStreamIdxLookupJoin(StreamLookupJoin);
        TKikimrRunner kikimr(settings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateSampleTables(session);

        auto result = session.ExecuteDataQuery(Q_(R"(
            $a = AsList(AsStruct(AsStruct("Key" as Key) as join_info), AsStruct(AsStruct("Name1" as Key) as join_info));
            SELECT a.join_info.Key as Key, b.Value as Value from AS_TABLE($a) as a
            LEFT JOIN `/Root/Join1_3` as b
            ON a.join_info.Key = b.Key
        )"), TTxControl::BeginTx().CommitTx()).ExtractValueSync();
        UNIT_ASSERT(result.IsSuccess());

        CompareYson(R"([["Key";#];["Name1";[1001]]])",
            FormatResultSetYson(result.GetResultSet(0)));
    }

    Y_UNIT_TEST(IdxLookupPartialLeftPredicate) {
        TKikimrSettings settings;
        TKikimrRunner kikimr(settings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateSampleTables(session);

        NYdb::NTable::TExecDataQuerySettings execSettings;
        execSettings.CollectQueryStats(ECollectQueryStatsMode::Profile);

        auto result = session.ExecuteDataQuery(Q_(R"(
            PRAGMA DisableSimpleColumns;
            SELECT * FROM `/Root/Join1_1` AS t1
            INNER JOIN `/Root/Join1_2` AS t2
            ON t1.Fk21 == t2.Key1
            WHERE t1.Value == "Value3" ORDER BY t2.Value;
        )"), TTxControl::BeginTx().CommitTx(), execSettings).ExtractValueSync();
        UNIT_ASSERT(result.IsSuccess());

        CompareYson(R"([
            [[105];["One"];[5];["Value3"];["Name2"];[105];["One"];["Value27"]];
            [[105];["One"];[5];["Value3"];["Name4"];[105];["Two"];["Value28"]];
            [[106];["Two"];[6];["Value3"];["Name3"];[106];["One"];["Value29"]]
        ])", FormatResultSetYson(result.GetResultSet(0)));

        auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());
        Cerr << stats.DebugString() << Endl;

        if (settings.AppConfig.GetTableServiceConfig().GetEnableKqpDataQueryStreamIdxLookupJoin()) {
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access().size(), 2);

            for (const auto& tableStat : stats.query_phases(0).table_access()) {
                if (tableStat.name() == "/Root/Join1_2") {
                    UNIT_ASSERT_VALUES_EQUAL(tableStat.reads().rows(), 3);
                } else {
                    UNIT_ASSERT_VALUES_EQUAL(tableStat.name(), "/Root/Join1_1");
                    UNIT_ASSERT_VALUES_EQUAL(tableStat.reads().rows(), 8);
                }
            }
        } else {
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), 2);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).name(), "/Root/Join1_1");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).reads().rows(), 8);

            ui32 index = 1;
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(index).table_access().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(index).table_access(0).name(), "/Root/Join1_2");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(index).table_access(0).reads().rows(), 3);
        }
    }

    Y_UNIT_TEST(IdxLookupPartialWithTempTable) {
        TKikimrRunner kikimr(SyntaxV1Settings());
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateSampleTables(session);

        auto params = TParamsBuilder()
            .AddParam("$in")
                .BeginList()
                    .AddListItem()
                        .BeginStruct()
                            .AddMember("k").Int32(101)
                        .EndStruct()
                .EndList().Build()
             .Build();


        const TString query = Q_(R"(
            DECLARE $in AS List<Struct<k: Int32>>;
            SELECT * FROM AS_TABLE($in) AS t1
            INNER JOIN `/Root/Join1_2` AS t2
            ON t1.k == t2.Key1
            ORDER BY k, Value;
        )");

        const TString expected = R"(
            [
                [["Name1"];[101];["One"];["Value21"];101];
                [["Name1"];[101];["Two"];["Value22"];101];
                [["Name3"];[101];["Three"];["Value23"];101];
            ]
        )";

        auto result = ExecQuery(session, query, params, expected, false);
        AssertTableReads(result, "/Root/Join1_2", 3);
    }

    Y_UNIT_TEST(JoinWithDuplicates) {
        TKikimrRunner kikimr;
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        AssertSuccessResult(session.ExecuteSchemeQuery(R"(
            --!syntax_v1

            CREATE TABLE ObjectParts (
                Hash Uint64,
                BucketName Utf8,
                ObjectName Utf8,
                UploadStartedUsec Uint64,
                Id Uint32,
                CreatedUsec Uint64,
                DataSize Uint64,
                DataMd5 Utf8,
                DataSourceType String,
                DataSource String,
                PRIMARY KEY(Hash, BucketName, ObjectName, UploadStartedUsec, Id)
            );

            CREATE TABLE Objects (
                Hash Uint64,
                BucketName Utf8,
                Name Utf8,
                CreatedUsec Uint64,
                StorageClass Uint8,
                UploadStartedUsec Uint64,
                DataSize Uint64,
                DataMd5 Utf8,
                MetadataType String,
                Metadata String,
                DataSourceType String,
                DataSource String,
                PartsCount Uint32,
                ACL String,
                CreatorId Utf8,
                PRIMARY KEY(Hash, BucketName, Name)
            );
        )").GetValueSync());

        auto result = session.ExecuteDataQuery(R"(
            --!syntax_v1

            $bucket_name = "bucket #1";
            $object_name = "mpobject";
            $upload_started_usec = 999;
            $parts_count = 5;

            UPSERT INTO Objects (Hash, BucketName, Name, UploadStartedUsec, PartsCount) VALUES
                (5775455696462964606, $bucket_name, $object_name, $upload_started_usec, $parts_count);

            UPSERT INTO ObjectParts (Hash, BucketName, ObjectName, UploadStartedUsec, Id, DataSize, DataMd5) VALUES
                (5775455696462964606, $bucket_name, $object_name, $upload_started_usec, 1, 1, "hash-1"),
                (5775455696462964606, $bucket_name, $object_name, $upload_started_usec, 2, 2, "hash-2"),
                (5775455696462964606, $bucket_name, $object_name, $upload_started_usec, 3, 3, "hash-3"),
                (5775455696462964606, $bucket_name, $object_name, $upload_started_usec, 4, 5, "hash-4"),
                (5775455696462964606, $bucket_name, $object_name, $upload_started_usec, 5, 8, "hash-5"),
                (5775455696462964606, $bucket_name, $object_name, $upload_started_usec + 1, 1, 1, "hash");
        )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());

        auto params = kikimr.GetTableClient().GetParamsBuilder()
            .AddParam("$bucket_name").Utf8("bucket #1").Build()
            .AddParam("$object_name").Utf8("mpobject").Build()
            .Build();

        result = session.ExecuteDataQuery(R"(
            --!syntax_v1

            DECLARE $bucket_name AS Utf8;
            DECLARE $object_name AS Utf8;

            SELECT ObjectParts.Id, ObjectParts.DataSize, ObjectParts.DataMd5
            FROM ObjectParts
            JOIN Objects
            ON Objects.Hash = ObjectParts.Hash
                AND Objects.BucketName = ObjectParts.BucketName
                AND Objects.Name = ObjectParts.ObjectName
                AND Objects.UploadStartedUsec = ObjectParts.UploadStartedUsec
            WHERE ObjectParts.Hash = Digest::CityHash($bucket_name || $object_name)
                AND ObjectParts.BucketName = $bucket_name
                AND ObjectParts.ObjectName = $object_name
            ORDER BY ObjectParts.Id;
        )", TTxControl::BeginTx().CommitTx(), params).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        CompareYson(R"([
            [[1u];[1u];["hash-1"]];
            [[2u];[2u];["hash-2"]];
            [[3u];[3u];["hash-3"]];
            [[4u];[5u];["hash-4"]];
            [[5u];[8u];["hash-5"]]
        ])", FormatResultSetYson(result.GetResultSet(0)));
    }

    Y_UNIT_TEST(IdxLookupSelf) {
        TKikimrRunner kikimr(SyntaxV1Settings());
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateSampleTables(session);

        const TString query = Q_(R"(
            SELECT t1.Fk21 AS Key, t2.Value AS Value
            FROM Join1_1 AS t1
            LEFT JOIN Join1_1 AS t2
            ON t1.Fk21 == t2.Key
            WHERE t1.Key == 2
            ORDER BY Key;
        )");

        auto result = ExecQueryAndTestResult(session, query, R"([[[102];#]])");
        AssertTableReads(result, "/Root/Join1_1", 1);
    }

    Y_UNIT_TEST_TWIN(LeftJoinWithNull, StreamLookupJoin) {
        TKikimrSettings settings;
        settings.AppConfig.MutableTableServiceConfig()->SetEnableKqpDataQueryStreamIdxLookupJoin(StreamLookupJoin);
        TKikimrRunner kikimr(settings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateSampleTables(session);

        auto result = session.ExecuteDataQuery(Q_(R"(
            PRAGMA DisableSimpleColumns;
            SELECT * FROM `/Root/Join1_1` AS t1
            INNER JOIN `/Root/Join1_2` AS t2
            ON t1.Fk21 == t2.Key1 AND t1.Fk22 == t2.Key2
            LEFT JOIN `/Root/Join1_3` AS t3
            ON t2.Fk3 = t3.Key
            WHERE t1.Value == "Value5" AND t2.Value == "Value31";
        )"), TTxControl::BeginTx().CommitTx()).ExtractValueSync();
        UNIT_ASSERT(result.IsSuccess());

        CompareYson(R"([[[108];["One"];[8];["Value5"];#;[108];["One"];["Value31"];#;#]])",
            FormatResultSetYson(result.GetResultSet(0)));
    }

    Y_UNIT_TEST(LeftJoinPushdownPredicate_Simple) {
        TKikimrRunner kikimr;
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateSampleTables(session);

        auto result = session.ExecuteDataQuery(Q_(R"(
            PRAGMA FilterPushdownOverJoinOptionalSide;

            SELECT t2.*, t3.Key, t3.Value FROM `/Root/Join1_2` AS t2
            LEFT JOIN `/Root/Join1_3` AS t3
            ON t2.Fk3 = t3.Key
            WHERE t3.Value == 1004;
        )"), TTxControl::BeginTx().CommitTx()).ExtractValueSync();
        UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        CompareYson(
            R"([
                [["Name4"];[105];["Two"];["Value28"];["Name4"];[1004]]
            ])",
            FormatResultSetYson(result.GetResultSet(0))
        );
    }

    Y_UNIT_TEST(LeftJoinPushdownPredicate_NoPushdown) {
        TKikimrRunner kikimr;
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateLeftJoinSampleTables(session);

        auto result = session.ExecuteDataQuery(Q_(R"(
            PRAGMA FilterPushdownOverJoinOptionalSide;

            SELECT t1.Key1, t1.Key2, t1.Fk1, t1.Value, t2.Key, t2.Value FROM `/Root/Join1_2` AS t2
            RIGHT JOIN `/Root/Join1_1` AS t1
            ON t2.Key = t1.Fk1
            WHERE t1.Key1 > 104
            ORDER BY t1.Key1, t1.Key2;
        )"), TTxControl::BeginTx().CommitTx()).ExtractValueSync();
        UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        CompareYson(
            R"([
                [[105];["One"];["Name3"];["no_right_key_3"];#;#];
                [[106];["One"];["Name4"];#;["Name4"];#];
                [[106];["Two"];["Name4"];["Value4"];["Name4"];#]
            ])",
            FormatResultSetYson(result.GetResultSet(0))
        );
    }

    Y_UNIT_TEST(LeftJoinPushdownPredicate_Nulls) {
        TKikimrRunner kikimr;
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateLeftJoinSampleTables(session);
        {
            auto result = session.ExecuteDataQuery(Q_(R"(
                PRAGMA FilterPushdownOverJoinOptionalSide;

                SELECT t1.Key1, t1.Key2, t1.Fk1, t1.Value, t2.Key, t2.Value FROM `/Root/Join1_1` AS t1
                LEFT JOIN `/Root/Join1_2` AS t2
                ON t1.Fk1 = t2.Key
                WHERE t2.Value > 1001
                ORDER BY t1.Value;
            )"), TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
            CompareYson(
                R"([
                    [[104];["One"];["Name2"];["Value2"];["Name2"];[1002]];
                    [[104];["Two"];["Name2"];["Value3"];["Name2"];[1002]]
                ])",
                FormatResultSetYson(result.GetResultSet(0))
            );
        }
        {
            auto result = session.ExecuteDataQuery(Q_(R"(
                SELECT t1.Fk1, t1.Key1, t1.Key2, t1.Value, t2.Key, t2.Value FROM `/Root/Join1_1` AS t1
                LEFT JOIN `/Root/Join1_2` AS t2
                ON t1.Fk1 = t2.Key
                WHERE t2.Value IS NULL
                ORDER BY t1.Key1, t1.Key2;
            )"), TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
            CompareYson(
                R"([
                    [#;[101];["One"];["no_right_key_1"];#;#];
                    [#;[102];["Two"];["no_right_key_2"];#;#];
                    [["Name3"];[105];["One"];["no_right_key_3"];#;#];
                    [["Name4"];[106];["One"];#;["Name4"];#];
                    [["Name4"];[106];["Two"];["Value4"];["Name4"];#]
                ])",
                FormatResultSetYson(result.GetResultSet(0))
            );
        }
        {
            auto result = session.ExecuteDataQuery(Q_(R"(
                PRAGMA FilterPushdownOverJoinOptionalSide;

                SELECT t1.Fk1, t1.Key1, t1.Key2, t1.Value, t2.Key, t2.Value, t3.Key, t3.Value FROM `/Root/Join1_1` AS t1
                LEFT JOIN `/Root/Join1_2` AS t2
                ON t1.Fk1 = t2.Key
                LEFT JOIN `/Root/Join1_3` AS t3
                ON t2.Key = t3.Key
                WHERE t2.Value IS NULL
                ORDER BY t1.Key1, t1.Key2;
            )"), TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
            CompareYson(
                R"([
                    [#;[101];["One"];["no_right_key_1"];#;#;#;#];
                    [#;[102];["Two"];["no_right_key_2"];#;#;#;#];
                    [["Name3"];[105];["One"];["no_right_key_3"];#;#;#;#];
                    [["Name4"];[106];["One"];#;["Name4"];#;#;#];
                    [["Name4"];[106];["Two"];["Value4"];["Name4"];#;#;#]
                ])",
                FormatResultSetYson(result.GetResultSet(0))
            );
        }
    }

     Y_UNIT_TEST(LeftJoinPushdownPredicate_NestedJoin) {
        TKikimrRunner kikimr;
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateLeftJoinSampleTables(session);
        {
            auto result = session.ExecuteDataQuery(Q_(R"(
                PRAGMA FilterPushdownOverJoinOptionalSide;
                PRAGMA config.flags("OptimizerFlags", "FuseEquiJoinsInputMultiLabels", "PullUpFlatMapOverJoinMultipleLabels");

                SELECT t1.Key1, t1.Key2, t1.Fk1, t1.Value, t2.Key, t2.Value, t3.Key, t3.Value

                FROM `/Root/Join1_1` AS t1
                LEFT JOIN `/Root/Join1_2` AS t2
                ON t1.Fk1 = t2.Key
                INNER JOIN `/Root/Join1_3` AS t3
                ON t1.Fk1 = t3.Key

                WHERE t2.Value > 1001;
            )"), TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
            CompareYson(
                R"([
                    [[104];["One"];["Name2"];["Value2"];["Name2"];[1002];["Name2"];[12345]];
                    [[104];["Two"];["Name2"];["Value3"];["Name2"];[1002];["Name2"];[12345]]
                ])",
                FormatResultSetYson(result.GetResultSet(0))
            );
        }
        {
            auto result = session.ExecuteDataQuery(Q_(R"(
                PRAGMA FilterPushdownOverJoinOptionalSide;
                PRAGMA config.flags("OptimizerFlags", "FuseEquiJoinsInputMultiLabels", "PullUpFlatMapOverJoinMultipleLabels");

                SELECT t1.Key1, t1.Key2, t1.Fk1, t1.Value, t2.Key, t2.Value, t3.Key, t3.Value

                FROM `/Root/Join1_1` AS t1
                CROSS JOIN `/Root/Join1_3` AS t3
                LEFT JOIN `/Root/Join1_2` AS t2
                ON t1.Fk1 = t2.Key

                WHERE t2.Value > 1001;
            )"), TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
            CompareYson(
                R"([
                    [[104];["One"];["Name2"];["Value2"];["Name2"];[1002];["Name2"];[12345]];
                    [[104];["Two"];["Name2"];["Value3"];["Name2"];[1002];["Name2"];[12345]]
                ])",
                FormatResultSetYson(result.GetResultSet(0))
            );
        }
    }

    // join on not key column => Full Scan
    Y_UNIT_TEST(RightSemiJoin_FullScan) {
        TKikimrRunner kikimr(SyntaxV1Settings());
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateRightSemiJoinSampleTables(session);

        /* join with parameters */
        {
            const TString query = Q_(R"(
                DECLARE $in AS List<Struct<v: String?>>;
                SELECT *
                FROM AS_TABLE($in) AS k RIGHT SEMI JOIN `/Root/RSJ_SimpleKey_1` AS t ON k.v = t.Value
                ORDER BY Key
            )");

            auto params = TParamsBuilder().AddParam("$in").BeginList()
                    .AddListItem().BeginStruct().AddMember("v").OptionalString("1.One").EndStruct()
                    .AddListItem().BeginStruct().AddMember("v").OptionalString("1.One").EndStruct()   // dup
                    .AddListItem().BeginStruct().AddMember("v").OptionalString("1.Two").EndStruct()
                    .AddListItem().BeginStruct().AddMember("v").OptionalString("Any").EndStruct()     // not exists
                    .AddListItem().BeginStruct().AddMember("v").OptionalString(std::nullopt).EndStruct() // null
                    .EndList().Build().Build();

            auto result = ExecQuery(session, query, params, R"([[[1];["1.One"]];[[2];["1.Two"]]])", false);
            AssertTableReads(result, "/Root/RSJ_SimpleKey_1", 5);
        }

        /* join with real table */
        {
            const TString query = Q_(R"(
                SELECT *
                FROM `/Root/RSJ_SimpleKey_1` AS l RIGHT SEMI JOIN `/Root/RSJ_SimpleKey_2` AS r on l.Value = r.Value
            )");

            auto result = ExecQuery(session, query, NoParams, R"([])", false);
            AssertTableReads(result, "/Root/RSJ_SimpleKey_1", 5);
            AssertTableReads(result, "/Root/RSJ_SimpleKey_2", 5);
        }
    }

    // join on key (simple and full) column => index-lookup
    Y_UNIT_TEST(RightSemiJoin_SimpleKey) {
        TKikimrRunner kikimr(SyntaxV1Settings());
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateRightSemiJoinSampleTables(session);

        /* join with parameters */
        {
            const TString query = Q_(R"(
                DECLARE $in AS List<Struct<k: Int32?>>;
                SELECT *
                FROM AS_TABLE($in) AS k RIGHT SEMI JOIN `/Root/RSJ_SimpleKey_1` AS t ON k.k = t.Key
                ORDER BY Value
            )");

            auto params = TParamsBuilder().AddParam("$in").BeginList()
                    .AddListItem().BeginStruct().AddMember("k").OptionalInt32(1).EndStruct()
                    .AddListItem().BeginStruct().AddMember("k").OptionalInt32(1).EndStruct()   // dup
                    .AddListItem().BeginStruct().AddMember("k").OptionalInt32(2).EndStruct()
                    .AddListItem().BeginStruct().AddMember("k").OptionalInt32(42).EndStruct()  // not exists
                    .AddListItem().BeginStruct().AddMember("k").OptionalInt32(std::nullopt).EndStruct() // null
                    .EndList().Build().Build();

            auto result = ExecQuery(session, query, params, R"([[[1];["1.One"]];[[2];["1.Two"]]])");
            AssertTableReads(result, "/Root/RSJ_SimpleKey_1", 2 /* [1, 2] */);
        }

        /* join with real table */
        {
            const TString query = Q_(R"(
                SELECT *
                FROM `/Root/RSJ_SimpleKey_1` AS l RIGHT SEMI JOIN `/Root/RSJ_SimpleKey_2` AS r ON l.Key = r.Key
                ORDER BY Key;
            )");

            auto result = ExecQuery(session, query, NoParams, R"([[[1];["2.One"]];[[2];["2.Two"]]])");
            AssertTableReads(result, "/Root/RSJ_SimpleKey_1", 5 /* all keys */);
            AssertTableReads(result, "/Root/RSJ_SimpleKey_2", 2 /* [1, 2] */);
        }
    }

    // join on key (complex and full) column => index-lookup
    Y_UNIT_TEST(RightSemiJoin_ComplexKey) {
        TKikimrRunner kikimr(SyntaxV1Settings());
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateRightSemiJoinSampleTables(session);

        /* join with parameters */
        {
            const TString query = Q_(R"(
                    DECLARE $in AS List<Struct<k1: Int32?, k2: String?>>;
                    SELECT *
                    FROM AS_TABLE($in) AS k RIGHT SEMI JOIN `/Root/RSJ_CompositeKey_1` AS t
                         ON k.k1 = t.Key1 AND k.k2 = t.Key2
                    ORDER BY Value
                )");

            auto params = TParamsBuilder().AddParam("$in").BeginList()
                    .AddListItem().BeginStruct().AddMember("k1").OptionalInt32(1)
                                                .AddMember("k2").OptionalString("One").EndStruct()
                    .AddListItem().BeginStruct().AddMember("k1").OptionalInt32(1)
                                                .AddMember("k2").OptionalString("One").EndStruct()       // dup
                    .AddListItem().BeginStruct().AddMember("k1").OptionalInt32(2)
                                                .AddMember("k2").OptionalString("Two").EndStruct()
                    .AddListItem().BeginStruct().AddMember("k1").OptionalInt32(42)
                                                .AddMember("k2").OptionalString("FortyTwo").EndStruct()  // not exists
                    .AddListItem().BeginStruct().AddMember("k1").OptionalInt32(std::nullopt)
                                                .AddMember("k2").OptionalString("One").EndStruct()       // null
                    .AddListItem().BeginStruct().AddMember("k1").OptionalInt32(1)
                                                .AddMember("k2").OptionalString(std::nullopt).EndStruct()   // null
                    .AddListItem().BeginStruct().AddMember("k1").OptionalInt32(std::nullopt)
                                                .AddMember("k2").OptionalString(std::nullopt).EndStruct()   // null
                    .EndList().Build().Build();

            auto result = ExecQuery(session, query, params, R"([[[1];["One"];["1.1.One"]];[[2];["Two"];["1.2.Two"]]])");
            AssertTableReads(result, "/Root/RSJ_CompositeKey_1", 2 /* [1, 2] */);
        }

        /* join with real table */
        {
            const TString query = Q_(R"(
                    SELECT *
                    FROM `/Root/RSJ_CompositeKey_1` AS l RIGHT SEMI JOIN `/Root/RSJ_CompositeKey_2` AS r
                         ON l.Key1 = r.Key1 AND l.Key2 = r.Key2
                    ORDER BY Value;
                )");

            auto result = ExecQuery(session, query, NoParams, R"([[[1];["One"];["2.1.One"]];[[6];["Six"];["2.6.Six"]]])");
            AssertTableReads(result, "/Root/RSJ_CompositeKey_1", 7 /* all keys */);
            AssertTableReads(result, "/Root/RSJ_CompositeKey_2", 2 /* [1, 6] */);
        }
    }

    Y_UNIT_TEST(TwoJoinsWithQueryService) {
        auto serverSettings = TKikimrSettings().SetWithSampleTables(false);

        TKikimrRunner kikimr(serverSettings);
        auto client = kikimr.GetTableClient();
        auto db = kikimr.GetQueryClient();
        auto settings = NYdb::NQuery::TExecuteQuerySettings();

        {
            auto session = client.CreateSession().GetValueSync().GetSession();
            const auto query = Q_(R"(
                CREATE TABLE ta(
                    a Int64,
                    b Int64,
                    c Int64,
                    PRIMARY KEY(a)
                );
            )");
            auto result = session.ExecuteSchemeQuery(query).ExtractValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        }
        {
            auto session = client.CreateSession().GetValueSync().GetSession();
            const auto query = Q_(R"(
                CREATE TABLE tb(
                    b Int64,
                    bval Int64,
                    PRIMARY KEY(b)
                );
            )");
            auto result = session.ExecuteSchemeQuery(query).ExtractValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        }
        {
            auto session = client.CreateSession().GetValueSync().GetSession();
            const auto query = Q_(R"(
                CREATE TABLE tc(
                    c Int64,
                    cval Int64,
                    PRIMARY KEY(c)
                );
            )");
            auto result = session.ExecuteSchemeQuery(query).ExtractValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        }
        {
            auto result = db.ExecuteQuery(R"(
                UPSERT INTO ta(a, b, c) VALUES (1, 1001, 2001), (2, 1002, 2002), (3, 1003, 2003);
                UPSERT INTO tb(b, bval) VALUES (1001, 1001), (1002, 1002), (1003, 1003);
                UPSERT INTO tc(c, cval) VALUES (2001, 2001), (2002, 2002), (2003, 2003);
            )", NYdb::NQuery::TTxControl::BeginTx().CommitTx(), settings).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        }
        {
            auto result = db.ExecuteQuery(R"(
                SELECT ta.a, tb.bval, tc.cval FROM ta INNER JOIN tb ON ta.b = tb.b LEFT JOIN tc ON ta.c = tc.cval
                ORDER BY ta.a, tb.bval, tc.cval;
            )", NYdb::NQuery::TTxControl::BeginTx().CommitTx(), settings).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([[[1];[1001];[2001]];[[2];[1002];[2002]];[[3];[1003];[2003]]])", FormatResultSetYson(result.GetResultSet(0)));
        }
    }

    // join on key prefix => index-lookup
    Y_UNIT_TEST(RightSemiJoin_KeyPrefix) {
        TKikimrRunner kikimr(SyntaxV1Settings());
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateRightSemiJoinSampleTables(session);

        /* join with parameters */
        {
            const TString query = Q_(R"(
                DECLARE $in AS List<Struct<k: Int32?>>;
                SELECT *
                FROM AS_TABLE($in) AS l RIGHT SEMI JOIN `/Root/RSJ_CompositeKey_1` AS r
                     ON l.k = r.Key1
                ORDER BY Value
            )");

            auto params = TParamsBuilder().AddParam("$in").BeginList()
                    .AddListItem().BeginStruct().AddMember("k").OptionalInt32(1).EndStruct()
                    .AddListItem().BeginStruct().AddMember("k").OptionalInt32(1).EndStruct()   // dup
                    .AddListItem().BeginStruct().AddMember("k").OptionalInt32(2).EndStruct()
                    .AddListItem().BeginStruct().AddMember("k").OptionalInt32(42).EndStruct()  // not exists
                    .AddListItem().BeginStruct().AddMember("k").OptionalInt32(std::nullopt).EndStruct() // null
                    .EndList().Build().Build();

            auto result = ExecQuery(session, query, params, R"([[[1];["One"];["1.1.One"]];[[2];["Two"];["1.2.Two"]]])");
            // read of not existing value |42| is not visible in statistics
            AssertTableReads(result, "/Root/RSJ_CompositeKey_1", 2 /* [1, 2, |42|] */);
        }

        /* join with real table */
        {
            const TString query = Q_(R"(
                        SELECT *
                        FROM `/Root/RSJ_SimpleKey_1` AS l RIGHT SEMI JOIN `/Root/RSJ_CompositeKey_1` AS r
                             ON l.Key = r.Key1
                        ORDER BY Value;
                    )");

            auto result = ExecQuery(session, query, NoParams, R"([[[1];["One"];["1.1.One"]];[[2];["Two"];["1.2.Two"]];[[3];["Three"];["1.3.Three"]]])");
            AssertTableReads(result, "/Root/RSJ_SimpleKey_1", 5 /* all rows */);
            AssertTableReads(result, "/Root/RSJ_CompositeKey_1", 3 /* [1, 2, 3] */);
        }
    }

    // join on secondary index => index-lookup
    Y_UNIT_TEST(RightSemiJoin_SecondaryIndex) {
        TKikimrRunner kikimr(SyntaxV1Settings());
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateRightSemiJoinSampleTables(session);

        UNIT_ASSERT(session.ExecuteSchemeQuery(R"(
            CREATE TABLE `/Root/RSJ_SimpleKey_3` (
                Key Int32, SubKey String, Value String,
                PRIMARY KEY (Key),
                INDEX SubKeyIndex GLOBAL ON (SubKey)
            )
        )").GetValueSync().IsSuccess());

        UNIT_ASSERT(session.ExecuteDataQuery(Q_(R"(
            REPLACE INTO `/Root/RSJ_SimpleKey_3` (Key, SubKey, Value) VALUES
                (1,    "2.One",   "Payload1"), -- SubKey contains in the `/Root/RSJ_SimpleKey_2`.Value
                (5,    "2.Five",  "Payload2"), -- SubKey contains in the `/Root/RSJ_SimpleKey_2`.Value
                (7,    "2.Seven", "Payload3"),
                (8,    "2.Eight", "Payload4"),
                (NULL, NULL,      "Payload5")
        )"), TTxControl::BeginTx().CommitTx()).GetValueSync().IsSuccess());

        /* join with parameters */
        {
            const TString query = Q_(R"(
                    DECLARE $in AS List<Struct<v: String?>>;
                    SELECT *
                    FROM AS_TABLE($in) AS l RIGHT SEMI JOIN `/Root/RSJ_SimpleKey_3` VIEW SubKeyIndex AS r
                         ON l.v = r.SubKey
                    ORDER BY Key
                )");

            auto params = TParamsBuilder().AddParam("$in").BeginList()
                    .AddListItem().BeginStruct().AddMember("v").OptionalString("2.One").EndStruct()
                    .AddListItem().BeginStruct().AddMember("v").OptionalString("2.One").EndStruct()   // dup
                    .AddListItem().BeginStruct().AddMember("v").OptionalString("2.Five").EndStruct()
                    .AddListItem().BeginStruct().AddMember("v").OptionalString("Any").EndStruct()     // not exists
                    .AddListItem().BeginStruct().AddMember("v").OptionalString(std::nullopt).EndStruct() // null
                    .EndList().Build().Build();

            auto result = ExecQuery(session, query, params, R"([[[1];["2.One"];["Payload1"]];[[5];["2.Five"];["Payload2"]]])");
            // todo: reading of absent |Any| key is not visible in statistics :(
            AssertTableReads(result, "/Root/RSJ_SimpleKey_3/SubKeyIndex/indexImplTable", 2 /* [2.One, 2.Five, |Any|] */);
            AssertTableReads(result, "/Root/RSJ_SimpleKey_3", 2 /* read .Value field */);
        }

        /* join with real table */
        {
            const TString query = Q_(R"(
                    SELECT *
                    FROM `/Root/RSJ_SimpleKey_2` AS l RIGHT SEMI JOIN `/Root/RSJ_SimpleKey_3` VIEW SubKeyIndex AS r
                         ON l.Value = r.SubKey
                    ORDER BY Key
                )");

            auto result = ExecQuery(session, query, NoParams, R"([[[1];["2.One"];["Payload1"]];[[5];["2.Five"];["Payload2"]]])");
            AssertTableReads(result, "/Root/RSJ_SimpleKey_2", 5 /* all keys */);
            // todo: reading of absent |Any| key is not visible in statistics :(
            AssertTableReads(result, "/Root/RSJ_SimpleKey_3/SubKeyIndex/indexImplTable", 2 /* [2.One, 2.Five, |Any|] */);
            AssertTableReads(result, "/Root/RSJ_SimpleKey_3", 2 /* [1, 5] */);
        }
    }

    // join on complex secondary index => index-lookup
    Y_UNIT_TEST(RightSemiJoin_ComplexSecondaryIndex) {
        TKikimrRunner kikimr(SyntaxV1Settings());
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateRightSemiJoinSampleTables(session);

        /* join with parameters */
        {
            const TString query = Q_(R"(
                        DECLARE $in AS List<Struct<k: Int32?, v: String?>>;
                        SELECT *
                        FROM AS_TABLE($in) AS l RIGHT SEMI JOIN `/Root/RSJ_SecondaryKeys_1` VIEW Index AS r
                             ON l.k = r.SubKey1 AND l.v = r.SubKey2
                        ORDER BY Key
                    )");

            auto params = TParamsBuilder().AddParam("$in").BeginList()
                    .AddListItem().BeginStruct().AddMember("k").OptionalInt32(1)
                                                .AddMember("v").OptionalString("2.One").EndStruct()
                    .AddListItem().BeginStruct().AddMember("k").OptionalInt32(1)
                                                .AddMember("v").OptionalString("2.One").EndStruct()   // dup
                    .AddListItem().BeginStruct().AddMember("k").OptionalInt32(5)
                                                .AddMember("v").OptionalString("2.Five").EndStruct()
                    .AddListItem().BeginStruct().AddMember("k").OptionalInt32(42)
                                                .AddMember("v").OptionalString("Any").EndStruct()     // not exists
                    .AddListItem().BeginStruct().AddMember("k").OptionalInt32(std::nullopt)
                                                .AddMember("v").OptionalString(std::nullopt).EndStruct() // null
                    .EndList().Build().Build();

            auto result = ExecQuery(session, query, params, R"([[[1];[1];["2.One"];["Payload1"]];[[5];[5];["2.Five"];["Payload2"]]])");
            // todo: reading of absent |Any| key is not visible in statistics
            AssertTableReads(result, "/Root/RSJ_SecondaryKeys_1/Index/indexImplTable", 2 /* [2.One, 2.Five, |Any|] */);
            AssertTableReads(result, "/Root/RSJ_SecondaryKeys_1", 2 /* read .Value field */);
        }

        /* join with real table */
        {
            const TString query = Q_(R"(
                        SELECT *
                        FROM `/Root/RSJ_SimpleKey_2` AS l RIGHT SEMI JOIN `/Root/RSJ_SecondaryKeys_1` VIEW Index AS r
                             ON l.Key = r.SubKey1 AND l.Value = r.SubKey2
                        ORDER BY Key
                    )");

            auto result = ExecQuery(session, query, NoParams, R"([[[1];[1];["2.One"];["Payload1"]];[[5];[5];["2.Five"];["Payload2"]]])");
            AssertTableReads(result, "/Root/RSJ_SimpleKey_2", 5 /* all keys */);
            // todo: reading of absent |Any| key is not visible in statistics
            AssertTableReads(result, "/Root/RSJ_SecondaryKeys_1/Index/indexImplTable", 2 /* [2.One, 2.Five, |Any|] */);
            AssertTableReads(result, "/Root/RSJ_SecondaryKeys_1", 2 /* [1, 5] */);
        }

    }

    // join on secondary index prefix => index-lookup
    Y_UNIT_TEST(RightSemiJoin_ComplexSecondaryIndexPrefix) {
        TKikimrRunner kikimr(SyntaxV1Settings());
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateRightSemiJoinSampleTables(session);

        /* join with parameters */
        {
            const TString query = Q_(R"(
                DECLARE $in AS List<Struct<k: Int32?>>;
                SELECT *
                FROM AS_TABLE($in) AS l RIGHT SEMI JOIN `/Root/RSJ_SecondaryKeys_1` VIEW Index AS r
                     ON l.k = r.SubKey1
                ORDER BY Key
            )");

            auto params = TParamsBuilder().AddParam("$in").BeginList()
                    .AddListItem().BeginStruct().AddMember("k").OptionalInt32(1).EndStruct()
                    .AddListItem().BeginStruct().AddMember("k").OptionalInt32(1).EndStruct()   // dup
                    .AddListItem().BeginStruct().AddMember("k").OptionalInt32(5).EndStruct()
                    .AddListItem().BeginStruct().AddMember("k").OptionalInt32(42).EndStruct()     // not exists
                    .AddListItem().BeginStruct().AddMember("k").OptionalInt32(std::nullopt).EndStruct() // null
                    .EndList().Build().Build();

            auto result = ExecQuery(session, query, params, R"([[[1];[1];["2.One"];["Payload1"]];[[5];[5];["2.Five"];["Payload2"]]])");
            // todo: reading of absent |Any| key is not visible in statistics
            AssertTableReads(result, "/Root/RSJ_SecondaryKeys_1/Index/indexImplTable", 2 /* [2.One, 2.Five, |Any|] */);
            AssertTableReads(result, "/Root/RSJ_SecondaryKeys_1", 2 /* read .Value field */);
        }

        /* join with real table */
        {
            const TString query = Q_(R"(
                SELECT *
                FROM `/Root/RSJ_SimpleKey_2` AS l RIGHT SEMI JOIN `/Root/RSJ_SecondaryKeys_1` VIEW Index AS r
                     ON l.Key = r.SubKey1
                -- WHERE r.Key > 1
                ORDER BY Key
            )");

            auto result = ExecQuery(session, query, NoParams, R"([[[1];[1];["2.One"];["Payload1"]];[[5];[5];["2.Five"];["Payload2"]]])");
            AssertTableReads(result, "/Root/RSJ_SimpleKey_2", 5 /* all keys */);
            // todo: reading of absent |Any| key is not visible in statistics
            AssertTableReads(result, "/Root/RSJ_SecondaryKeys_1/Index/indexImplTable", 2 /* [2.One, 2.Five, |Any|] */);
            AssertTableReads(result, "/Root/RSJ_SecondaryKeys_1", 2 /* [1, 5] */);
        }
    }

    void TestInnerJoinWithPredicate(const TString& predicate, const TString& expected) {
        TKikimrRunner kikimr(SyntaxV1Settings());
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        UNIT_ASSERT(session.ExecuteSchemeQuery(R"(
            CREATE TABLE `/Root/SecondaryKeys` (
                Key Int32,
                Fk Int32,
                Value String,
                PRIMARY KEY (Key),
                INDEX Index GLOBAL ON (Fk)
            )
        )").GetValueSync().IsSuccess());

        auto result = session.ExecuteDataQuery(Q_(R"(
            REPLACE INTO `/Root/SecondaryKeys` (Key, Fk, Value) VALUES
                (1, 101, "Payload1"),
                (5, 105, "Payload2")
        )"), TTxControl::BeginTx().CommitTx()).GetValueSync();

        UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());

        const TString query = Sprintf(R"(
            DECLARE $in AS List<Struct<k: Int32?>>;
            SELECT *
            FROM AS_TABLE($in) AS l INNER JOIN `/Root/SecondaryKeys` VIEW Index AS r
                 ON l.k = r.Fk
            WHERE %s
            ORDER BY Key
        )", predicate.c_str());

        auto params = TParamsBuilder().AddParam("$in").BeginList()
                .AddListItem().BeginStruct().AddMember("k").OptionalInt32(105).EndStruct()
                .EndList().Build().Build();

        result = session.ExecuteDataQuery(Q_(query), TTxControl::BeginTx().CommitTx(), params)
                .ExtractValueSync();
        UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        CompareYson(expected, FormatResultSetYson(result.GetResultSet(0)));
    }

    Y_UNIT_TEST(RightTableKeyPredicate) {
        TestInnerJoinWithPredicate("r.Key > 1", "[[[105];[5];[\"Payload2\"];[105]]]");
    }

    Y_UNIT_TEST(RightTableIndexPredicate) {
        TestInnerJoinWithPredicate("r.Fk > 1", "[[[105];[5];[\"Payload2\"];[105]]]");
    }

    Y_UNIT_TEST(RightTableValuePredicate) {
        TestInnerJoinWithPredicate("r.Value = \"Payload2\"", "[[[105];[5];[\"Payload2\"];[105]]]");
    }

    Y_UNIT_TEST(JoinAggregateSingleRow) {
        TKikimrRunner kikimr;
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        CreateSampleTables(session);

        auto params = db.GetParamsBuilder()
            .AddParam("$key1")
                .Int32(101)
                .Build()
            .AddParam("$key2")
                .String("Two")
                .Build()
            .Build();

        auto result = session.ExecuteDataQuery(Q_(R"(
            DECLARE $key1 AS Int32;
            DECLARE $key2 AS String;

            SELECT
                j2.Key2 AS Key,
                SOME(j2.Value),
                SOME(j3.Value)
            FROM `/Root/Join1_2` AS j2
            LEFT JOIN `/Root/Join1_3` AS j3
            ON j3.Key = j2.Fk3
            WHERE j2.Key1 = $key1 AND j2.Key2 = $key2
            GROUP BY j2.Key2;
        )"), TTxControl::BeginTx().CommitTx(), params).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());

        CompareYson(R"([[["Two"];["Value22"];[1001]]])",
            FormatResultSetYson(result.GetResultSet(0)));
    }

    Y_UNIT_TEST(JoinAggregate) {
        TKikimrRunner kikimr;
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();
        CreateSampleTables(session);

        {
            auto result = session.ExecuteDataQuery(Q_(R"(
                SELECT t1.Value, SUM(t3.Value)
                FROM `/Root/Join1_1` AS t1
                INNER JOIN `/Root/Join1_2` AS t2
                ON t1.Fk21 == t2.Key1
                LEFT JOIN `/Root/Join1_3` AS t3
                ON t2.Fk3 = t3.Key
                GROUP BY t1.Value
                ORDER BY t1.Value;
            )"), TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());

            CompareYson(R"([[["Value1"];[3004]];[["Value2"];[1001]];[["Value3"];[2006]];[["Value5"];#]])",
                FormatResultSetYson(result.GetResultSet(0)));
        }

        {
            auto result = session.ExecuteDataQuery(Q_(R"(
                SELECT t1.Value, SUM(t3.Value)
                FROM `/Root/Join1_1` AS t1
                INNER JOIN `/Root/Join1_2` AS t2
                ON t1.Fk21 == t2.Key1
                LEFT JOIN `/Root/Join1_3` AS t3
                ON t2.Fk3 = t3.Key
                GROUP BY t1.Value
                ORDER BY t1.Value LIMIT 3;
            )"), TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());

            CompareYson(R"([[["Value1"];[3004]];[["Value2"];[1001]];[["Value3"];[2006]]])",
                FormatResultSetYson(result.GetResultSet(0)));
        }
    }

    Y_UNIT_TEST(JoinConvert) {
        TKikimrRunner kikimr;
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();
        CreateSampleTables(session);

        UNIT_ASSERT(session.ExecuteSchemeQuery(R"(
            CREATE TABLE `/Root/Join1_3_ui64` (
                Key String,
                Value Int64,
                PRIMARY KEY (Key)
            );
        )").GetValueSync().IsSuccess());

        UNIT_ASSERT(session.ExecuteDataQuery(Q_(R"(
            REPLACE INTO `/Root/Join1_3_ui64` (Key, Value) VALUES
                ("Name1", 108);
        )"), TTxControl::BeginTx().CommitTx()).GetValueSync().IsSuccess());

        auto result = session.ExecuteDataQuery(Q_(R"(
            SELECT t1.Value, t2.Value, t3.Value FROM `/Root/Join1_1` AS t1
            INNER JOIN `/Root/Join1_2` AS t2
            ON t1.Fk21 == t2.Key1 AND t1.Fk22 == t2.Key2
            LEFT JOIN `/Root/Join1_3_ui64` AS t3
            ON t2.Key1 = t3.Value
            WHERE t1.Value == "Value5";
        )"), TTxControl::BeginTx().CommitTx()).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());

        CompareYson(R"([[["Value5"];["Value31"];[108]]])",
            FormatResultSetYson(result.GetResultSet(0)));
    }

    Y_UNIT_TEST(ExclusionJoin) {
        TKikimrRunner kikimr;
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();
        CreateSampleTables(session);

        auto result = session.ExecuteDataQuery(Q_(R"(
            SELECT left.Fk21, left.Key, left.Value, right.Key1, right.Value
            FROM `/Root/Join1_1` as left
            EXCLUSION JOIN `/Root/Join1_2` as right
            ON left.Fk21 = right.Key1
        )"), TTxControl::BeginTx().CommitTx()).GetValueSync();

        UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        CompareYson(
            R"([[
                [107];[7];["Value4"];#;#];
                [#;#;#;[109];["Value41"]
            ]])",
            FormatResultSetYson(result.GetResultSet(0))
        );
    }

    Y_UNIT_TEST(FullOuterJoin) {
        TKikimrRunner kikimr;
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();
        CreateSampleTables(session);

        auto result = session.ExecuteDataQuery(Q_(R"(
            SELECT left.Fk21, left.Key, left.Value, right.Key1, right.Value
            FROM `/Root/Join1_1` as left
            FULL OUTER JOIN `/Root/Join1_2` as right
            ON left.Fk21 = right.Key1
            ORDER BY left.Fk21, left.Key, left.Value, right.Key1, right.Value
        )"), TTxControl::BeginTx().CommitTx()).GetValueSync();

        UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        CompareYson(
            R"([
                [#;#;#;[109];["Value41"]];
                [[101];[1];["Value1"];[101];["Value21"]];
                [[101];[1];["Value1"];[101];["Value22"]];
                [[101];[1];["Value1"];[101];["Value23"]];
                [[102];[2];["Value1"];[102];["Value24"]];
                [[103];[3];["Value2"];[103];["Value25"]];
                [[104];[4];["Value2"];[104];["Value26"]];
                [[105];[5];["Value3"];[105];["Value27"]];
                [[105];[5];["Value3"];[105];["Value28"]];
                [[106];[6];["Value3"];[106];["Value29"]];
                [[107];[7];["Value4"];#;#];
                [[108];[8];["Value5"];[108];["Value31"]]
            ])",
            FormatResultSetYson(result.GetResultSet(0))
        );
    }

    Y_UNIT_TEST(FullOuterJoin2) {
        TKikimrRunner kikimr;
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();
        CreateSampleTables(session);

        auto result = session.ExecuteDataQuery(Q_(R"(
            SELECT left.Key, left.Value, right.Value
            FROM `/Root/Join1_1` AS left
            FULL OUTER JOIN `/Root/Join1_2` AS right
            ON left.Fk21 = right.Key1 AND left.Fk22 = right.Key2
            WHERE left.Key < 5
            ORDER BY left.Key
        )"), TTxControl::BeginTx().CommitTx()).GetValueSync();

        UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        CompareYson(R"([
            [[1];["Value1"];["Value21"]];
            [[2];["Value1"];#];
            [[3];["Value2"];["Value25"]];
            [[4];["Value2"];#]
        ])", FormatResultSetYson(result.GetResultSet(0)));
    }

    Y_UNIT_TEST(FullOuterJoinSizeCheck) {
        TKikimrRunner kikimr;
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();
        CreateSampleTables(session);

        auto result = session.ExecuteDataQuery(Q_(R"(
            SELECT COUNT(*)
            FROM `/Root/Join1_1` as left
            FULL OUTER JOIN `/Root/Join1_2` as right
            ON left.Fk21 = right.Key1
            UNION ALL
            SELECT COUNT(*)
            FROM `/Root/Join1_2` as right
            FULL OUTER JOIN `/Root/Join1_1` as left
            ON left.Fk21 = right.Key1
        )"), TTxControl::BeginTx().CommitTx()).GetValueSync();

        UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        CompareYson(
                "[[12u];[12u]]", // numbers MUST be same
                FormatResultSetYson(result.GetResultSet(0))
        );
    }

    Y_UNIT_TEST(CrossJoinCount) {
        TKikimrRunner kikimr;
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();
        CreateSampleTables(session);

        auto result = session.ExecuteDataQuery(Q1_(R"(
            SELECT COUNT(*)
            FROM `/Root/Join1_1` as left
            CROSS JOIN `/Root/Join1_2` as right
        )"), TTxControl::BeginTx().CommitTx()).GetValueSync();

        UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        CompareYson(R"([[88u]])", FormatResultSetYson(result.GetResultSet(0)));
    }

    Y_UNIT_TEST(JoinDupColumnRight) {
        TKikimrRunner kikimr;
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();
        CreateSampleTables(session);

        auto result = session.ExecuteDataQuery(Q1_(R"(
            SELECT Key, Key1, Key2
            FROM `/Root/Join1_1` AS t1 LEFT JOIN `/Root/Join1_2` AS t2
                ON t1.Key = t2.Key1 AND t1.Fk21 = t2.Key1
            WHERE t1.Value == "Value1"
            ORDER BY Key;
        )"), TTxControl::BeginTx().CommitTx()).GetValueSync();

        UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        CompareYson(R"([
            [[1];#;#];
            [[2];#;#]
        ])", FormatResultSetYson(result.GetResultSet(0)));
    }

    Y_UNIT_TEST(JoinDupColumnRightPure) {
        TKikimrRunner kikimr;
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();
        CreateSampleTables(session);

        auto params = TParamsBuilder()
            .AddParam("$rows")
                .BeginList()
                    .AddListItem()
                        .BeginStruct()
                            .AddMember("Key").Int32(1)
                            .AddMember("Fk21").Int32(101)
                        .EndStruct()
                    .AddListItem()
                        .BeginStruct()
                            .AddMember("Key").Int32(2)
                            .AddMember("Fk21").Int32(102)
                        .EndStruct()
                .EndList().Build()
             .Build();

        auto result = session.ExecuteDataQuery(Q1_(R"(
            DECLARE $rows AS List<Struct<Key: Int32, Fk21: Int32>>;

            SELECT Key, Key1, Key2
            FROM AS_TABLE($rows) AS t1
            LEFT JOIN Join1_2 AS t2 ON t1.Key = t2.Key1 AND t1.Fk21 = t2.Key1
            ORDER BY Key;
        )"), TTxControl::BeginTx().CommitTx(), params).GetValueSync();

        UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        CompareYson(R"([
            [1;#;#];
            [2;#;#]
        ])", FormatResultSetYson(result.GetResultSet(0)));
    }

    Y_UNIT_TEST(JoinLeftPureInner) {
        TKikimrRunner kikimr;
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        auto result = session.ExecuteDataQuery(Q1_(R"(
            DECLARE $rows AS List<Struct<Row: Uint32, Value: String>>;

            SELECT COUNT(*)
            FROM AS_TABLE($rows) AS tl
            INNER JOIN Join1 AS tr
            ON tl.Value = tr.Value;
        )"), TTxControl::BeginTx().CommitTx(), BuildPureTableParams(db)).GetValueSync();

        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        CompareYson(R"([[5u]])", FormatResultSetYson(result.GetResultSet(0)));
    }

    Y_UNIT_TEST(JoinLeftPureInnerConverted) {
        TKikimrRunner kikimr;
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();
        CreateSampleTables(session);

        auto params = db.GetParamsBuilder()
            .AddParam("$rows")
                .BeginList()
                .AddListItem()
                    .BeginStruct()
                        .AddMember("Key").Uint8(1)
                    .EndStruct()
                .EndList()
            .Build()
        .Build();
        auto result = session.ExecuteDataQuery(Q1_(R"(
            DECLARE $rows AS List<Struct<Key: Uint8>>;

            SELECT COUNT(*)
            FROM AS_TABLE($rows) AS tl
            INNER JOIN `/Root/Join1_1` AS tr
            ON tl.Key = tr.Key;  -- Uint8 = Int32
        )"), TTxControl::BeginTx().CommitTx(), params).GetValueSync();

        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        CompareYson(R"([[1u]])", FormatResultSetYson(result.GetResultSet(0)));
    }

    Y_UNIT_TEST(JoinLeftPureFull) {
        TKikimrRunner kikimr;
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        auto result = session.ExecuteDataQuery(Q1_(R"(
            DECLARE $rows AS List<Struct<Row: Uint32, Value: String>>;

            SELECT COUNT(*)
            FROM AS_TABLE($rows) AS tl
            FULL JOIN Join1 AS tr
            ON tl.Value = tr.Value;
        )"), TTxControl::BeginTx().CommitTx(), BuildPureTableParams(db)).GetValueSync();

        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        CompareYson(R"([[11u]])", FormatResultSetYson(result.GetResultSet(0)));
    }

    Y_UNIT_TEST(JoinLeftPureExclusion) {
        TKikimrRunner kikimr;
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        auto result = session.ExecuteDataQuery(Q1_(R"(
            DECLARE $rows AS List<Struct<Row: Uint32, Value: String>>;

            SELECT COUNT(*)
            FROM AS_TABLE($rows) AS tl
            EXCLUSION JOIN Join1 AS tr
            ON tl.Value = tr.Value;
        )"), TTxControl::BeginTx().CommitTx(), BuildPureTableParams(db)).GetValueSync();

        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        CompareYson(R"([[6u]])", FormatResultSetYson(result.GetResultSet(0)));
    }

    Y_UNIT_TEST(JoinLeftPureCross) {
        TKikimrRunner kikimr;
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        auto result = session.ExecuteDataQuery(Q1_(R"(
            DECLARE $rows AS List<Struct<Row: Uint32, Value: String>>;

            SELECT COUNT(*)
            FROM AS_TABLE($rows) AS tl
            CROSS JOIN Join1 AS tr;
        )"), TTxControl::BeginTx().CommitTx(), BuildPureTableParams(db)).GetValueSync();

        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        CompareYson(R"([[36u]])", FormatResultSetYson(result.GetResultSet(0)));
    }

    Y_UNIT_TEST(JoinMismatchDictKeyTypes) {
        TKikimrRunner kikimr;
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        UNIT_ASSERT(session.ExecuteSchemeQuery(R"(
            CREATE TABLE `/Root/Join_Uint64_1` (
                Key Uint64,
                Value String,
                PRIMARY KEY (Key)
            );
        )").GetValueSync().IsSuccess());

        auto result = session.ExecuteDataQuery(Q1_(R"(
            SELECT t1.Key
            FROM
                (SELECT 2 AS Key, COUNT(*) AS Cnt FROM `/Root/Join_Uint64_1`) AS t1
            LEFT JOIN
            AS_TABLE(
                AsList(AsStruct(2u AS Key1, "Val1" AS Value))
            ) AS t2
            ON t1.Key = t2.Key1
        )"), TTxControl::BeginTx().CommitTx()).GetValueSync();

        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        CompareYson(R"([[2]])", FormatResultSetYson(result.GetResultSet(0)));
    }

    Y_UNIT_TEST(FullOuterJoinNotNullJoinKey) {
        TKikimrRunner kikimr;
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {  // init tables
            AssertSuccessResult(session.ExecuteSchemeQuery(R"(
                --!syntax_v1

                CREATE TABLE left
                (
                    Key Int64 NOT NULL,
                    Value Int64,
                    PRIMARY KEY (Key)
                );

                CREATE TABLE right
                (
                    Key Int64 NOT NULL,
                    Value Int64,
                    PRIMARY KEY (Key)
                );
            )").GetValueSync());

            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                REPLACE INTO left (Key, Value) VALUES
                    (1, 10),
                    (2, 20),
                    (3, 30);

                REPLACE INTO right (Key, Value) VALUES
                    (1, 10),
                    (2, 200),
                    (3, 300),
                    (4, 40);
            )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        {
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT l.Key, l.Value, r.Key, r.Value FROM left as l FULL JOIN right as r
                    ON (l.Value = r.Value AND l.Key = r.Key)
                    ORDER BY l.Key, r.Key;
            )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [#;#;[2];[200]];
                [#;#;[3];[300]];
                [#;#;[4];[40]];
                [[1];[10];[1];[10]];
                [[2];[20];#;#];
                [[3];[30];#;#]
            ])", FormatResultSetYson(result.GetResultSet(0)));
        }
    }

    Y_UNIT_TEST_TWIN(AllowJoinsForComplexPredicates, StreamLookup) {
        TKikimrSettings appsettings;
        appsettings.AppConfig.MutableTableServiceConfig()->SetEnableKqpDataQueryStreamIdxLookupJoin(StreamLookup);
        appsettings.AppConfig.MutableTableServiceConfig()->SetIdxLookupJoinPointsLimit(10);

        TKikimrRunner kikimr(appsettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        NYdb::NTable::TExecDataQuerySettings settings;
        settings.CollectQueryStats(ECollectQueryStatsMode::Profile);

        {
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT l.Fk21, l.Fk22, r.Key1, r.Key2, r.Name  FROM Join1 as l JOIN Join2 as r
                    ON (l.Fk21 = r.Key1 AND l.Fk22 = r.Key2)
                WHERE r.Key1 > 0 and r.Name > ""
                    ORDER BY l.Fk21 ASC, l.Fk22 ASC
            )", TTxControl::BeginTx().CommitTx(), settings).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[101u];["One"];[101u];["One"];["Name1"]];
                [[101u];["Two"];[101u];["Two"];["Name1"]];
                [[103u];["One"];[103u];["One"];["Name1"]];
                [[105u];["One"];[105u];["One"];["Name2"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
            AssertTableReads(result, "/Root/Join2", 5);
            UNIT_ASSERT(result.GetQueryPlan().contains("Lookup"));
        }

        {
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT l.Fk21, l.Fk22, r.Key1, r.Key2, r.Name  FROM Join1 as l JOIN Join2 as r
                    ON (l.Fk21 = r.Key1 AND l.Fk22 = r.Key2)
                WHERE r.Key1 = 101u and r.Key2 >= "One" and r.Key2 <= "Two"
                    ORDER BY l.Fk21 ASC, l.Fk22 ASC
            )", TTxControl::BeginTx().CommitTx(), settings).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[101u];["One"];[101u];["One"];["Name1"]];
                [[101u];["Two"];[101u];["Two"];["Name1"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
            AssertTableReads(result, "/Root/Join2", 2);
            UNIT_ASSERT(result.GetQueryPlan().contains("Lookup"));
        }

        {
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT l.Fk21, l.Fk22, r.Key1, r.Key2, r.Name  FROM Join1 as l JOIN Join2 as r
                    ON (l.Fk21 = r.Key1 AND l.Fk22 = r.Key2)
                WHERE r.Key1 >= 101u and r.Key1 <= 103u and r.Key2 >= "One" and r.Key2 <= "Two"
                    ORDER BY l.Fk21 ASC, l.Fk22 ASC
            )", TTxControl::BeginTx().CommitTx(), settings).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[101u];["One"];[101u];["One"];["Name1"]];
                [[101u];["Two"];[101u];["Two"];["Name1"]];
                [[103u];["One"];[103u];["One"];["Name1"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
            AssertTableReads(result, "/Root/Join2", 3);
            UNIT_ASSERT(result.GetQueryPlan().contains("Lookup"));
        }

        {
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT l.Fk21, l.Fk22, r.Key1, r.Key2, r.Name  FROM Join1 as l JOIN Join2 as r
                    ON (l.Fk21 = r.Key1 AND l.Fk22 = r.Key2)
                WHERE r.Key1 = 101u or r.Key1 = 105u
                    ORDER BY l.Fk21 ASC, l.Fk22 ASC
            )", TTxControl::BeginTx().CommitTx(), settings).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[101u];["One"];[101u];["One"];["Name1"]];
                [[101u];["Two"];[101u];["Two"];["Name1"]];
                [[105u];["One"];[105u];["One"];["Name2"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
            AssertTableReads(result, "/Root/Join2", 3);
            UNIT_ASSERT(result.GetQueryPlan().contains("Lookup"));
        }

        {
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                SELECT l.Fk21, l.Fk22, r.Key1, r.Key2, r.Name  FROM Join1 as l JOIN Join2 as r
                    ON (l.Fk21 = r.Key1 AND l.Fk22 = r.Key2)
                WHERE (r.Key1 = 101u  AND r.Key2 = "One") OR r.Key1 = 105u
                    ORDER BY l.Fk21 ASC, l.Fk22 ASC
            )", TTxControl::BeginTx().CommitTx(), settings).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [[101u];["One"];[101u];["One"];["Name1"]];
                [[105u];["One"];[105u];["One"];["Name2"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
            AssertTableReads(result, "/Root/Join2", 2);
            UNIT_ASSERT(result.GetQueryPlan().contains("Lookup"));
        }
    }

    Y_UNIT_TEST(ComplexJoin) {
        TKikimrRunner kikimr;
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {  // init tables
            AssertSuccessResult(session.ExecuteSchemeQuery(R"(
                --!syntax_v1

                CREATE TABLE Input1
                (
                    k1 String,
                    v1 String,
                    u1 String,
                    PRIMARY KEY (k1, v1, u1)
                );

                CREATE TABLE Input2
                (
                    k2 String,
                    v2 String,
                    u2 String,
                    PRIMARY KEY (k2, v2, u2)
                );

                CREATE TABLE Input3
                (
                    k3 String,
                    v3 String,
                    u3 String,
                    PRIMARY KEY (k3, v3, u3)
                );

                CREATE TABLE Input4
                (
                    k4 String,
                    v4 String,
                    u4 String,
                    PRIMARY KEY (k4, v4, u4)
                );

                CREATE TABLE Input5
                (
                    k5 String,
                    v5 String,
                    u5 String,
                    PRIMARY KEY (k5, v5, u5)
                );
            )").GetValueSync());

            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                REPLACE INTO Input1 (k1, v1, u1) VALUES
                    ("01","1","01"),
                    ("01","2","02"),
                    ("02","1","03"),
                    ("02","2","05"),
                    ("02","2","06"),
                    ("03",NULL,"07"),
                    ("03","1","08"),
                    ("03","2","09"),
                    ("04","1","10"),
                    ("04","2","11"),
                    ("05","1","12"),
                    ("05","2","13"),
                    ("06","1","14"),
                    ("06","2","15"),
                    ("07","1","16"),
                    ("07","2","17"),
                    ("08","1","18"),
                    ("08","2","19"),
                    ("09","1","20"),
                    ("09","2","21"),
                    ("10","1","22"),
                    ("10","2","23");

                REPLACE INTO Input2 (k2, v2, u2) VALUES
                    ("02","1","01"),
                    ("02","1","02"),
                    ("02","2","03"),
                    ("02","2","04"),
                    ("03","1","05"),
                    ("03","2","06"),
                    ("04","1","07"),
                    ("04","2","08"),
                    ("05","1","09"),
                    ("05","2","10"),
                    ("06","1","11"),
                    ("06","2","12"),
                    ("07","1","13"),
                    ("07","2","14"),
                    ("09","1","15"),
                    ("09","2","16");

                REPLACE INTO Input3 (k3, v3, u3) VALUES
                    (NULL,"1","01"),
                    (NULL,"2","02"),
                    ("04","1","03"),
                    ("04","2","04"),
                    ("05","1","05"),
                    ("05","2","06");

                REPLACE INTO Input4 (k4, v4, u4) VALUES
                    ("03",NULL,"01"),
                    ("03","1","02"),
                    ("03","2","03"),
                    ("04","1","04"),
                    ("04","2","05"),
                    ("05","1","06"),
                    ("05","2","07"),
                    ("06","1","08"),
                    ("06","2","09"),
                    ("07","1","10"),
                    ("07","2","11"),
                    ("08","1","12"),
                    ("08","2","13");

                REPLACE INTO Input5 (k5, v5, u5) VALUES
                    (NULL,"1","01"),
                    (NULL,"2","02"),
                    ("02","1","03"),
                    ("02","1","04"),
                    ("02","2","05"),
                    ("02","2","06"),
                    ("03","1","07"),
                    ("03","2","08"),
                    ("09","1","09"),
                    ("09","2","10");
            )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        {
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1

                    pragma ydb.OptOverrideStatistics = '{"/Root/Input1": {"n_rows":10000}, "/Root/Input2" : {"n_rows":10000}, "/Root/Input3":{"n_rows":10000}, "/Root/Input4":{"n_rows":10000}, "/Root/Input5":{"n_rows":10000}}';

                    $rightSemi = select * from Input2 as b right semi join Input1 as a on a.v1 = b.v2 and a.k1 = b.k2;
                    $leftOnly = select * from $rightSemi as rs left only join Input3 as c on rs.k1 = c.k3 and rs.v1 = c.v3;
                    $right = select * from Input4 as d right join $leftOnly as lo on d.v4 = lo.v1 and lo.k1 = d.k4;
                    $inner = select * from $right as r join any Input5 as e on r.k1 = e.k5 and e.v5 = r.v1;

                    select * from $inner order by u1,u5;
            )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            Cout << FormatResultSetYson(result.GetResultSet(0));
            CompareYson(R"(
            [[["02"];#;["02"];["03"];#;["03"];["1"];#;["1"]];[["02"];#;["02"];["05"];#;["05"];["2"];#;["2"]];[["02"];#;["02"];["06"];#;["05"];["2"];#;["2"]];[["03"];["03"];["03"];["08"];["02"];["07"];["1"];["1"];["1"]];[["03"];["03"];["03"];["09"];["03"];["08"];["2"];["2"];["2"]];[["09"];#;["09"];["20"];#;["09"];["1"];#;["1"]];[["09"];#;["09"];["21"];#;["10"];["2"];#;["2"]]]
            )", FormatResultSetYson(result.GetResultSet(0)));
        }
    }

    Y_UNIT_TEST(HashJoinWithAsTable) {
        TKikimrRunner kikimr;

        auto client = kikimr.GetQueryClient();

        {
            const TString query = R"(
                CREATE TABLE test_table (
                    test_column Int32,
                    PRIMARY key (test_column)
                ))";

                const auto result = client.ExecuteQuery(query, NYdb::NQuery::TTxControl::NoTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NYdb::EStatus::SUCCESS, result.GetIssues().ToString());
        }

        const TString joinQuery = R"(
            PRAGMA ydb.HashJoinMode = "grace";
            PRAGMA ydb.OptShuffleElimination = "true";

            $as_table = SELECT * FROM AS_TABLE([<|test_column: 42|>]);

            SELECT
                as_table.test_column
            FROM $as_table AS as_table
            LEFT JOIN test_table
                ON test_table.test_column = as_table.test_column
        )";

        const auto result = client.ExecuteQuery(joinQuery, NYdb::NQuery::TTxControl::NoTx()).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NYdb::EStatus::SUCCESS, result.GetIssues().ToString());
        UNIT_ASSERT_VALUES_EQUAL(result.GetResultSets().size(), 1);

        const auto& resultSet = result.GetResultSet(0);
        UNIT_ASSERT_VALUES_EQUAL(resultSet.ColumnsCount(), 1);
        UNIT_ASSERT_VALUES_EQUAL(resultSet.RowsCount(), 1);

        TResultSetParser parser(resultSet);
        UNIT_ASSERT(parser.TryNextRow());
        UNIT_ASSERT_VALUES_EQUAL(parser.ColumnParser(0).GetInt32(), 42);
    }

    Y_UNIT_TEST(PushdownPredicateNoFullScan) {
        NKikimrConfig::TAppConfig appCfg;
        TKikimrRunner kikimr(appCfg);

        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            auto res1 = session.ExecuteSchemeQuery(R"(
                CREATE TABLE `/Root/TableRight` (
                    id Uint64,
                    value Utf8,
                    PRIMARY KEY (id)
                );
            )").GetValueSync();
            UNIT_ASSERT_C(res1.IsSuccess(), res1.GetIssues().ToString());

            auto res2 = session.ExecuteSchemeQuery(R"(
                CREATE TABLE `/Root/TableLeft` (
                    hash_key Uint64,
                    ref_id Uint64,
                    data Utf8,
                    PRIMARY KEY (hash_key)
                );
            )").GetValueSync();
            UNIT_ASSERT_C(res2.IsSuccess(), res2.GetIssues().ToString());
        }

        // Digest::MurMurHash(Utf8("target")) = 9488119898155926451
        const ui64 hashValue = 9488119898155926451ULL;

        {
            auto result = session.ExecuteDataQuery(R"(
                --!syntax_v1
                UPSERT INTO `/Root/TableRight` (id, value) VALUES
                    (1, "one"), (2, "two"), (3, "three");
            )", TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());

            auto result2 = session.ExecuteDataQuery(Sprintf(R"(
                --!syntax_v1
                UPSERT INTO `/Root/TableLeft` (hash_key, ref_id, data) VALUES
                    (%llu, 2, "match"),
                    (%llu, 1, "no_match_1"),
                    (%llu, 3, "no_match_2");
            )", hashValue, hashValue + 1, hashValue + 2), TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT(result2.IsSuccess());
        }

        const TString query = R"(
            SELECT r.value FROM `/Root/TableLeft` AS l
            INNER JOIN `/Root/TableRight` AS r ON l.ref_id = r.id
            WHERE l.hash_key = Digest::MurMurHash(Utf8("target"));
        )";

        NYdb::NTable::TExecDataQuerySettings settings;
        settings.CollectQueryStats(ECollectQueryStatsMode::Profile);

        auto result = session.ExecuteDataQuery(query, TTxControl::BeginTx().CommitTx(), settings).ExtractValueSync();
        UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());

        const auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());
        Cerr << stats.DebugString() << Endl;

        bool leftTableChecked = false;
        bool rightTableChecked = false;
        for (const auto& phase : stats.query_phases()) {
            for (const auto& access : phase.table_access()) {
                if (access.name() == "/Root/TableLeft") {
                    UNIT_ASSERT_VALUES_EQUAL(access.reads().rows(), 1);
                    leftTableChecked = true;
                }
                if (access.name() == "/Root/TableRight") {
                     UNIT_ASSERT_VALUES_EQUAL(access.reads().rows(), 1);
                    rightTableChecked = true;
                }
            }
        }

        UNIT_ASSERT_C(leftTableChecked, "No reads found for /Root/TableLeft");
        UNIT_ASSERT_C(rightTableChecked, "No reads found for /Root/TableRight");
    }
}

} // namespace NKqp
} // namespace NKikimr
