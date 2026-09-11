#include "../src/ui/LauncherPreferences.h"
#include <cstdio>

int main() {
	using namespace DXL::LauncherPreferences;
	unsigned checks = 0;
	auto check = [&](bool ok, const char* what) {
		++checks;
		if (!ok) { std::fprintf(stderr, "FAIL %s\n", what); return false; }
		return true;
	};
	if (!check(ReadAdminLaunch(""), "new install defaults to elevation") ||
		!check(ReadAdminLaunch("{}"), "empty preferences use default elevation") ||
		!check(!ReadAdminLaunch("{\"adminLaunch\":\"true\"}"), "string is not opt-in") ||
		!check(!ReadAdminLaunch("{\"adminLaunch\":true"), "truncated config is not opt-in") ||
		!check(!ReadAdminLaunch("{\"profiles\":{\"adminLaunch\":true}}"), "profile cannot enable elevation") ||
		!check(!ReadAdminLaunch("{\"adminLaunch\":false,\"adminLaunch\":true}"), "ambiguous config is not opt-in") ||
		!check(ReadAdminLaunch(" \r\n{ \"adminLaunch\" : true }\t"), "valid whitespace") ||
		!check(ReadAdminLaunch(Serialize(true)), "enabled roundtrip") ||
		!check(!ReadAdminLaunch(Serialize(false)), "disabled roundtrip")) return 1;
	for (bool enabled : {false, true}) for (bool elevated : {false, true}) for (bool attempted : {false, true}) {
		const bool expected = enabled && !elevated && !attempted;
		if (!check(ShouldElevate(enabled, elevated, attempted) == expected, "single-attempt startup decision")) return 1;
	}
	std::printf("PASS %u launcher elevation preference cases; no UAC or profile changes\n", checks);
	return 0;
}
