// The metrics accumulators (spec 003): the histograms built from events (R2.2) and the capped
// high-cardinality series (R2.3). Pure state - no SDK, no network, no settings: what feeds them is
// the sink's worker, what reads them is the scrape.

#include "acl_otel_metrics.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "yyjson.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace duckdb {
namespace acl_otel {

using namespace duckdb_yyjson; // NOLINT

void Histogram::Record(const Labels &labels, double value) {
	auto &point = points[labels];
	if (point.counts.empty()) {
		point.counts.assign(bounds.size() + 1, 0);
		point.min = value;
		point.max = value;
	}
	// the first bound the value does not exceed; everything past the last is the overflow bucket
	idx_t bucket = bounds.size();
	for (idx_t i = 0; i < bounds.size(); i++) {
		if (value <= bounds[i]) {
			bucket = i;
			break;
		}
	}
	point.counts[bucket]++;
	point.count++;
	point.sum += value;
	point.min = MinValue(point.min, value);
	point.max = MaxValue(point.max, value);
}

vector<Histogram> DefaultHistograms() {
	vector<Histogram> out;
	out.push_back(Histogram {"acl.rewrite.duration",
	                         "us",
	                         "how long a decision took, by kind and verdict",
	                         {25, 50, 100, 250, 500, 1000, 2500, 5000, 10000},
	                         {}});
	out.push_back(Histogram {"acl.session.duration",
	                         "s",
	                         "how long a session lived, by door and how it ended",
	                         {1, 5, 15, 60, 300, 900, 3600, 14400, 86400},
	                         {}});
	out.push_back(Histogram {"acl.ingest.rows",
	                         "rows",
	                         "rows per completed ingest, by door",
	                         {100, 1000, 10000, 100000, 1000000, 10000000},
	                         {}});
	return out;
}

void ApplyBucketDocument(vector<Histogram> &histograms, const string &json) {
	auto trimmed = json;
	StringUtil::Trim(trimmed);
	if (trimmed.empty()) {
		return;
	}
	auto document = yyjson_read(trimmed.c_str(), trimmed.size(), 0);
	if (!document) {
		throw InvalidInputException("acl_otel_histogram_buckets is not a JSON document");
	}
	auto root = yyjson_doc_get_root(document);
	if (!yyjson_is_obj(root)) {
		yyjson_doc_free(document);
		throw InvalidInputException("acl_otel_histogram_buckets expected a JSON object of instrument -> bounds");
	}
	// collected first, applied only when the whole document is good: a refused SET changes nothing
	std::map<string, vector<double>> parsed;
	string refusal;
	yyjson_obj_iter iter;
	yyjson_obj_iter_init(root, &iter);
	while (auto key = yyjson_obj_iter_next(&iter)) {
		string name = yyjson_get_str(key);
		auto value = yyjson_obj_iter_get_val(key);
		bool known = false;
		for (auto &histogram : histograms) {
			known = known || histogram.name == name;
		}
		if (!known) {
			refusal = "acl_otel_histogram_buckets names an instrument that does not exist: '" + name + "'";
			break;
		}
		if (!yyjson_is_arr(value) || yyjson_arr_size(value) == 0) {
			refusal = "acl_otel_histogram_buckets: '" + name + "' needs a non-empty array of bounds";
			break;
		}
		vector<double> bounds;
		size_t index, max;
		yyjson_val *item;
		yyjson_arr_foreach(value, index, max, item) {
			if (!yyjson_is_num(item)) {
				refusal = "acl_otel_histogram_buckets: a bound of '" + name + "' is not a number";
				break;
			}
			auto bound = yyjson_get_num(item);
			if (!bounds.empty() && bound <= bounds.back()) {
				refusal = "acl_otel_histogram_buckets: the bounds of '" + name + "' must ascend";
				break;
			}
			bounds.push_back(bound);
		}
		if (!refusal.empty()) {
			break;
		}
		parsed[name] = std::move(bounds);
	}
	yyjson_doc_free(document);
	if (!refusal.empty()) {
		throw InvalidInputException(refusal);
	}
	for (auto &histogram : histograms) {
		auto entry = parsed.find(histogram.name);
		if (entry == parsed.end()) {
			continue; // an instrument the document does not name keeps its defaults
		}
		histogram.bounds = entry->second;
		histogram.points.clear(); // the old points counted into other buckets; they are not comparable
	}
}

std::map<string, vector<string>> ParseSeriesAllowlist(const string &json) {
	std::map<string, vector<string>> out;
	auto trimmed = json;
	StringUtil::Trim(trimmed);
	if (trimmed.empty()) {
		return out;
	}
	auto document = yyjson_read(trimmed.c_str(), trimmed.size(), 0);
	if (!document) {
		throw InvalidInputException("acl_otel_series_allowlist is not a JSON document");
	}
	auto root = yyjson_doc_get_root(document);
	if (!yyjson_is_obj(root)) {
		yyjson_doc_free(document);
		throw InvalidInputException("acl_otel_series_allowlist expected a JSON object of instrument -> values");
	}
	string refusal;
	yyjson_obj_iter iter;
	yyjson_obj_iter_init(root, &iter);
	while (auto key = yyjson_obj_iter_next(&iter)) {
		string name = yyjson_get_str(key);
		auto value = yyjson_obj_iter_get_val(key);
		if (!yyjson_is_arr(value)) {
			refusal = "acl_otel_series_allowlist: '" + name + "' needs an array of values";
			break;
		}
		vector<string> values;
		size_t index, max;
		yyjson_val *item;
		yyjson_arr_foreach(value, index, max, item) {
			if (!yyjson_is_str(item)) {
				refusal = "acl_otel_series_allowlist: a value of '" + name + "' is not a string";
				break;
			}
			values.emplace_back(yyjson_get_str(item));
		}
		if (!refusal.empty()) {
			break;
		}
		out[name] = std::move(values);
	}
	yyjson_doc_free(document);
	if (!refusal.empty()) {
		throw InvalidInputException(refusal);
	}
	return out;
}

CappedSeries::CappedSeries(string name_p, string label_p, idx_t cap_p)
    : name(std::move(name_p)), label(std::move(label_p)), cap(cap_p == 0 ? 1 : cap_p) {
}

void CappedSeries::SetAllowlist(vector<string> allowed) {
	std::lock_guard<std::mutex> guard(lock);
	allowlist.clear();
	for (auto &value : allowed) {
		allowlist.insert(value);
	}
}

void CappedSeries::SetCap(idx_t cap_p) {
	std::lock_guard<std::mutex> guard(lock);
	cap = cap_p == 0 ? 1 : cap_p;
}

void CappedSeries::Add(const string &value, const Labels &extra) {
	std::lock_guard<std::mutex> guard(lock);
	// an allowlisted value is exact whenever it arrives and never spends the cap
	string kept = value;
	if (allowlist.find(value) == allowlist.end()) {
		if (known.find(value) == known.end()) {
			if (known.size() >= cap) {
				kept = OTHER;
				folded++;
			} else {
				known.insert(value);
			}
		}
	}
	Labels labels;
	labels.emplace_back(label, kept);
	for (auto &pair : extra) {
		labels.push_back(pair);
	}
	std::sort(labels.begin(), labels.end());
	points[labels]++;
}

vector<std::pair<Labels, int64_t>> CappedSeries::Points() const {
	std::lock_guard<std::mutex> guard(lock);
	vector<std::pair<Labels, int64_t>> out;
	out.reserve(points.size());
	for (auto &entry : points) {
		out.emplace_back(entry.first, entry.second);
	}
	return out;
}

int64_t CappedSeries::Folded() const {
	std::lock_guard<std::mutex> guard(lock);
	return folded;
}

idx_t CappedSeries::Distinct() const {
	std::lock_guard<std::mutex> guard(lock);
	return known.size();
}

namespace {

int64_t NowMicros() {
	return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch())
	    .count();
}

string JsonQuote(const string &value) {
	string out = "\"";
	for (auto c : value) {
		if (c == '"' || c == '\\') {
			out += '\\';
			out += c;
		} else if (static_cast<unsigned char>(c) < 0x20) {
			out += ' ';
		} else {
			out += c;
		}
	}
	return out + "\"";
}

//! the instrument a series name stands for, and the label it counts by (R2.3)
struct SeriesSpec {
	const char *option;
	const char *name;
	const char *label;
};

const SeriesSpec SERIES_SPECS[] = {{"by_role", "acl.decisions.by_role", "role"},
                                   {"by_object", "acl.decisions.by_object", "object"},
                                   {"by_subject", "acl.decisions.by_subject", "subject"},
                                   {"by_claim", "acl.decisions.by_claim", "claim"}};

} // namespace

