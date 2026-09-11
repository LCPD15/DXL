#pragma once

#include "../common/StartupDiagnostics.h"
#include <string>

namespace DXL {
inline std::wstring StartupReport(DWORD pid, const wchar_t* phase) {
    StartupDiagnostics::Snapshot snapshot;
    DWORD error = ERROR_SUCCESS;
    wchar_t text[1024]{};
    if (!StartupDiagnostics::Read(pid, snapshot, error)) {
        _snwprintf_s(text, _TRUNCATE,
            L"[StartupDiag] pid=%lu phase=%s channel=unavailable win32=%lu. "
            L"No startup milestone confirmed; channel creation/access failure is also possible.",
            pid, phase, error);
    } else {
        using namespace StartupDiagnostics;
        const auto bit = [&](LONG flag) { return (snapshot.milestones & flag) ? 1u : 0u; };
        _snwprintf_s(text, _TRUNCATE,
            L"[StartupDiag] pid=%lu phase=%s flags=0x%08lX attach=%u attachTid=%lu "
            L"workerRequested=%u workerCreated=%u workerFailed=%u workerTid=%lu workerError=%lu "
            L"initEntered=%u logEntered=%u logReturned=%u statusEntered=%u statusReady=%u "
            L"statusFailed=%u statusError=%lu detached=%u",
            pid, phase, snapshot.milestones, bit(ProcessAttach), snapshot.attachThread,
            bit(WorkerCreateRequested), bit(WorkerCreated), bit(WorkerCreateFailed), snapshot.workerThread,
            snapshot.workerCreateError, bit(WorkerEntered), bit(LogOpenEntered), bit(LogOpenReturned),
            bit(StatusOpenEntered), bit(StatusReady), bit(StatusOpenFailed), snapshot.statusOpenError,
            bit(ProcessDetach));
    }
    return text;
}
} // namespace DXL
