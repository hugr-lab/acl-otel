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

} // namespace acl_otel
} // namespace duckdb