OtelMetrics::OtelMetrics(int64_t interval_s_p, vector<Histogram> histograms_p, shared_ptr<MetricsExporter> exporter_p)
    : interval_s(interval_s_p <= 0 ? 15 : interval_s_p), start_us(NowMicros()), histograms(std::move(histograms_p)),
      exporter(std::move(exporter_p)) {
	if (!exporter) {
		exporter = make_shared_ptr<NoMetricsExporter>("no endpoint");
	}
}

OtelMetrics::~OtelMetrics() {
	Stop();
}

void OtelMetrics::SetSeries(const vector<string> &names, const string &claim, idx_t cap,
                            const std::map<string, vector<string>> &allowlists) {
	std::lock_guard<std::mutex> guard(lock);
	claim_dimension = claim;
	series.clear();
	for (auto &spec : SERIES_SPECS) {
		bool wanted = false;
		for (auto &name : names) {
			wanted = wanted || name == spec.option;
		}
		// by_claim without a claim to count by is not a series, it is a mistake the settings refuse
		if (!wanted || (string(spec.option) == "by_claim" && claim.empty())) {
			continue;
		}
		auto one = make_uniq<CappedSeries>(spec.name, spec.label, cap);
		auto allowed = allowlists.find(spec.name);
		if (allowed != allowlists.end()) {
			one->SetAllowlist(allowed->second);
		}
		series.push_back(std::move(one));
	}
}

