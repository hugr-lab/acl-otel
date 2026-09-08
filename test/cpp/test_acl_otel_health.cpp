// The health judgement of spec 006 (R7.3) with the test's own clock, so nothing here sleeps: a node
// is losing while its non-policy drop counters grow, and stays so for the window after the last one.

#include "acl_otel.hpp"
#include "acl_otel_test_util.hpp"

using namespace duckdb;
using namespace acl_otel_test;

int main() {
	std::printf("test_acl_otel_health\n");
	const int64_t second = 1000000;
	{
		acl_otel::Health health;
		int64_t now = 1000 * second;
		// the first judgement only takes a baseline: a count that was already high when we started
		// looking is not news, and every node that ever dropped an event since boot is not a signal
		Check(!health.Losing(42, now, 60), "the first judgement of an old count is not losing");
		Check(health.LosingSince() == 0, "...and nothing is stamped");
		Check(!health.Losing(42, now + second, 60), "a count that does not move stays healthy");
	}
	{
		acl_otel::Health health;
		int64_t now = 1000 * second;
		health.Losing(0, now, 60);
		Check(health.Losing(1, now + second, 60), "a drop makes the node losing");
		Check(health.LosingSince() == now + second, "...stamped when it was seen");
		Check(health.Losing(1, now + 30 * second, 60), "still losing inside the window");
		Check(!health.Losing(1, now + 62 * second, 60), "and healthy again once the window has passed");
	}
	{
		acl_otel::Health health;
		int64_t now = 1000 * second;
		health.Losing(0, now, 60);
		health.Losing(1, now + second, 60);
		Check(health.Losing(2, now + 50 * second, 60), "a further drop is seen");
		Check(health.Losing(2, now + 100 * second, 60), "...and extends the window from ITS moment");
		Check(!health.Losing(2, now + 115 * second, 60), "which then expires in its turn");
	}
	{
		acl_otel::Health health;
		int64_t now = 1000 * second;
		health.Losing(0, now, 0);
		Check(!health.Losing(5, now + 1, 0), "a window of zero is a node that never reports unhealthy");
	}
	std::printf("PASS\n");
	return 0;
}
