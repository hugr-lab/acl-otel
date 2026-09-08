// The sampler (spec 005, R6.1): which `allowed` statements a backend gets to store. Pure, stateless
// and deterministic - a mix of the event's own seq, so two nodes with the same setting sample the
// same statements, and nothing here needs a lock or a random source on the audit thread.

#include "acl_otel.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "yyjson.hpp"

#include <algorithm>

namespace duckdb {
namespace acl_otel {

using namespace duckdb_yyjson; // NOLINT

namespace {

double Ratio(double value, const string &what) {
	if (!(value >= 0.0 && value <= 1.0)) {
		throw InvalidInputException("acl_otel_sample_allowed: %s must be a ratio between 0 and 1, not %f", what, value);
	}
	return value;
}

//! a ratio written as a bare number, or nothing
bool AsNumber(const string &text, double &out) {
	try {
		size_t used = 0;
		auto value = std::stod(text, &used);
		if (used != text.size()) {
			return false;
		}
		out = value;
		return true;
	} catch (...) {
		return false;
	}
}

} // namespace

Sampler::Sampler(const string &document) {
	auto text = document;
	StringUtil::Trim(text);
	if (text.empty()) {
		return; // the default: everything
	}
	double ratio = 0;
	if (AsNumber(text, ratio)) {
		fallback = Ratio(ratio, "the ratio");
		everything = fallback >= 1.0;
		return;
	}
	auto parsed = yyjson_read(text.c_str(), text.size(), 0);
	if (!parsed) {
		throw InvalidInputException(
		    "acl_otel_sample_allowed is neither a ratio nor a JSON object of role -> ratio: '%s'", text);
	}
	auto root = yyjson_doc_get_root(parsed);
	if (!yyjson_is_obj(root)) {
		yyjson_doc_free(parsed);
		throw InvalidInputException("acl_otel_sample_allowed expected a ratio or a JSON object of role -> ratio");
	}
	string refusal;
	std::map<string, double> collected;
	double star = 1.0;
	yyjson_obj_iter iter;
	yyjson_obj_iter_init(root, &iter);
	while (auto key = yyjson_obj_iter_next(&iter)) {
		string role = yyjson_get_str(key);
		auto value = yyjson_obj_iter_get_val(key);
		if (!yyjson_is_num(value)) {
			refusal = "acl_otel_sample_allowed: the ratio for '" + role + "' is not a number";
			break;
		}
		auto number = yyjson_get_num(value);
		if (!(number >= 0.0 && number <= 1.0)) {
			refusal = "acl_otel_sample_allowed: the ratio for '" + role + "' is not between 0 and 1";
			break;
		}
		if (role == "*") {
			star = number;
		} else {
			collected[role] = number;
		}
	}
	yyjson_doc_free(parsed);
	if (!refusal.empty()) {
		throw InvalidInputException(refusal);
	}
	fallback = star;
	by_role = std::move(collected);
	everything = fallback >= 1.0;
	for (auto &entry : by_role) {
		everything = everything && entry.second >= 1.0;
	}
}

double Sampler::RatioFor(const vector<string> &roles) const {
	if (by_role.empty()) {
		return fallback;
	}
	// the highest ratio any of the principal's roles names, and the fallback when none does
	double best = -1;
	for (auto &role : roles) {
		auto found = by_role.find(role);
		if (found != by_role.end()) {
			best = MaxValue(best, found->second);
		}
	}
	return best < 0 ? fallback : best;
}

bool Sampler::Keep(const acl::AuditEvent &event) const {
	// R6.1: only an allowed STATEMENT is ever sampled away. An admin decision is a change to the
	// policy - the most audit-relevant allowed event there is - and everything else (a refusal, a
	// session, a door, an ingest, a policy, a keys event) is the record the audit exists for.
	if (everything || !event.allowed || event.kind != "statement") {
		return true;
	}
	auto ratio = RatioFor(event.principal.roles);
	if (ratio >= 1.0) {
		return true;
	}
	if (ratio <= 0.0) {
		return false;
	}
	// deterministic in the event's own seq: a 64-bit mix (splitmix64's finaliser), compared against
	// the ratio in millionths. No state, no lock, and two nodes agree about the same statement.
	auto mixed = static_cast<uint64_t>(event.seq) + 0x9E3779B97F4A7C15ULL;
	mixed = (mixed ^ (mixed >> 30)) * 0xBF58476D1CE4E5B9ULL;
	mixed = (mixed ^ (mixed >> 27)) * 0x94D049BB133111EBULL;
	mixed = mixed ^ (mixed >> 31);
	return (mixed % 1000000ULL) < static_cast<uint64_t>(ratio * 1000000.0);
}

} // namespace acl_otel
} // namespace duckdb