void OtelMetrics::SetExporter(shared_ptr<MetricsExporter> exporter_p) {
	shared_ptr<MetricsExporter> previous;
	{
		std::lock_guard<std::mutex> guard(lock);
		previous = std::move(exporter);
		exporter = exporter_p ? std::move(exporter_p) : make_shared_ptr<NoMetricsExporter>("no endpoint");
	}
	// as the sink does: the old transport's destructor shuts a client down, and Observe holds this
	// same lock on the base's audit thread
}

void OtelMetrics::SetHistogramSums(bool on) {
	std::lock_guard<std::mutex> guard(lock);
	histogram_sums = on;
}

void OtelMetrics::Observe(const acl::AuditEvent &event) {
	std::lock_guard<std::mutex> guard(lock);
	auto record = [&](const char *name, const Labels &labels, double value) {
		for (auto &histogram : histograms) {
			if (histogram.name == name) {
				histogram.Record(labels, value);
				return;
			}
		}
	};
	if ((event.kind == "statement" || event.kind == "admin") && event.rewrite_us >= 0) {
		record("acl.rewrite.duration", {{"kind", event.kind}, {"verdict", event.allowed ? "allowed" : "denied"}},
		       static_cast<double>(event.rewrite_us));
	}
	if (event.kind == "session" && event.duration_us >= 0) {
		record("acl.session.duration",
		       {{"door", event.door.empty() ? "gateway" : event.door},
		        {"how", event.detail.empty() ? "closed" : event.detail}},
		       static_cast<double>(event.duration_us) / 1000000.0);
	}
	if (event.kind == "ingest" && event.rows >= 0) {
		record("acl.ingest.rows", {{"door", event.door.empty() ? "gateway" : event.door}},
		       static_cast<double>(event.rows));
	}
	// the opt-in series: only decisions, and only what the operator asked for (R2.3)
	if (event.kind != "statement" && event.kind != "admin") {
		return;
	}
	Labels verdict {{"verdict", event.allowed ? "allowed" : "denied"}};
	for (auto &one : series) {
		if (one->Name() == "acl.decisions.by_role") {
			for (auto &role : event.principal.roles) {
				one->Add(role, verdict);
			}
		} else if (one->Name() == "acl.decisions.by_object") {
			for (auto &object : event.objects) {
				one->Add(object.name, {{"capability", object.capability}, {"verdict", verdict[0].second}});
			}
		} else if (one->Name() == "acl.decisions.by_subject") {
			if (!event.principal.subject.empty()) {
				one->Add(event.principal.subject, verdict);
			}
		} else if (one->Name() == "acl.decisions.by_claim") {
			auto claim = event.principal.claims.find(claim_dimension);
			if (claim != event.principal.claims.end() && !claim->second.empty()) {
				one->Add(claim->second, verdict);
			}
		}
	}
}

MetricsSnapshot OtelMetrics::Build(acl::AuditHooks &hooks) {
	MetricsSnapshot snapshot;
	snapshot.start_us = start_us;
	snapshot.now_us = NowMicros();
	// R2.1 / R2.4: the base's own numbers, by name, never re-derived here
	auto counters = hooks.Counters().Snapshot();
	auto gauges = hooks.Gauges().Snapshot();
	auto take = [&](const vector<acl::AuditMetric> &metrics, bool monotonic) {
		for (auto &metric : metrics) {
			MetricPoint point;
			point.name = metric.name;
			point.labels = metric.attributes;
			point.value = metric.value;
			point.monotonic = monotonic;
			point.unit = metric.unit;
			point.description = metric.description;
			snapshot.points.push_back(std::move(point));
		}
	};
	take(counters, true);
	take(gauges, false);
	bool with_sums = false;
	{
		std::lock_guard<std::mutex> guard(lock);
		with_sums = histogram_sums;
		snapshot.histograms = histograms; // a copy: the accumulation goes on while this is exported
		for (auto &one : series) {
			for (auto &entry : one->Points()) {
				MetricPoint point;
				point.name = one->Name();
				point.labels = entry.first;
				point.value = entry.second;
				point.monotonic = true;
				point.unit = "1";
				point.description = "decisions, by a label the operator asked for (spec 003)";
				snapshot.points.push_back(std::move(point));
			}
		}
	}
	// R2.5: the pair a bridge that cannot ingest a histogram still understands
	if (with_sums) {
		for (auto &histogram : snapshot.histograms) {
			for (auto &point : histogram.points) {
				MetricPoint sum;
				sum.name = histogram.name + "_sum";
				sum.labels = point.first;
				// the pair is integral (a MetricPoint is), so a fractional sum rounds rather than
				// truncates - the histogram beside it carries the exact number
				sum.value = static_cast<int64_t>(std::llround(point.second.sum));
				sum.monotonic = true;
				sum.unit = histogram.unit;
				sum.description = histogram.description + " (sum)";
				snapshot.points.push_back(std::move(sum));
				MetricPoint count;
				count.name = histogram.name + "_count";
				count.labels = point.first;
				count.value = point.second.count;
				count.monotonic = true;
				count.unit = "1";
				count.description = histogram.description + " (count)";
				snapshot.points.push_back(std::move(count));
			}
		}
	}
	return snapshot;
}

