#pragma once
#include <windows.h>
#include <string>
#include <string_view>

namespace DXL {
struct ProfileCommandResult { unsigned matched = 0, succeeded = 0; };

// The profile file identifies its game, independently of the status selector.
// The caller validates the file name before dispatch; default.json has no target.
template<class Targets, class Send>
ProfileCommandResult DispatchProfileCommand(const Targets& targets,
    std::wstring_view file, Send&& send) {
    ProfileCommandResult result;
    for (const auto& target : targets) {
        if (!target.pid || target.name.empty()) continue;
        const std::wstring expected = target.name + L".json";
        if (CompareStringOrdinal(file.data(), static_cast<int>(file.size()),
            expected.data(), static_cast<int>(expected.size()), TRUE) != CSTR_EQUAL) continue;
        ++result.matched;
        if (send(target.pid)) ++result.succeeded;
    }
    return result;
}
}
