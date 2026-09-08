// The per-instance state (spec 001, R8): where the sink and the policy live, how they attach to the
// base's registry and detach from it, and what acl_otel_status() reports.

#include "acl_otel.hpp"
#include "acl_otel_metrics.hpp"
#include "acl_otel_otlp.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/config.hpp"

#include <chrono>
#include <cstdlib>

namespace duckdb {
namespace acl_otel {

namespace {

string JsonQuote(const string &value) {
	string out = "\"";
	for (auto c : value) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (static_cast<unsigned char>(c) < 0x20) {
				out += StringUtil::Format("\\u%04x", static_cast<int>(static_cast<unsigned char>(c)));
			} else {
				out += c;
			}
		}
	}
	return out + "\"";
}

} // namespace

string SettingString(DatabaseInstance &db, const char *name, const string &fallback) {
	Value value;
	if (db.TryGetCurrentSetting(name, value) && !value.IsNull()) {
		return value.ToString();
	}
	return fallback;
}

int64_t SettingInt64(DatabaseInstance &db, const char *name, int64_t fallback) {
	Value value;
	if (db.TryGetCurrentSetting(name, value) && !value.IsNull()) {
		return value.GetValue<int64_t>();
	}
	return fallback;
}

shared_ptr<OtelState> OtelState::Of(DatabaseInstance &db) {
	return db.GetObjectCache().GetOrCreate<OtelState>(ObjectType());
}

OtelState::~OtelState() {
	Stop();
}

shared_ptr<OtelSink> OtelState::Sink() {
	std::lock_guard<std::mutex> guard(lock);
	return sink;
}

shared_ptr<OtelPolicy> OtelState::Policy() {
	std::lock_guard<std::mutex> guard(lock);
	return policy;
}

bool OtelState::Attached() {
	std::lock_guard<std::mutex> guard(lock);
	return attached;
}

bool OtelState::Start(DatabaseInstance &db) {
	std::lock_guard<std::mutex> guard(lock);
	if (attached) {
		return false;
	}
	// the registry: whoever comes first creates it, the base adopts it when it loads (C2) - unless
	// it was stamped with another contract version (C2a: a base built from another acl_audit.hpp
	// created it first): then nothing of ours goes on it, and the status says so
	string why;
	hooks = acl::AuditHooks::Reach(db.GetObjectCache(), why);
	if (!hooks) {
		attach_error = why;
		return false;
	}
	attach_error.clear();
	auto queue = SettingInt64(db, "acl_otel_queue_size", 10000);
	auto batch = SettingInt64(db, "acl_otel_batch_size", 512);
	auto flush = SettingInt64(db, "acl_otel_flush_interval", 5);
	vector<string> names;
	auto exporter = BuildExporter(db, string(), Value(), names);
	header_names = std::move(names);
	sink = make_shared_ptr<OtelSink>(NumericCast<idx_t>(MaxValue<int64_t>(queue, 1)),
	                                 NumericCast<idx_t>(MaxValue<int64_t>(batch, 1)),
	                                 MaxValue<int64_t>(flush, 1) * 1000, std::move(exporter));
	if (!policy) {
		policy = make_shared_ptr<OtelPolicy>();
		policy->SetRules(ParseLevelRules(rules_json));
	}
	// spec 003: the metrics side, when the operator has not turned it off. Its transport is the
	// same endpoint, protocol and headers as the logs'; its timer is its own.
	Value on;
	bool metrics_on = !db.TryGetCurrentSetting("acl_otel_metrics", on) || on.IsNull() || on.GetValue<bool>();
	if (metrics_on) {
		auto histograms = DefaultHistograms();
		ApplyBucketDocument(histograms, SettingString(db, "acl_otel_histogram_buckets", ""));
		metrics = make_shared_ptr<OtelMetrics>(SettingInt64(db, "acl_otel_metrics_interval", 15), std::move(histograms),
		                                       BuildMetricsExporter(db, string(), Value()));
		ApplySeriesSettings(db, *metrics);
		sink->SetMetrics(metrics);
		metrics->Start(hooks);
	}
	hooks->AddSink(sink);
	hooks->SetSessionPolicy(policy);
	attached = true;
	return true;
}

//! `acl_otel_series` / `_claim_dimension` / `_max_series` / `_series_allowlist` in one place: the
//! opt-in series of R2.3, rebuilt whenever one of them changes.
void OtelState::ApplySeriesSettings(DatabaseInstance &db, OtelMetrics &target) {
	ReconfigureSeries(db, string(), Value(), target);
}

//! the settings as they will be after this SET (a callback runs before the value is stored), on the
//! running scrape - the series are rebuilt, so what was counted under the old shape is dropped and
//! the status says how many series there are now
void OtelState::ReconfigureSeries(DatabaseInstance &db, const string &changed, const Value &value) {
	shared_ptr<OtelMetrics> current;
	{
		std::lock_guard<std::mutex> guard(lock);
		current = metrics;
	}
	if (current) {
		ReconfigureSeries(db, changed, value, *current);
	}
}

