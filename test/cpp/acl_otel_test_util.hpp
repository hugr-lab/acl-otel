// The one helper the standalone C++ tests share: a check that names what failed and exits non-zero.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>

namespace acl_otel_test {

inline void Check(bool ok, const std::string &what) {
	if (ok) {
		std::printf("  ok:   %s\n", what.c_str());
		return;
	}
	std::printf("  FAIL: %s\n", what.c_str());
	std::exit(1);
}

} // namespace acl_otel_test
