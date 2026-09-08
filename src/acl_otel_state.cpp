// The per-instance state (spec 001, R8): where the sink and the policy live, how they attach to the
// base's registry and detach from it, and what acl_otel_status() reports.

#include "acl_otel.hpp"
#include "acl_otel_metrics.hpp"
#include "acl_otel_otlp.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/config.hpp"

#include <chrono>
#include <cstdlib>

namespace duckdb {
namespace acl_otel {

namespace {

int64_t NowMicros() {
	return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch())
	    .count();
}

} // namespace

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
		Value sums;
		metrics->SetHistogramSums(!db.TryGetCurrentSetting("acl_otel_histogram_sums", sums) || sums.IsNull() ||
		                          sums.GetValue<bool>());
		// `this` and the instance outlive the scrape by construction: Stop() joins its thread, and
		// ~OtelState calls Stop() before anything of ours is gone. The reader is cleared there too.
		auto *state = this;
		auto *instance = &db;
		metrics->SetSelfMetrics([state, instance]() { return state->SelfMetrics(*instance); });
		sink->SetMetrics(metrics);
		metrics->Start(hooks);
	}
	hooks->AddSink(sink);
	hooks->SetSessionPolicy(policy);
	attached = true;
	return true;
}

//! spec 007: the reader runs only when a table is named, and reads once at once so a node that just
//! started is not a whole interval behind the fleet
void OtelState::StartRules(DatabaseInstance &db) {
	auto table = SettingString(db, "acl_otel_rules_table", "");
	StringUtil::Trim(table);
	if (table.empty()) {
		StopRulesReader();
		return;
	}
	try {
		RefreshRules(db);
	} catch (std::exception &) {
		// the status carries the reason; the reader will try again on its own clock
	}
	StartRulesReader(db);
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

//! R7.2: the extension's own numbers as metric points. Read from the sink and the scrape, named
//! `acl_otel.` so a dashboard never confuses the node's numbers with its reporter's.
vector<MetricPoint> OtelState::SelfMetrics(DatabaseInstance &db) {
	shared_ptr<OtelSink> current;
	shared_ptr<OtelMetrics> scrape;
	bool is_attached;
	{
		std::lock_guard<std::mutex> guard(lock);
		current = sink;
		scrape = metrics;
		is_attached = attached;
	}
	vector<MetricPoint> points;
	auto counter = [&](const char *name, int64_t value, const Labels &labels, const char *description) {
		points.push_back(MetricPoint {name, labels, value, true, "1", description});
	};
	auto gauge = [&](const char *name, int64_t value, const char *description) {
		points.push_back(MetricPoint {name, {}, value, false, "1", description});
	};
	if (current) {
		auto &stats = current->stats;
		counter("acl_otel.received", stats.received.load(), {}, "audit events this sink was handed");
		counter("acl_otel.exported", stats.exported.load(), {}, "audit events a backend received");
		counter("acl_otel.dropped", stats.dropped_queue.load(), {{"why", "queue"}}, "audit events lost, by why");
		counter("acl_otel.dropped", stats.dropped_no_exporter.load(), {{"why", "no_exporter"}},
		        "audit events lost, by why");
		counter("acl_otel.dropped", stats.export_errors.load(), {{"why", "export_error"}}, "audit events lost, by why");
		counter("acl_otel.dropped", stats.sampled.load(), {{"why", "sampled"}},
		        "audit events not exported by policy (spec 005), counted apart from a loss");
		counter("acl_otel.export_errors", stats.exported_batches_failed.load(), {}, "batches a backend refused");
		gauge("acl_otel.queue_fill", NumericCast<int64_t>(current->QueueFill()), "events waiting to be exported");
	}
	if (scrape) {
		counter("acl_otel.metrics_ticks", scrape->stats.ticks.load(), {}, "metric scrapes attempted");
		counter("acl_otel.metrics_errors", scrape->stats.export_errors.load(), {}, "metric scrapes a backend refused");
	}
	gauge("acl_otel.attached", is_attached ? 1 : 0, "1 while this extension is on the base's audit registry");
	gauge("acl_otel.healthy", Healthy(db) ? 1 : 0,
	      "0 while acl_otel_strict is on and this node is losing events, or is not attached (R7.3)");
	return points;
}

//! R7.3: strict is what gives the gauge teeth. Without it a node is always healthy - an operator who
//! did not ask for the signal is not given an alert.
bool OtelState::Healthy(DatabaseInstance &db) {
	Value strict;
	bool is_strict = db.TryGetCurrentSetting("acl_otel_strict", strict) && !strict.IsNull() && strict.GetValue<bool>();
	if (!is_strict) {
		return true;
	}
	shared_ptr<OtelSink> current;
	bool is_attached;
	{
		std::lock_guard<std::mutex> guard(lock);
		current = sink;
		is_attached = attached;
	}
	if (!is_attached || !current) {
		return false; // nothing is being delivered at all
	}
	shared_ptr<OtelMetrics> scrape;
	{
		std::lock_guard<std::mutex> guard(lock);
		scrape = metrics;
	}
	// What a policy leaves out (spec 005's sampling) is not a loss; an audit event that was recorded
	// and never delivered is - whether the queue was full, nothing was configured to send it, or a
	// backend refused the batch. A metric scrape a backend refused counts too: the node's numbers
	// did not arrive either. A scrape that had no endpoint to send to does NOT - a quiet node with
	// no endpoint has lost nothing.
	auto &stats = current->stats;
	auto drops = stats.dropped_queue.load() + stats.dropped_no_exporter.load() + stats.export_errors.load();
	if (scrape) {
		drops += scrape->stats.export_errors.load();
	}
	auto window = SettingInt64(db, "acl_otel_health_window", 60);
	std::lock_guard<std::mutex> guard(lock);
	return !health.Losing(drops, NowMicros(), window);
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
	StopRulesReader(); // first: a thread that queries a database must never outlive the database
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
		ending_metrics->Stop();                  // outside the lock: its thread may be mid-export
		ending_metrics->SetSelfMetrics(nullptr); // nothing of ours is read after this point
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

//! spec 007: the central table, read on a connection of ours. A failed or malformed read leaves the
//! rules exactly as they were - a fleet's configuration mistake must never quietly switch a node's
//! auditing off - and says what happened in the status.
idx_t OtelState::RefreshRules(DatabaseInstance &db) {
	auto table = SettingString(db, "acl_otel_rules_table", "");
	StringUtil::Trim(table);
	if (table.empty()) {
		std::lock_guard<std::mutex> guard(lock);
		rules_error.clear();
		return rules_in_force;
	}
	auto cap = MaxValue<int64_t>(SettingInt64(db, "acl_otel_max_rules", 1000), 1);
	// the table is the operator's own name, not a principal's input: quoted as an identifier path so
	// a name with a dot or a space still reads, and never concatenated from anything an event carried
	string query = "SELECT seq, role, subject, issuer, door, level FROM " + table + " ORDER BY seq LIMIT " +
	               std::to_string(cap + 1);
	Connection con(db);
	auto result = con.Query(query);
	if (result->HasError()) {
		std::lock_guard<std::mutex> guard(lock);
		rules_error = result->GetError();
		throw InvalidInputException("acl_otel: the rules table could not be read: %s", rules_error);
	}
	vector<LevelRule> parsed;
	string trouble;
	auto text = [](const Value &value) {
		return value.IsNull() ? string() : value.ToString();
	};
	for (idx_t row = 0; row < result->RowCount(); row++) {
		if (parsed.size() >= NumericCast<idx_t>(cap)) {
			trouble = "the rules table holds more than acl_otel_max_rules (" + std::to_string(cap) + ") rows";
			break;
		}
		auto seq = result->GetValue(0, row);
		try {
			parsed.push_back(RuleFromRow(text(result->GetValue(1, row)), text(result->GetValue(2, row)),
			                             text(result->GetValue(3, row)), text(result->GetValue(4, row)),
			                             text(result->GetValue(5, row)),
			                             seq.IsNull() ? NumericCast<int64_t>(row) : seq.GetValue<int64_t>()));
		} catch (std::exception &ex) {
			trouble = ErrorData(ex).RawMessage();
			break;
		}
	}
	if (!trouble.empty()) {
		std::lock_guard<std::mutex> guard(lock);
		rules_error = trouble;
		throw InvalidInputException("acl_otel: %s", trouble);
	}
	auto count = parsed.size();
	{
		std::lock_guard<std::mutex> guard(lock);
		rules_error.clear();
		rules_read_us = NowMicros();
		rules_in_force = count;
		// the JSON setting wins while it is set (§5): the table is read, kept, and not applied
		if (!rules_json.empty() && rules_json != "[]") {
			return count;
		}
		if (!policy) {
			policy = make_shared_ptr<OtelPolicy>();
		}
		policy->SetRules(std::move(parsed));
	}
	return count;
}

string OtelState::RulesSource(DatabaseInstance &db) {
	{
		std::lock_guard<std::mutex> guard(lock);
		if (!rules_json.empty() && rules_json != "[]") {
			return "setting";
		}
	}
	auto table = SettingString(db, "acl_otel_rules_table", "");
	StringUtil::Trim(table);
	return table.empty() ? "none" : "table";
}

void OtelState::StartRulesReader(DatabaseInstance &db) {
	{
		std::lock_guard<std::mutex> guard(lock);
		if (rules_worker.joinable()) {
			return;
		}
		rules_stopping = false;
	}
	auto *instance = &db; // outlives the thread: StopRulesReader joins it, and ~OtelState calls it
	rules_worker = std::thread([this, instance] { ReadRules(*instance); });
}

void OtelState::StopRulesReader() {
	{
		std::lock_guard<std::mutex> guard(lock);
		if (rules_stopping) {
			return;
		}
		rules_stopping = true;
	}
	rules_wake.notify_all();
	if (rules_worker.joinable()) {
		rules_worker.join();
	}
}

void OtelState::ReadRules(DatabaseInstance &db) {
	std::unique_lock<std::mutex> guard(lock);
	while (!rules_stopping) {
		auto seconds = MaxValue<int64_t>(SettingInt64(db, "acl_otel_rules_interval", 30), 1);
		rules_wake.wait_for(guard, std::chrono::seconds(seconds), [this] { return rules_stopping; });
		if (rules_stopping) {
			return;
		}
		guard.unlock();
		try {
			RefreshRules(db);
		} catch (std::exception &) {
			// the status already carries the reason; a reader that throws is a reader that stops
		}
		guard.lock();
	}
}

void OtelState::SetSampling(const string &document) {
	auto sampler = make_shared_ptr<Sampler>(document); // refused here, at the SET, never on an event
	shared_ptr<OtelSink> current;
	{
		std::lock_guard<std::mutex> guard(lock);
		sampling_document = document;
		current = sink;
	}
	if (current) {
		current->SetSampler(std::move(sampler));
	}
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
	if (changed == "acl_otel_claim_attributes") {
		config.claim_attributes = ParseClaimAttributes(value.IsNull() ? string() : value.ToString());
	}
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
	// spec 006: what an orchestrator reads, and why it reads what it reads
	Value strict;
	bool is_strict = db.TryGetCurrentSetting("acl_otel_strict", strict) && !strict.IsNull() && strict.GetValue<bool>();
	json += ",\"strict\":" + string(is_strict ? "true" : "false");
	json += ",\"healthy\":" + string(Healthy(db) ? "true" : "false");
	int64_t since = 0;
	{
		std::lock_guard<std::mutex> guard(lock);
		since = health.LosingSince();
	}
	json += ",\"losing_since_us\":" + (since > 0 ? std::to_string(since) : string("null"));
	// spec 007: where the rules came from, and whether the last read of the table worked
	json += ",\"rules_source\":" + JsonQuote(RulesSource(db));
	json += ",\"rules_table\":" + JsonQuote(SettingString(db, "acl_otel_rules_table", ""));
	{
		std::lock_guard<std::mutex> guard(lock);
		json += ",\"rules_read_us\":" + (rules_read_us > 0 ? std::to_string(rules_read_us) : string("null"));
		json += ",\"rules_error\":" + (rules_error.empty() ? string("null") : JsonQuote(rules_error));
	}
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
		json += ",\"sampled\":" + std::to_string(stats.sampled.load());
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
