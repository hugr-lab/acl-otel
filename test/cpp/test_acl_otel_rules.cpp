// The level rules (spec 001, R3), on the engine alone: parsing, the refusals, first-match order,
// `*` and absence meaning "any", role membership, case, and "no opinion" when nothing matches.
// Build + run via `GEN=ninja make test-cpp`.

#include "acl_otel.hpp"
#include "acl_otel_test_util.hpp"
#include "duckdb/common/error_data.hpp"

using namespace duckdb;
using namespace acl_otel_test;

namespace {

acl::Principal Someone(const std::string &subject, const std::string &issuer, std::vector<std::string> roles) {
	acl::Principal p;
	p.subject = subject;
	p.issuer = issuer;
	for (auto &role : roles) {
		p.roles.push_back(role);
	}
	return p;
}

bool Refused(const std::string &json, const std::string &fragment) {
	try {
		acl_otel::ParseLevelRules(json);
	} catch (std::exception &ex) {
		// duckdb's what() is a JSON envelope (quotes escaped); ErrorData gives the message as written
		return ErrorData(ex).RawMessage().find(fragment) != std::string::npos;
	}
	return false;
}

} // namespace

int main() {
	std::printf("test_acl_otel_rules\n");
	Check(acl_otel::ParseLevelRules("").empty(), "an empty document is no rules");
	Check(acl_otel::ParseLevelRules("  [] ").empty(), "an empty array is no rules");
	auto rules = acl_otel::ParseLevelRules(
	    R"([{"role": "analyst", "door": "flight", "level": "all"},
	        {"subject": "u7", "level": "off"},
	        {"issuer": "https://idp.corp/x", "role": "*", "level": "decisions"},
	        {"level": "denied"}])");
	Check(rules.size() == 4, "four rules parsed");
	Check(rules[0].role == "analyst" && rules[0].door == "flight" && rules[0].level == acl::AuditLevel::ALL,
	      "rule 1 as written");
	Check(rules[2].role.empty() && rules[2].issuer == "https://idp.corp/x", "`*` is any");
	Check(rules[3].role.empty() && rules[3].subject.empty() && rules[3].door.empty(), "a bare level matches all");

	Check(Refused("{\"role\": \"x\"}", "expected a JSON array"), "an object is refused");
	Check(Refused("[1]", "rule 1 is not an object"), "a non-object rule is refused");
	Check(Refused("[{\"role\": \"x\", \"level\": \"loud\"}]", "needs a level of off, denied, decisions or all"),
	      "a bad level is refused");
	Check(Refused("[{\"role\": \"x\"}]", "needs a level"), "a rule without a level is refused");
	Check(Refused("[{\"roles\": \"x\", \"level\": \"all\"}]", "unknown key \"roles\""), "a typo key is refused");
	Check(Refused("nope", "not a JSON document"), "non-JSON is refused");

	acl_otel::OtelPolicy policy;
	acl::AuditLevel level;
	Check(!policy.LevelFor(Someone("u1", "https://idp.corp/x", {"analyst"}), "flight", level), "no rules: no opinion");
	policy.SetRules(rules);
	Check(policy.LevelFor(Someone("u1", "https://idp.corp/x", {"viewer", "ANALYST"}), "flight", level) &&
	          level == acl::AuditLevel::ALL,
	      "first match: the analyst through flight is `all` (role membership, case-insensitive)");
	Check(policy.LevelFor(Someone("u1", "https://idp.corp/x", {"analyst"}), "quack", level) &&
	          level == acl::AuditLevel::DECISIONS,
	      "the analyst through quack skips rule 1 (door) and lands on the issuer rule");
	Check(policy.LevelFor(Someone("u7", "https://other", {"analyst"}), "quack", level) && level == acl::AuditLevel::OFF,
	      "the subject rule wins over the default for u7");
	Check(policy.LevelFor(Someone("u9", "https://other", {"viewer"}), "gateway", level) &&
	          level == acl::AuditLevel::DENIED,
	      "the bare default catches the rest");
	policy.SetRules({});
	Check(!policy.LevelFor(Someone("u9", "https://other", {"viewer"}), "gateway", level),
	      "rules cleared: no opinion again");
	std::printf("PASS\n");
	return 0;
}
