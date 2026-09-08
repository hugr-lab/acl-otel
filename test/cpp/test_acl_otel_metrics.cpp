// The metrics accumulators of spec 003, without a network or an SDK: the histogram's bucket edges
// and its sum/min/max, the bucket document refused as a whole, and the cap that keeps a series
// alive under a tenant explosion - the first `cap` values exact, the rest folded into `other` and
// counted, an allowlisted value exact whenever it arrives.
// Run via `make test-cpp`.

#include "acl_otel_metrics.hpp"
#include "acl_otel_test_util.hpp"

#include "duckdb/common/error_data.hpp"

using namespace duckdb;
using namespace acl_otel_test;

namespace {

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
	std::printf("PASS\n");
	return 0;
}