void OtelState::ReconfigureSeries(DatabaseInstance &db, const string &changed, const Value &value,
                                  OtelMetrics &target) {
	auto text = [&](const char *setting, const char *fallback) {
		if (changed == setting) {
			return value.IsNull() ? string() : value.ToString();
		}
		return SettingString(db, setting, fallback);
	};
	auto names = StringUtil::Split(text("acl_otel_series", ""), ',');
	for (auto &name : names) {
		StringUtil::Trim(name);
	}
	auto claim = text("acl_otel_claim_dimension", "");
	auto cap = changed == "acl_otel_max_series" ? (value.IsNull() ? 1000 : value.GetValue<int64_t>())
	                                            : SettingInt64(db, "acl_otel_max_series", 1000);
	target.SetSeries(names, claim, NumericCast<idx_t>(MaxValue<int64_t>(cap, 1)),
	                 ParseSeriesAllowlist(text("acl_otel_series_allowlist", "")));
}

void OtelState::SetHistogramSums(bool on) {
	shared_ptr<OtelMetrics> current;
	{
		std::lock_guard<std::mutex> guard(lock);
		current = metrics;
	}
	if (current) {
		current->SetHistogramSums(on);
	}
}

bool OtelState::FlushMetrics() {
	shared_ptr<OtelMetrics> current;
	shared_ptr<acl::AuditHooks> registry;
	{
		std::lock_guard<std::mutex> guard(lock);
		current = metrics;
		registry = hooks;
	}
	return current && registry && current->TickNow(*registry);
}

bool OtelState::Stop() {
	shared_ptr<OtelSink> ending;
	shared_ptr<OtelMetrics> ending_metrics;
	{
		std::lock_guard<std::mutex> guard(lock);
		if (!attached) {
			return false;
		}
		attached = false;
		if (hooks) {
			hooks->RemoveSink(sink);
			hooks->SetSessionPolicy(nullptr);
		}
		ending = std::move(sink);
		if (metrics) {
			ending_metrics = std::move(metrics);
		}
	}
	if (ending_metrics) {
		ending_metrics->Stop(); // outside the lock: its thread may be mid-export
	}
	// outside the lock: the worker may be mid-export, and Stop waits for it
	if (ending) {
		ending->Flush();
		ending->Stop();
	}
	return true;
}

string OtelState::AttachError() {
	std::lock_guard<std::mutex> guard(lock);
	return attach_error;
}

bool OtelState::Flush() {
	shared_ptr<OtelSink> current;
	{
		std::lock_guard<std::mutex> guard(lock);
		if (!attached) {
			return false;
		}
		current = sink;
	}
	return current && current->FlushNow();
}

void OtelState::SetRulesJson(const string &json) {
	auto rules = ParseLevelRules(json); // refused here, at the SET, never at a session open
	std::lock_guard<std::mutex> guard(lock);
	rules_json = json;
	if (!policy) {
		policy = make_shared_ptr<OtelPolicy>();
	}
	policy->SetRules(std::move(rules));
}

//! spec 002: an OTLP transport when an endpoint is configured - by a setting, or by the standard
//! environment alone (a container that sets OTEL_EXPORTER_OTLP_ENDPOINT exports without a SET);
//! otherwise the stand-in that counts. The SDK's own default (localhost:4318) is deliberately NOT
//! an endpoint: nothing configured means nothing exported, said so.
namespace {

//! The settings as they will be after this SET: a callback runs BEFORE the value is stored, so the
//! one being set is handed in explicitly. Shared by both transports.
OtlpConfig ConfigAfter(DatabaseInstance &db, const string &changed, const Value &value) {
	auto config = OtlpConfig::From(db);
	auto text = [&](const string &name, string &field) {
		if (changed == name) {
			field = value.IsNull() ? string() : value.ToString();
		}
	};
	text("acl_otel_endpoint", config.endpoint);
	text("acl_otel_protocol", config.protocol);
	text("acl_otel_certificate", config.certificate);
	text("acl_otel_service_name", config.service_name);
	text("acl_otel_resource_attributes", config.resource_attributes);
	if (changed == "acl_otel_timeout") {
		config.timeout_s = value.IsNull() ? 0 : value.GetValue<int64_t>();
	}
	if (changed == "acl_otel_insecure") {
		config.insecure = !value.IsNull() && value.GetValue<bool>();
	}
	return config;
}

//! an endpoint from a setting, or from the standard environment alone (a container that sets
//! OTEL_EXPORTER_OTLP_ENDPOINT exports without a SET). The SDK's own default (localhost:4318) is
//! deliberately NOT an endpoint: nothing configured means nothing exported, said so.
bool HaveEndpoint(const OtlpConfig &config, const char *signal_variable) {
	if (!config.endpoint.empty()) {
		return true;
	}
	for (auto name : {"OTEL_EXPORTER_OTLP_ENDPOINT", signal_variable}) {
		const char *env = std::getenv(name);
		if (env && *env) {
			return true;
		}
	}
	return false;
}

} // namespace

