// The level rules (spec 001, R3): a JSON document, ordered, first match wins - parsed once when the
// setting is written (a malformed document is refused there, never at a session open), matched in
// O(rules) on the session-open path with no allocation and no I/O (R3.2, R10.2).

#include "acl_otel.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "yyjson.hpp"

namespace duckdb {
namespace acl_otel {

namespace {

string Text(duckdb_yyjson::yyjson_val *value) {
	if (!value || !duckdb_yyjson::yyjson_is_str(value)) {
		return string();
	}
	return duckdb_yyjson::yyjson_get_str(value);
}

//! "*" and "" both mean "any": an operator writes `*` to say so on purpose, and leaves the field out
//! to say the same
string Any(const string &value) {
	return value == "*" ? string() : value;
}

} // namespace

vector<LevelRule> ParseLevelRules(const string &json) {
	vector<LevelRule> rules;
	auto trimmed = json;
	StringUtil::Trim(trimmed);
	if (trimmed.empty()) {
		return rules;
	}
	auto *doc = duckdb_yyjson::yyjson_read(trimmed.c_str(), trimmed.size(), 0);
	if (!doc) {
		throw InvalidInputException("acl_otel_level_rules: not a JSON document");
	}
	auto *root = duckdb_yyjson::yyjson_doc_get_root(doc);
	if (!root || !duckdb_yyjson::yyjson_is_arr(root)) {
		duckdb_yyjson::yyjson_doc_free(doc);
		throw InvalidInputException("acl_otel_level_rules: expected a JSON array of rules");
	}
	duckdb_yyjson::yyjson_val *item;
	duckdb_yyjson::yyjson_arr_iter iter;
	duckdb_yyjson::yyjson_arr_iter_init(root, &iter);
	idx_t position = 0;
	while ((item = duckdb_yyjson::yyjson_arr_iter_next(&iter))) {
		position++;
		if (!duckdb_yyjson::yyjson_is_obj(item)) {
			duckdb_yyjson::yyjson_doc_free(doc);
			throw InvalidInputException("acl_otel_level_rules: rule %llu is not an object", position);
		}
		LevelRule rule;
		rule.role = Any(Text(duckdb_yyjson::yyjson_obj_get(item, "role")));
		rule.subject = Any(Text(duckdb_yyjson::yyjson_obj_get(item, "subject")));
		rule.issuer = Any(Text(duckdb_yyjson::yyjson_obj_get(item, "issuer")));
		rule.door = Any(Text(duckdb_yyjson::yyjson_obj_get(item, "door")));
		auto level = Text(duckdb_yyjson::yyjson_obj_get(item, "level"));
		if (!acl::ParseAuditLevel(level, rule.level)) {
			duckdb_yyjson::yyjson_doc_free(doc);
			throw InvalidInputException("acl_otel_level_rules: rule %llu needs a level of off, denied, decisions or "
			                            "all, not \"%s\"",
			                            position, level);
		}
		// what a rule may name is closed: a key nobody reads is a typo that would silently match
		// everything (a rule with `roles` instead of `role` is a catch-all)
		duckdb_yyjson::yyjson_val *key, *value;
		duckdb_yyjson::yyjson_obj_iter keys;
		duckdb_yyjson::yyjson_obj_iter_init(item, &keys);
		while ((key = duckdb_yyjson::yyjson_obj_iter_next(&keys))) {
			value = duckdb_yyjson::yyjson_obj_iter_get_val(key);
			(void)value;
			string name = duckdb_yyjson::yyjson_get_str(key);
			if (name != "role" && name != "subject" && name != "issuer" && name != "door" && name != "level") {
				duckdb_yyjson::yyjson_doc_free(doc);
				throw InvalidInputException("acl_otel_level_rules: rule %llu has an unknown key \"%s\" (role, "
				                            "subject, issuer, door, level)",
				                            position, name);
			}
		}
		rules.push_back(std::move(rule));
	}
	duckdb_yyjson::yyjson_doc_free(doc);
	return rules;
}

bool RuleMatches(const LevelRule &rule, const acl::Principal &principal, const string &door) {
	if (!rule.door.empty() && !StringUtil::CIEquals(rule.door, door)) {
		return false;
	}
	if (!rule.subject.empty() && rule.subject != principal.subject) {
		return false;
	}
	if (!rule.issuer.empty() && rule.issuer != principal.issuer) {
		return false;
	}
	if (!rule.role.empty()) {
		bool held = false;
		for (auto &role : principal.roles) {
			held = held || StringUtil::CIEquals(role, rule.role);
		}
		if (!held) {
			return false;
		}
	}
	return true;
}

bool OtelPolicy::LevelFor(const acl::Principal &principal, const string &door, acl::AuditLevel &out) {
	std::lock_guard<std::mutex> guard(lock);
	for (auto &rule : rules) {
		if (RuleMatches(rule, principal, door)) {
			out = rule.level;
			return true;
		}
	}
	return false; // no opinion: the instance's level applies (C6)
}

void OtelPolicy::SetRules(vector<LevelRule> rules_p) {
	std::lock_guard<std::mutex> guard(lock);
	rules = std::move(rules_p);
}

idx_t OtelPolicy::RuleCount() {
	std::lock_guard<std::mutex> guard(lock);
	return rules.size();
}

} // namespace acl_otel
} // namespace duckdb
