// The OTLP metrics transport (spec 003): one tick of the scrape becomes one `ResourceMetrics` and
// one Export call, over the same endpoint, protocol and headers as the logs of spec 002. No SDK
// MeterProvider and no periodic reader - the timer is ours, and the base's numbers are the numbers.

#include "acl_otel_metrics.hpp"
#include "acl_otel_otlp.hpp"

#include "duckdb/common/exception.hpp"

#include "opentelemetry/exporters/otlp/otlp_grpc_metric_exporter_factory.h"
#include "opentelemetry/exporters/otlp/otlp_grpc_metric_exporter_options.h"
#include "opentelemetry/exporters/otlp/otlp_http_metric_exporter_factory.h"
#include "opentelemetry/exporters/otlp/otlp_http_metric_exporter_options.h"
#include "opentelemetry/sdk/instrumentationscope/instrumentation_scope.h"
#include "opentelemetry/sdk/metrics/export/metric_producer.h"
#include "opentelemetry/sdk/metrics/push_metric_exporter.h"
#include "opentelemetry/sdk/resource/resource.h"

#include <chrono>

namespace duckdb {
namespace acl_otel {

namespace otlp = opentelemetry::exporter::otlp;
namespace sdkmetrics = opentelemetry::sdk::metrics;

namespace {

sdkmetrics::PointAttributes AttributesOf(const Labels &labels) {
	sdkmetrics::PointAttributes attributes;
	for (auto &label : labels) {
		attributes.SetAttribute(label.first, label.second);
	}
	return attributes;
}

opentelemetry::common::SystemTimestamp Stamp(int64_t micros) {
	return opentelemetry::common::SystemTimestamp(std::chrono::microseconds(micros));
}

} // namespace

//! The OTLP metric exporter: the metrics twin of spec 002's OtlpExporter, built from the same
//! OtlpConfig so an operator configures one endpoint and gets both signals.
class OtlpMetricsExporter : public MetricsExporter {
public:
	explicit OtlpMetricsExporter(const OtlpConfig &config);
	~OtlpMetricsExporter() override;

