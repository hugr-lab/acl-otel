// The metrics accumulators of spec 003, without a network or an SDK: the histogram's bucket edges
// and its sum/min/max, the bucket document refused as a whole, and the cap that keeps a series
// alive under a tenant explosion - the first `cap` values exact, the rest folded into `other` and
// counted, an allowlisted value exact whenever it arrives.
// Run via `make test-cpp`.

#include "acl_otel_metrics.hpp"
#include "acl_otel_test_util.hpp"

#include "duckdb/common/error_data.hpp"

#include <chrono>
#include <thread>

using namespace duckdb;
using namespace acl_otel_test;

namespace {

//! the transport, minus the network: it keeps the last snapshot it was handed
struct RecordingMetrics : acl_otel::MetricsExporter {
	acl_otel::MetricsSnapshot last;
	bool Export(const acl_otel::MetricsSnapshot &snapshot, string &) override {
		last = snapshot;
		return true;
	}
	string Describe() const override {
		return "recording";
	}
};

struct RefusingMetrics : acl_otel::MetricsExporter {
	bool Export(const acl_otel::MetricsSnapshot &, string &error) override {
		error = "the collector said no";
		return false;
	}
	string Describe() const override {
		return "refusing";
	}
};

acl::AuditEvent MakeEvent(const string &kind, bool allowed) {
	acl::AuditEvent event;
	event.kind = kind;
	event.allowed = allowed;
	event.ts_us = 1757000000000000;
	return event;
}

//! wait for a condition the worker thread satisfies, or give up
bool Within(int64_t ms, const std::function<bool()> &done) {
	for (int64_t waited = 0; waited < ms; waited += 20) {
		if (done()) {
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	return done();
}

acl_otel::Histogram RewriteHistogram() {
	for (auto &histogram : acl_otel::DefaultHistograms()) {
		if (histogram.name == "acl.rewrite.duration") {
			return histogram;
		}
	}
	throw std::runtime_error("acl.rewrite.duration is not among the default histograms");
}

const acl_otel::HistogramPoint &Only(const acl_otel::Histogram &histogram) {
	return histogram.points.begin()->second;
}

string Refusal(const std::function<void()> &what) {
	try {
		what();
	} catch (std::exception &ex) {
		return ErrorData(ex).RawMessage();
	}
	return "";
}

} // namespace

int main() {
	std::printf("test_acl_otel_metrics\n");
	{
		auto histogram = RewriteHistogram();
		acl_otel::Labels labels {{"kind", "statement"}, {"verdict", "allowed"}};
		// bounds 25, 50, 100, 250, 500, 1000, 2500, 5000, 10000 - a value equal to a bound belongs to
		// that bucket, and anything past the last one to the overflow
		histogram.Record(labels, 25);
		histogram.Record(labels, 26);
		histogram.Record(labels, 10000);
		histogram.Record(labels, 10001);
		auto &point = Only(histogram);
		Check(point.counts.size() == 10, "one bucket per bound plus the overflow");
		Check(point.counts[0] == 1 && point.counts[1] == 1 && point.counts[8] == 1 && point.counts[9] == 1,
		      "a value equal to a bound is in that bucket, one past the last is in the overflow");
		Check(point.count == 4 && point.sum == 25 + 26 + 10000 + 10001, "count and sum");
		Check(point.min == 25 && point.max == 10001, "min and max");
		acl_otel::Labels denied {{"kind", "statement"}, {"verdict", "denied"}};
		histogram.Record(denied, 40);
		Check(histogram.points.size() == 2 && Only(histogram).count == 4,
		      "a second attribute tuple is its own point, and does not disturb the first");
	}
	{
		auto histograms = acl_otel::DefaultHistograms();
		Check(histograms.size() == 3, "three instruments by default (R2.2)");
		acl_otel::ApplyBucketDocument(histograms, "");
		Check(histograms[0].bounds.size() == 9, "an empty document leaves the defaults alone");
		acl_otel::ApplyBucketDocument(histograms, R"({"acl.ingest.rows": [10, 100, 1000]})");
		Check(histograms[2].bounds == vector<double> {10, 100, 1000}, "the named instrument takes the new bounds");
		Check(histograms[0].bounds.size() == 9, "...and the instruments it does not name keep theirs");
	}
	{
		auto histograms = acl_otel::DefaultHistograms();
		auto before = histograms[0].bounds;
		Check(Refusal([&] { acl_otel::ApplyBucketDocument(histograms, "not json"); }).find("not a JSON document") !=
		          string::npos,
		      "a document that is not JSON is refused");
		Check(Refusal([&] { acl_otel::ApplyBucketDocument(histograms, "[1,2]"); }).find("expected a JSON object") !=
		          string::npos,
		      "an array is refused");
		Check(Refusal([&] {
			      acl_otel::ApplyBucketDocument(histograms, R"({"acl.nothing": [1]})");
		      }).find("does not exist") != string::npos,
		      "an instrument nobody has is refused");
		Check(Refusal([&] {
			      acl_otel::ApplyBucketDocument(histograms, R"({"acl.ingest.rows": []})");
		      }).find("non-empty array") != string::npos,
		      "an empty bound list is refused");
		Check(Refusal([&] {
			      acl_otel::ApplyBucketDocument(histograms, R"({"acl.ingest.rows": [10, 5]})");
		      }).find("must ascend") != string::npos,
		      "bounds that do not ascend are refused");
		Check(Refusal([&] {
			      acl_otel::ApplyBucketDocument(histograms,
			                                    R"({"acl.rewrite.duration": [1], "acl.ingest.rows": [3, 2]})");
		      }).find("must ascend") != string::npos,
		      "a document with one bad instrument is refused whole...");
		Check(histograms[0].bounds == before, "...and changed nothing, not even the good instrument before it");
	}
	{
		// the cap (R2.3): three values exact, the rest folded and counted
		acl_otel::CappedSeries series("acl.decisions.by_role", "role", 3);
		acl_otel::Labels none;
		for (int i = 0; i < 3; i++) {
			series.Add("role_" + std::to_string(i), none);
		}
		series.Add("role_0", none); // a known value is still exact, and does not spend the cap again
		series.Add("role_9", none);
		series.Add("role_10", none);
		Check(series.Distinct() == 3, "three distinct values kept");
		Check(series.Folded() == 2, "the two beyond the cap were folded, and counted");
		int64_t other = 0, role_0 = 0;
		for (auto &point : series.Points()) {
			for (auto &label : point.first) {
				if (label.first == "role" && label.second == "other") {
					other = point.second;
				}
				if (label.first == "role" && label.second == "role_0") {
					role_0 = point.second;
				}
			}
		}
		Check(other == 2, "the folded values share one series");
		Check(role_0 == 2, "a value that has a series keeps counting into it");
	}
	{
		// an allowlisted value is exact whenever it arrives, and never spends the cap
		acl_otel::CappedSeries series("acl.decisions.by_claim", "tenant", 1);
		series.SetAllowlist({"acme"});
		acl_otel::Labels none;
		series.Add("first", none);  // spends the cap
		series.Add("second", none); // folded
		series.Add("acme", none);   // exact regardless
		bool acme_exact = false;
		for (auto &point : series.Points()) {
			for (auto &label : point.first) {
				acme_exact = acme_exact || (label.first == "tenant" && label.second == "acme");
			}
		}
		Check(acme_exact, "an allowlisted value has its own series past the cap");
		Check(series.Distinct() == 1 && series.Folded() == 1, "...and did not spend the cap; the other one folded");
	}
	{
		// the extra labels a series carries beside its own (by_object has two)
		acl_otel::CappedSeries series("acl.decisions.by_object", "object", 10);
		series.Add("c.orders", {{"capability", "select"}});
		series.Add("c.orders", {{"capability", "insert"}});
		Check(series.Points().size() == 2, "the same value under two capabilities is two series");
		Check(series.Distinct() == 1, "...and one distinct object against the cap");
	}
	{
		// the scrape (R2.1/R2.4): the base's counters and gauges by name, ours beside them, one tick
		acl::AuditHooks hooks;
		hooks.Counters().Add("acl.decisions", {{"verdict", "allowed"}});
		hooks.Counters().Add("acl.decisions", {{"verdict", "allowed"}});
		hooks.Counters().Add("acl.denials", {{"reason_code", "capability"}});
		hooks.Gauges().Register("acl.sessions.live", {{"door", "flight"}}, "1", "sessions", [] { return int64_t(7); });
		auto recording = make_shared_ptr<RecordingMetrics>();
		acl_otel::OtelMetrics metrics(15, acl_otel::DefaultHistograms(), recording);
		metrics.SetSeries({"by_role"}, "", 100, {});
		auto allowed = MakeEvent("statement", true);
		allowed.rewrite_us = 120;
		allowed.principal.roles = {"analyst"};
		metrics.Observe(allowed);
		auto closed = MakeEvent("session", true);
		closed.duration_us = 90000000; // 90 s
		closed.door = "flight";
		closed.detail = "idle";
		metrics.Observe(closed);
		Check(metrics.TickNow(hooks), "the tick was taken by the transport");
		auto &snapshot = recording->last;
		int64_t decisions = 0, sessions_live = 0, by_role = 0;
		bool decisions_monotonic = false, gauge_monotonic = true;
		for (auto &point : snapshot.points) {
			if (point.name == "acl.decisions") {
				decisions = point.value;
				decisions_monotonic = point.monotonic;
			}
			if (point.name == "acl.sessions.live") {
				sessions_live = point.value;
				gauge_monotonic = point.monotonic;
			}
			if (point.name == "acl.decisions.by_role") {
				by_role = point.value;
			}
		}
		Check(decisions == 2 && decisions_monotonic, "the base's counter, by its own name, as a monotonic sum");
		Check(sessions_live == 7 && !gauge_monotonic, "the base's gauge, read at snapshot time, as a gauge");
		Check(by_role == 1, "the opt-in series is exported beside them");
		int64_t rewrite_count = 0, session_count = 0;
		double session_value = 0;
		for (auto &histogram : snapshot.histograms) {
			for (auto &point : histogram.points) {
				if (histogram.name == "acl.rewrite.duration") {
					rewrite_count += point.second.count;
				}
				if (histogram.name == "acl.session.duration") {
					session_count += point.second.count;
					session_value = point.second.sum;
				}
			}
		}
		Check(rewrite_count == 1, "the rewrite histogram saw the statement");
		Check(session_count == 1 && session_value == 90, "the session histogram counts seconds, not microseconds");
		Check(snapshot.start_us > 0 && snapshot.now_us >= snapshot.start_us, "the snapshot carries its window");
		Check(metrics.StatusJson().find("\"ticks\":1") != string::npos, "the status counts the tick");
	}
	{
		// no transport: every tick is counted as dropped, never silent (R6.2)
		acl::AuditHooks hooks;
		acl_otel::OtelMetrics metrics(15, acl_otel::DefaultHistograms(), nullptr);
		Check(!metrics.TickNow(hooks), "a tick with no exporter does not export");
		Check(metrics.stats.dropped_no_exporter == 1 && metrics.stats.export_errors == 0,
		      "...and is counted as a drop, not as an error");
		Check(metrics.ExporterName().rfind("none", 0) == 0, "the status names the stand-in");
	}
	{
		// a transport that refuses: an export error with its reason
		acl::AuditHooks hooks;
		auto refusing = make_shared_ptr<RefusingMetrics>();
		acl_otel::OtelMetrics metrics(15, acl_otel::DefaultHistograms(), refusing);
		Check(!metrics.TickNow(hooks), "the refused tick did not export");
		Check(metrics.stats.export_errors == 1 && metrics.LastError() == "the collector said no",
		      "...and the reason is the transport's own");
	}
	{
		// the thread: it starts, ticks on its own clock, and stops
		acl::AuditHooks hooks_object;
		auto hooks = shared_ptr<acl::AuditHooks>(&hooks_object, [](acl::AuditHooks *) {});
		auto recording = make_shared_ptr<RecordingMetrics>();
		acl_otel::OtelMetrics metrics(1, acl_otel::DefaultHistograms(), recording);
		metrics.Start(hooks);
		Check(Within(4000, [&] { return metrics.stats.ticks >= 1; }), "the scrape ticks on its own clock");
		metrics.Stop();
		auto after = metrics.stats.ticks.load();
		Check(Within(1500, [&] { return metrics.stats.ticks == after; }), "...and stops when it is stopped");
	}
	std::printf("PASS\n");
	return 0;
}
