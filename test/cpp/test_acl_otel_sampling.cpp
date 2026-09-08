// The sampler of spec 005 (R6.1), without a network or an SDK: what a ratio means, what a per-role
// document means, what is never sampled at all, and that the decision is deterministic in the
// event's seq - two nodes with the same setting keep the same statements.
// Run via `make test-cpp`.

#include "acl_otel.hpp"
#include "acl_otel_test_util.hpp"

#include "duckdb/common/error_data.hpp"

#include <functional>

using namespace duckdb;
using namespace acl_otel_test;

namespace {

acl::AuditEvent MakeEvent(int64_t seq, const string &kind, bool allowed, const vector<string> &roles = {}) {
	acl::AuditEvent event;
	event.seq = seq;
	event.kind = kind;
	event.allowed = allowed;
	event.principal.roles = roles;
	return event;
}

int64_t Kept(const acl_otel::Sampler &sampler, int64_t count, const vector<string> &roles = {}) {
	int64_t kept = 0;
	for (int64_t seq = 1; seq <= count; seq++) {
		kept += sampler.Keep(MakeEvent(seq, "statement", true, roles)) ? 1 : 0;
	}
	return kept;
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
	std::printf("test_acl_otel_sampling\n");
	{
		acl_otel::Sampler everything("1");
		Check(everything.KeepsEverything() && Kept(everything, 1000) == 1000, "'1' keeps every allowed statement");
		acl_otel::Sampler none("0");
		Check(Kept(none, 1000) == 0, "'0' keeps none of them");
		acl_otel::Sampler blank("");
		Check(blank.KeepsEverything(), "an empty setting is the default: everything");
	}
	{
		// a ratio keeps roughly its share, and exactly the same share twice (R6.1's determinism)
		acl_otel::Sampler tenth("0.1");
		auto kept = Kept(tenth, 10000);
		Check(kept > 500 && kept < 1500, "a tenth keeps roughly a tenth of 10000 (" + std::to_string(kept) + ")");
		Check(Kept(tenth, 10000) == kept, "...and the same events every time: the decision is in the seq");
		acl_otel::Sampler same("0.1");
		Check(Kept(same, 10000) == kept, "...so a second node with the same setting keeps the same ones");
	}
	{
		// only an allowed decision is eligible; everything else is the record the audit exists for
		acl_otel::Sampler none("0");
		Check(none.Keep(MakeEvent(1, "statement", false)), "a refusal is never sampled");
		Check(none.Keep(MakeEvent(2, "session", true)) && none.Keep(MakeEvent(3, "door", true)) &&
		          none.Keep(MakeEvent(4, "policy", true)) && none.Keep(MakeEvent(5, "keys", true)) &&
		          none.Keep(MakeEvent(6, "ingest", true)),
		      "a session, a door, a policy, a keys and an ingest event are never sampled");
		Check(none.Keep(MakeEvent(7, "admin", true)),
		      "an admin decision is never sampled either: it is a change to the policy");
		Check(!none.Keep(MakeEvent(8, "statement", true)), "an allowed statement is the only thing that is");
	}
	{
		// per role: the highest ratio any of the principal's roles names, the `*` for the rest
		acl_otel::Sampler roles(R"({"analyst": 0, "auditor": 1, "*": 0})");
		Check(roles.RatioFor({"analyst"}) == 0.0, "the role's own ratio");
		Check(roles.RatioFor({"auditor"}) == 1.0, "...each of them");
		Check(roles.RatioFor({"analyst", "auditor"}) == 1.0,
		      "a principal with two roles is judged by the higher: sampling never thins by association");
		Check(roles.RatioFor({"nobody"}) == 0.0, "a role the document does not name gets the '*'");
		Check(Kept(roles, 100, {"auditor"}) == 100 && Kept(roles, 100, {"analyst"}) == 0,
		      "and that is what the events see");
	}
	{
		acl_otel::Sampler no_star(R"({"analyst": 0})");
		Check(no_star.RatioFor({"nobody"}) == 1.0, "with no '*', a role nobody named keeps everything");
	}
	{
		Check(Refusal([] { acl_otel::Sampler("1.5"); }).find("between 0 and 1") != string::npos,
		      "a ratio above one is refused");
		Check(Refusal([] { acl_otel::Sampler("-0.5"); }).find("between 0 and 1") != string::npos, "...and below zero");
		Check(Refusal([] { acl_otel::Sampler("often"); }).find("neither a ratio nor a JSON object") != string::npos,
		      "a word is refused");
		Check(Refusal([] { acl_otel::Sampler("[0.5]"); }).find("expected a ratio or a JSON object") != string::npos,
		      "an array is refused");
		Check(Refusal([] { acl_otel::Sampler(R"({"analyst": "half"})"); }).find("is not a number") != string::npos,
		      "a ratio that is not a number is refused");
		Check(Refusal([] { acl_otel::Sampler(R"({"analyst": 2})"); }).find("not between 0 and 1") != string::npos,
		      "a role's ratio out of range is refused");
	}
	std::printf("PASS\n");
	return 0;
}