shared_ptr<Exporter> OtelState::BuildExporter(DatabaseInstance &db, const string &changed, const Value &value,
                                              vector<string> &names) {
	auto config = ConfigAfter(db, changed, value);
	names.clear();
	if (!HaveEndpoint(config, "OTEL_EXPORTER_OTLP_LOGS_ENDPOINT")) {
		return make_shared_ptr<NoneExporter>();
	}
	auto exporter = make_shared_ptr<OtlpExporter>(config);
	names = exporter->HeaderNames();
	return exporter;
}

//! spec 003: the same endpoint, protocol, TLS and headers as the logs', on the metrics path
shared_ptr<MetricsExporter> OtelState::BuildMetricsExporter(DatabaseInstance &db, const string &changed,
                                                            const Value &value) {
	auto config = ConfigAfter(db, changed, value);
	if (!HaveEndpoint(config, "OTEL_EXPORTER_OTLP_METRICS_ENDPOINT")) {
		return make_shared_ptr<NoMetricsExporter>("acl_otel_endpoint is empty");
	}
	return MakeOtlpMetricsExporter(config);
}

void OtelState::Reconfigure(DatabaseInstance &db, const string &changed, const Value &value) {
	shared_ptr<OtelSink> current;
	{
		std::lock_guard<std::mutex> guard(lock);
		current = sink;
	}
	vector<string> names;
	auto exporter = BuildExporter(db, changed, value, names); // may throw: the SET is then refused
	shared_ptr<OtelMetrics> metrics_now;
	{
		std::lock_guard<std::mutex> guard(lock);
		header_names = std::move(names);
		metrics_now = metrics;
	}
	if (metrics_now) {
		metrics_now->SetExporter(BuildMetricsExporter(db, changed, value));
	}
	if (current) {
		current->SetExporter(std::move(exporter));
	}
}

vector<string> OtelState::HeaderNames() {
	std::lock_guard<std::mutex> guard(lock);
	return header_names;
}

string OtelState::StatusJson(DatabaseInstance &db) {
	shared_ptr<OtelSink> current;
	shared_ptr<OtelPolicy> rules;
	bool is_attached;
	{
		std::lock_guard<std::mutex> guard(lock);
		current = sink;
		rules = policy;
		is_attached = attached;
	}
	string json = "{\"attached\":" + string(is_attached ? "true" : "false");
	auto refused = AttachError();
	json += ",\"attach_error\":" + (refused.empty() ? string("null") : JsonQuote(refused));
	json += ",\"acl_loaded\":" +
	        string(db.GetObjectCache().Get<acl::AuditHooks>(acl::AuditHooks::ObjectType()) ? "true" : "false");
	json += ",\"endpoint\":" + JsonQuote(MaskUserinfo(SettingString(db, "acl_otel_endpoint", "")));
	json += ",\"exporter\":" + JsonQuote(current ? current->ExporterName() : "none (stopped)");
	json += ",\"headers\":[";
	auto names = HeaderNames();
	for (idx_t i = 0; i < names.size(); i++) {
		json += (i ? "," : "") + JsonQuote(names[i]);
	}
	json += "]";
	json += ",\"rules\":" + std::to_string(rules ? rules->RuleCount() : 0);
	shared_ptr<OtelMetrics> metrics_now;
	{
		std::lock_guard<std::mutex> guard(lock);
		metrics_now = metrics;
	}
	json += ",\"metrics\":" + (metrics_now ? metrics_now->StatusJson() : string("null"));
	if (current) {
		auto &stats = current->stats;
		json += ",\"queue_fill\":" + std::to_string(current->QueueFill());
		json += ",\"queue_size\":" + std::to_string(current->QueueSize());
		json += ",\"received\":" + std::to_string(stats.received.load());
		json += ",\"exported\":" + std::to_string(stats.exported.load());
		json += ",\"batches\":" + std::to_string(stats.batches.load());
		json += ",\"batches_failed\":" + std::to_string(stats.exported_batches_failed.load());
		json += ",\"dropped\":{\"queue\":" + std::to_string(stats.dropped_queue.load()) +
		        ",\"no_exporter\":" + std::to_string(stats.dropped_no_exporter.load()) + "}";
		json += ",\"export_errors\":" + std::to_string(stats.export_errors.load());
		auto last = stats.last_export_us.load();
		json += ",\"last_export_us\":" + (last > 0 ? std::to_string(last) : string("null"));
		auto last_error = current->LastError();
		json += ",\"last_error\":" + (last_error.empty() ? string("null") : JsonQuote(last_error));
	}
	json += "}";
	return json;
}

} // namespace acl_otel
} // namespace duckdb
