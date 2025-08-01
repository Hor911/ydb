#include "config.h"

#include <library/cpp/cron_expression/cron_expression.h>

namespace NYT::NQueueClient {

////////////////////////////////////////////////////////////////////////////////

void TPartitionReaderConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("max_row_count", &TThis::MaxRowCount)
        .Default(1000);
    registrar.Parameter("max_data_weight", &TThis::MaxDataWeight)
        .Default(16_MB);
    registrar.Parameter("data_weight_per_row_hint", &TThis::DataWeightPerRowHint)
        .Default();

    registrar.Parameter("use_native_tablet_node_api", &TThis::UseNativeTabletNodeApi)
        .Default(false);
    registrar.Parameter("use_pull_queue_consumer", &TThis::UsePullQueueConsumer)
        .Alias("use_pull_consumer")
        .Default(false);

    registrar.Postprocessor([] (TThis* config) {
        if (config->UsePullQueueConsumer && !config->UseNativeTabletNodeApi) {
            THROW_ERROR_EXCEPTION("PullQueueConsumer can only be used with the native tablet node api for pulling rows");
        }
    });
}

////////////////////////////////////////////////////////////////////////////////

void TQueueAutoTrimConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("enable", &TThis::Enable)
        .Default(false);
    registrar.Parameter("retained_rows", &TThis::RetainedRows)
        .Default();
    registrar.Parameter("retained_lifetime_duration", &TThis::RetainedLifetimeDuration)
        .Default();

    registrar.Postprocessor([] (TThis* trimConfig) {
        if (trimConfig->RetainedLifetimeDuration &&
            trimConfig->RetainedLifetimeDuration->GetValue() % TDuration::Seconds(1).GetValue() != 0)
        {
            THROW_ERROR_EXCEPTION("The value of \"retained_lifetime_duration\" must be a multiple of 1000 (1 second)");
        }
    });
}

bool operator==(const TQueueAutoTrimConfig& lhs, const TQueueAutoTrimConfig& rhs)
{
    return std::tie(lhs.Enable, lhs.RetainedRows, lhs.RetainedLifetimeDuration) ==
        std::tie(rhs.Enable, rhs.RetainedRows, rhs.RetainedLifetimeDuration);
}

////////////////////////////////////////////////////////////////////////////////

void TQueueStaticExportConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("export_period", &TThis::ExportPeriod)
        .GreaterThan(TDuration::Zero())
        .Optional();
    registrar.Parameter("export_cron_schedule", &TThis::ExportCronSchedule)
        .NonEmpty()
        .Optional();
    registrar.Parameter("export_directory", &TThis::ExportDirectory);
    registrar.Parameter("export_ttl", &TThis::ExportTtl)
        .Default(TDuration::Zero());
    registrar.Parameter("output_table_name_pattern", &TThis::OutputTableNamePattern)
        .Default("%UNIX_TS-%PERIOD");
    registrar.Parameter("use_upper_bound_for_table_names", &TThis::UseUpperBoundForTableNames)
        .Default(false);

    registrar.Postprocessor([] (TThis* config) {
        if (config->ExportPeriod && config->ExportCronSchedule) {
            THROW_ERROR_EXCEPTION("Both \"export_period\" and \"export_cron_schedule\" cannot be set at the same time");
        }

        if (config->ExportPeriod) {
            if (config->ExportPeriod->GetValue() % TDuration::Seconds(1).GetValue() != 0) {
                THROW_ERROR_EXCEPTION("The value of \"export_period\" must be a multiple of 1000 (1 second)");
            }
        } else if (config->ExportCronSchedule) {
            try {
                TCronExpression{*config->ExportCronSchedule};
            } catch (const std::exception& ex) {
                THROW_ERROR_EXCEPTION("Export CRON schedule %Qv is not well-formed", *config->ExportCronSchedule)
                    << ex;
            }
        } else {
            THROW_ERROR_EXCEPTION("One of \"export_period\", \"export_cron_schedule\" must be specified");
        }
    });
}

bool operator==(const TQueueStaticExportConfig& lhs, const TQueueStaticExportConfig& rhs)
{
    return std::tie(lhs.ExportPeriod, lhs.ExportCronSchedule, lhs.ExportDirectory) ==
        std::tie(rhs.ExportPeriod, lhs.ExportCronSchedule, rhs.ExportDirectory);
}

////////////////////////////////////////////////////////////////////////////////

void TQueueStaticExportDestinationConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("originating_queue_id", &TThis::OriginatingQueueId)
        .Default();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NQueueClient