bool OtelMetrics::Send(const MetricsSnapshot &snapshot) {
	shared_ptr<MetricsExporter> transport;
	{
		std::lock_guard<std::mutex> guard(lock);
		transport = exporter;
	}
	string error;
	stats.ticks++;
	int64_t points = NumericCast<int64_t>(snapshot.points.size());
	for (auto &histogram : snapshot.histograms) {
		points += NumericCast<int64_t>(histogram.points.size());
	}
	stats.points = points;
	if (transport->Export(snapshot, error)) {
		stats.exported++;
		stats.last_export_us = snapshot.now_us;
		return true;
	}
	if (!transport->Configured()) {
		stats.dropped_no_exporter++;
		return false;
	}
	stats.export_errors++;
	std::lock_guard<std::mutex> guard(lock);
	stats.last_error = error;
	return false;
}

bool OtelMetrics::TickNow(acl::AuditHooks &hooks_p) {
	return Send(Build(hooks_p));
}

void OtelMetrics::Start(shared_ptr<acl::AuditHooks> hooks_p) {
	{
		std::lock_guard<std::mutex> guard(lock);
		if (worker.joinable()) {
			return;
		}
		hooks = std::move(hooks_p);
		stopping = false;
	}
	worker = std::thread([this] { Run(); });
}

void OtelMetrics::Stop() {
	{
		std::lock_guard<std::mutex> guard(lock);
		if (stopping) {
			return;
		}
		stopping = true;
	}
	wake.notify_all();
	if (worker.joinable()) {
		worker.join();
	}
}

void OtelMetrics::Run() {
	std::unique_lock<std::mutex> guard(lock);
	while (!stopping) {
		wake.wait_for(guard, std::chrono::seconds(interval_s), [this] { return stopping; });
		if (stopping) {
			break;
		}
		auto registry = hooks;
		guard.unlock();
		if (registry) {
			Send(Build(*registry));
		}
		guard.lock();
	}
}

string OtelMetrics::ExporterName() {
	std::lock_guard<std::mutex> guard(lock);
	return exporter->Describe();
}

string OtelMetrics::LastError() {
	std::lock_guard<std::mutex> guard(lock);
	return stats.last_error;
}

string OtelMetrics::StatusJson() {
	string json = "{\"interval\":" + std::to_string(interval_s);
	json += ",\"exporter\":" + JsonQuote(ExporterName());
	json += ",\"ticks\":" + std::to_string(stats.ticks.load());
	json += ",\"exported\":" + std::to_string(stats.exported.load());
	json += ",\"points\":" + std::to_string(stats.points.load());
	json += ",\"dropped_no_exporter\":" + std::to_string(stats.dropped_no_exporter.load());
	json += ",\"export_errors\":" + std::to_string(stats.export_errors.load());
	auto last = stats.last_export_us.load();
	json += ",\"last_export_us\":" + (last > 0 ? std::to_string(last) : string("null"));
	auto error = LastError();
	json += ",\"last_error\":" + (error.empty() ? string("null") : JsonQuote(error));
	json += ",\"series\":[";
	{
		std::lock_guard<std::mutex> guard(lock);
		for (idx_t i = 0; i < series.size(); i++) {
			json += string(i ? "," : "") + "{\"name\":" + JsonQuote(series[i]->Name()) +
			        ",\"distinct\":" + std::to_string(series[i]->Distinct()) +
			        ",\"folded\":" + std::to_string(series[i]->Folded()) + "}";
		}
	}
	return json + "]}";
}

} // namespace acl_otel
} // namespace duckdb
