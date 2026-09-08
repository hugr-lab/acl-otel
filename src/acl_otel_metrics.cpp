// The metrics accumulators (spec 003): the histograms built from events (R2.2) and the capped
// high-cardinality series (R2.3). Pure state - no SDK, no network, no settings: what feeds them is
// the sink's worker, what reads them is the scrape.

#include "acl_otel_metrics.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "yyjson.hpp"

#include <algorithm>

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

} // namespace acl_otel
} // namespace duckdb
