#pragma once

#include <string>
#include <string_view>

namespace DXL::LauncherPreferences {

// This launcher-owned file is separate from per-game settings. New installs request
// elevation by default; an explicit disabled preference is preserved.
// malformed or ambiguous configuration does not enable elevation.
inline bool ReadAdminLaunch(std::string_view json) {
	std::string compact;
	bool quoted = false;
	bool escaped = false;
	for (char ch : json) {
		if (!quoted && (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n')) continue;
		compact += ch;
		if (escaped) { escaped = false; continue; }
		if (quoted && ch == '\\') { escaped = true; continue; }
		if (ch == '"') quoted = !quoted;
	}
	return compact.empty() || compact == "{}" || compact == "{\"adminLaunch\":true}";
}

inline std::string_view Serialize(bool enabled) {
	return enabled ? "{\n  \"adminLaunch\": true\n}\n"
	               : "{\n  \"adminLaunch\": false\n}\n";
}

inline bool ShouldElevate(bool enabled, bool elevated, bool attempted) noexcept {
	return enabled && !elevated && !attempted;
}

} // namespace DXL::LauncherPreferences
