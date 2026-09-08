//===----------------------------------------------------------------------===//
// acl_otel_metrics.hpp - the metrics side (spec 003)
//
// What spec 002 does for the record of a decision, this does for the shape of them: the base's
// counters and gauges scraped on a timer and pushed as OTLP metrics (R2.1), the histograms the base
// deliberately does not keep - built here from the same events the sink already receives (R2.2) -
// and the high-cardinality series an operator asks for by name, each capped so a tenant explosion
// costs one series and not a backend (R2.3).
//
// Nothing here runs on the decision path: the accumulators are fed from the sink's worker, and the
// scrape has its own thread.
//===----------------------------------------------------------------------===//

#pragma once

#include "acl_audit.hpp"
#include "duckdb/main/database.hpp"

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace duckdb {
namespace acl_otel {

//! An attribute tuple as a metric carries it: ordered, bounded, and compared as a whole.
using Labels = vector<std::pair<string, string>>;

//! One histogram's state for one attribute tuple: cumulative since the extension started (R2.1's
//! temporality, kept for every instrument so the base's counters and ours agree in shape).
struct HistogramPoint {
	vector<int64_t> counts; // bounds.size() + 1, the last being the overflow bucket
	double sum = 0;
	double min = 0;
	double max = 0;
	int64_t count = 0;
};

//! An instrument built here from events (R2.2). The bounds are the operator's
//! (`acl_otel_histogram_buckets`), the defaults are in DefaultBuckets().
struct Histogram {
	string name;
	string unit;
	string description;
	vector<double> bounds;
	std::map<Labels, HistogramPoint> points;

	void Record(const Labels &labels, double value);
};

//! The three instruments of R2.2, with the defaults of the spec.
vector<Histogram> DefaultHistograms();
//! `acl_otel_histogram_buckets`: {"acl.rewrite.duration": [25, 50, ...]}. Throws
//! InvalidInputException naming what is wrong; "" leaves every instrument at its defaults. An
//! instrument the document does not name keeps them.
void ApplyBucketDocument(vector<Histogram> &histograms, const string &json);

//! A counter whose label values are unbounded in principle (a role, an object, a subject, a claim
//! value) and bounded in practice (R2.3): the first `cap` distinct values are kept as they are,
//! everything after folds into one series named `other`, and the fold is counted. An allowlisted
//! value is exact whenever it arrives, and does not spend the cap.
class CappedSeries {
public:
	static constexpr const char *OTHER = "other";

	CappedSeries(string name, string label, idx_t cap);

	void Add(const string &value, const Labels &extra);
	//! the values the operator wants exact whatever the order of arrival
	void SetAllowlist(vector<string> allowed);
	void SetCap(idx_t cap);
	//! label tuples and their counts, for the exporter
	vector<std::pair<Labels, int64_t>> Points() const;
	int64_t Folded() const;
	idx_t Distinct() const;
	const string &Name() const {
		return name;
	}

private:
	string name;
	string label;
	idx_t cap;
	mutable std::mutex lock;
	std::map<Labels, int64_t> points;
	std::set<string> allowlist;
	std::set<string> known; // the label values that already have a series of their own
	int64_t folded = 0;
};

//! One counter or gauge as a transport receives it: the base's own name, attributes, unit and
//! description, plus what kind of instrument it is. `monotonic` separates a counter from a gauge.
struct MetricPoint {
	string name;
	Labels labels;
	int64_t value = 0;
	bool monotonic = false;
	string unit;
	string description;
};

//! What one tick hands the transport: the base's numbers as they are now, our histograms as they
//! have accumulated, and the moment the extension started (cumulative temporality, spec 003).
struct MetricsSnapshot {
	int64_t start_us = 0;
	int64_t now_us = 0;
	vector<MetricPoint> points;
	vector<Histogram> histograms;
};

//! The transport seam, the metrics twin of spec 002's `Exporter`: the OTLP exporter implements it,
//! and a stand-in that counts stands where there is none.
class MetricsExporter {
public:
	virtual ~MetricsExporter() = default;
	virtual bool Export(const MetricsSnapshot &snapshot, string &error) = 0;
	virtual string Describe() const = 0;
	virtual bool Configured() const {
		return true;
	}
};

//! No endpoint configured: nothing leaves, and every tick is counted as dropped (R6.2).
class NoMetricsExporter : public MetricsExporter {
public:
	explicit NoMetricsExporter(string why_p) : why(std::move(why_p)) {
	}
	bool Export(const MetricsSnapshot &, string &) override {
		return false;
	}
	string Describe() const override {
		return "none (" + why + ")";
	}
	bool Configured() const override {
		return false;
	}

private:
	string why;
};

//! The extension's own numbers about metrics (R7), beside the sink's.
struct MetricsStats {
	std::atomic<int64_t> ticks {0};
	std::atomic<int64_t> exported {0};
	std::atomic<int64_t> dropped_no_exporter {0};
	std::atomic<int64_t> export_errors {0};
	std::atomic<int64_t> points {0}; // what the last tick sent
	std::atomic<int64_t> last_export_us {0};
	string last_error; // under `lock`
};

//! The scrape (R2.1): one thread, one tick every `interval_s`, one Export per tick. Fed from the
//! sink's OnEvent for the histograms and the opt-in series - so a histogram sees exactly what the
//! audit level records, which is the honest answer and the one the spec states.
class OtelMetrics {
public:
	OtelMetrics(int64_t interval_s, vector<Histogram> histograms, shared_ptr<MetricsExporter> exporter);
	~OtelMetrics();

	//! every event the sink receives, before its queue: O(bounds), one short lock, no I/O (R10.1)
	void Observe(const acl::AuditEvent &event);
	//! the series of R2.3, by name (`by_role`, `by_object`, `by_subject`, `by_claim`); empty = none
	void SetSeries(const vector<string> &names, const string &claim, idx_t cap,
	               const std::map<string, vector<string>> &allowlists);
	void SetExporter(shared_ptr<MetricsExporter> exporter);
	//! R2.5: beside every histogram, the `_sum` / `_count` pair a bridge that cannot ingest an OTLP
	//! histogram still understands. On by default - the Azure bridge is why this exists.
	void SetHistogramSums(bool on);
	//! export one tick now (a test, a shutdown); true when the transport took it
	bool TickNow(acl::AuditHooks &hooks);
	//! start scraping `hooks` until Stop()
	void Start(shared_ptr<acl::AuditHooks> hooks);
	void Stop();

	string ExporterName();
	string LastError();
	string StatusJson();
	MetricsStats stats;

private:
	void Run();
	MetricsSnapshot Build(acl::AuditHooks &hooks);
	bool Send(const MetricsSnapshot &snapshot);

	int64_t interval_s;
	int64_t start_us;
	std::mutex lock;
	std::condition_variable wake;
	bool stopping = false;
	vector<Histogram> histograms; // under `lock`
	vector<unique_ptr<CappedSeries>> series;
	string claim_dimension;
	bool histogram_sums = true;
	shared_ptr<MetricsExporter> exporter;
	shared_ptr<acl::AuditHooks> hooks;
	std::thread worker;
};

//! spec 003: the OTLP metrics transport (src/acl_otel_otlp_metrics.cpp), built from the same config
//! as the logs' - declared here so acl_otel_state.cpp needs no SDK header of its own
shared_ptr<MetricsExporter> MakeOtlpMetricsExporter(const struct OtlpConfig &config);
//! `acl_otel_series_allowlist`: {"acl.decisions.by_role": ["analyst", ...]}. Throws
//! InvalidInputException naming what is wrong; "" is no allowlist.
std::map<string, vector<string>> ParseSeriesAllowlist(const string &json);

} // namespace acl_otel
} // namespace duckdb