	bool Export(const MetricsSnapshot &snapshot, string &error) override;
	string Describe() const override;

private:
	string description;
	std::unique_ptr<sdkmetrics::PushMetricExporter> exporter;
	unique_ptr<opentelemetry::sdk::resource::Resource> resource;
	unique_ptr<opentelemetry::sdk::instrumentationscope::InstrumentationScope> scope;
};

OtlpMetricsExporter::OtlpMetricsExporter(const OtlpConfig &config) {
	if (!OtlpConfig::ValidProtocol(config.protocol)) {
		throw InvalidInputException("acl_otel_protocol accepts 'http/protobuf' or 'grpc', not '%s'", config.protocol);
	}
	resource = make_uniq<opentelemetry::sdk::resource::Resource>(ResourceOf(config));
	auto created = opentelemetry::sdk::instrumentationscope::InstrumentationScope::Create(
	    "acl_otel", config.acl_otel_version.empty() ? "dev" : config.acl_otel_version);
	scope = unique_ptr<opentelemetry::sdk::instrumentationscope::InstrumentationScope>(created.release());
	auto timeout = std::chrono::seconds(config.timeout_s);
	if (config.ResolvedProtocol() == "grpc") {
		otlp::OtlpGrpcMetricExporterOptions options;
		if (!config.endpoint.empty()) {
			options.endpoint = config.endpoint;
		}
		if (config.timeout_s > 0) {
			options.timeout = timeout;
		}
		if (config.insecure) {
			options.use_ssl_credentials = false;
		} else if (!config.certificate.empty()) {
			options.use_ssl_credentials = true;
			options.ssl_credentials_cacert_path = config.certificate;
		}
		description = "otlp/grpc -> " + MaskUserinfo(options.endpoint);
		exporter = otlp::OtlpGrpcMetricExporterFactory::Create(options);
	} else {
		otlp::OtlpHttpMetricExporterOptions options;
		if (!config.endpoint.empty()) {
			options.url = SignalUrl(config.endpoint, "/v1/metrics");
		}
		if (config.timeout_s > 0) {
			options.timeout = timeout;
		}
		if (!config.certificate.empty()) {
			options.ssl_ca_cert_path = config.certificate;
		}
		description = "otlp/http -> " + MaskUserinfo(options.url);
		exporter = otlp::OtlpHttpMetricExporterFactory::Create(options);
	}
}

OtlpMetricsExporter::~OtlpMetricsExporter() {
	if (exporter) {
		exporter->Shutdown(std::chrono::seconds(2));
	}
}

string OtlpMetricsExporter::Describe() const {
	return description;
}

bool OtlpMetricsExporter::Export(const MetricsSnapshot &snapshot, string &error) {
	vector<sdkmetrics::MetricData> metrics;
	metrics.reserve(snapshot.points.size() + snapshot.histograms.size());
	// one MetricData per instrument name; points of the same name share it, which is what a backend
	// expects (a series is a name plus its attributes)
	std::map<string, idx_t> by_name;
	for (auto &point : snapshot.points) {
		auto found = by_name.find(point.name);
		if (found == by_name.end()) {
			sdkmetrics::MetricData data;
			data.instrument_descriptor = {point.name, point.description, point.unit,
			                              point.monotonic ? sdkmetrics::InstrumentType::kCounter
			                                              : sdkmetrics::InstrumentType::kObservableGauge,
			                              sdkmetrics::InstrumentValueType::kLong};
			data.aggregation_temporality = sdkmetrics::AggregationTemporality::kCumulative;
			data.start_ts = Stamp(snapshot.start_us);
			data.end_ts = Stamp(snapshot.now_us);
			by_name[point.name] = metrics.size();
			metrics.push_back(std::move(data));
			found = by_name.find(point.name);
		}
		sdkmetrics::PointDataAttributes entry;
		entry.attributes = AttributesOf(point.labels);
		if (point.monotonic) {
			sdkmetrics::SumPointData sum;
			sum.value_ = point.value;
			sum.is_monotonic_ = true;
			entry.point_data = sum; // a trivially copyable SDK point: a move would be a copy anyway
		} else {
			sdkmetrics::LastValuePointData value;
			value.value_ = point.value;
			value.is_lastvalue_valid_ = true;
			value.sample_ts_ = Stamp(snapshot.now_us);
			entry.point_data = value;
		}
		metrics[found->second].point_data_attr_.push_back(std::move(entry));
	}
	for (auto &histogram : snapshot.histograms) {
		if (histogram.points.empty()) {
			continue; // an instrument nothing was recorded into is not a series
		}
		sdkmetrics::MetricData data;
		data.instrument_descriptor = {histogram.name, histogram.description, histogram.unit,
		                              sdkmetrics::InstrumentType::kHistogram, sdkmetrics::InstrumentValueType::kDouble};
		data.aggregation_temporality = sdkmetrics::AggregationTemporality::kCumulative;
		data.start_ts = Stamp(snapshot.start_us);
		data.end_ts = Stamp(snapshot.now_us);
		for (auto &point : histogram.points) {
			sdkmetrics::PointDataAttributes entry;
			entry.attributes = AttributesOf(point.first);
			sdkmetrics::HistogramPointData data_point(histogram.bounds);
			data_point.counts_.assign(point.second.counts.begin(), point.second.counts.end());
			data_point.count_ = NumericCast<uint64_t>(point.second.count);
			data_point.sum_ = point.second.sum;
			data_point.min_ = point.second.min;
			data_point.max_ = point.second.max;
			data_point.record_min_max_ = true;
			entry.point_data = std::move(data_point);
			data.point_data_attr_.push_back(std::move(entry));
		}
		metrics.push_back(std::move(data));
	}
	sdkmetrics::ResourceMetrics resource_metrics;
	resource_metrics.resource_ = resource.get();
	// emplace, never an initializer list: a PointDataAttributes cannot be copy-assigned, and a list
	// copies its elements
	resource_metrics.scope_metric_data_.emplace_back(scope.get(), std::move(metrics));
	TakeSdkError(); // what the SDK says about THIS export, not an earlier one (spec 002's handler)
	auto result = exporter->Export(resource_metrics);
	auto why = TakeSdkError();
	if (result == opentelemetry::sdk::common::ExportResult::kSuccess && why.empty()) {
		error.clear();
		return true;
	}
	error = "the export failed (" + description + ")" + (why.empty() ? "" : ": " + why);
	return false;
}

shared_ptr<MetricsExporter> MakeOtlpMetricsExporter(const OtlpConfig &config) {
	return make_shared_ptr<OtlpMetricsExporter>(config);
}

} // namespace acl_otel
} // namespace duckdb
