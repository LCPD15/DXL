#pragma once

#include <windows.h>

namespace DXL::StartupDiagnostics {

// Startup-only telemetry. The game creates this mapping with its own token;
// the launcher opens it read-only. No cross-process wait is used by DllMain.
inline constexpr LONG Magic = 0x44584C49;
inline constexpr LONG Version = 1;
enum Milestone : LONG {
    ProcessAttach = 1 << 0,
    WorkerCreateRequested = 1 << 1,
    WorkerCreated = 1 << 2,
    WorkerCreateFailed = 1 << 3,
    WorkerEntered = 1 << 4,
    LogOpenEntered = 1 << 5,
    LogOpenReturned = 1 << 6,
    StatusOpenEntered = 1 << 7,
    StatusReady = 1 << 8,
    StatusOpenFailed = 1 << 9,
    ProcessDetach = 1 << 10,
};

struct Block {
    LONG magic;
    LONG version;
    LONG pid;
    volatile LONG milestones;
    volatile LONG attachThread;
    volatile LONG workerThread;
    volatile LONG workerCreateError;
    volatile LONG statusOpenError;
};
static_assert(sizeof(Block) == 32);

// Avoid CRT formatting and allocation in the DLL entry point.
inline void MakeName(DWORD pid, wchar_t (&name)[64]) noexcept {
    constexpr wchar_t prefix[] = L"Local\\DXL.Startup.";
    unsigned n = 0;
    for (; prefix[n]; ++n) name[n] = prefix[n];
    wchar_t digits[10];
    unsigned count = 0;
    do { digits[count++] = wchar_t(L'0' + pid % 10); pid /= 10; } while (pid);
    while (count) name[n++] = digits[--count];
    name[n] = 0;
}

struct Writer {
    HANDLE mapping = nullptr;
    Block* view = nullptr;

    bool Open() noexcept {
        wchar_t name[64];
        const DWORD pid = GetCurrentProcessId();
        MakeName(pid, name);
        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
            0, sizeof(Block), name);
        const DWORD createError = GetLastError();
        if (!mapping) return false;
        // Do not overwrite an existing object's telemetry (e.g. another copy
        // of the DLL). An unavailable diagnostic channel must not affect NR.
        if (createError == ERROR_ALREADY_EXISTS) { CloseHandle(mapping); mapping = nullptr; return false; }
        view = static_cast<Block*>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(Block)));
        if (!view) { CloseHandle(mapping); mapping = nullptr; return false; }
        view->version = Version;
        view->pid = LONG(pid);
        view->attachThread = LONG(GetCurrentThreadId());
        InterlockedExchange(&view->magic, Magic);
        Mark(ProcessAttach);
        return true;
    }
    void Mark(LONG bits) noexcept {
        if (view) InterlockedOr(&view->milestones, bits);
    }
    void Created(DWORD threadId, DWORD error) noexcept {
        if (!view) return;
        view->workerThread = LONG(threadId);
        view->workerCreateError = LONG(error);
        Mark(error ? WorkerCreateFailed : WorkerCreated);
    }
    void Entered() noexcept { Mark(WorkerEntered); }
    void StatusResult(bool ok, DWORD error) noexcept {
        if (!view) return;
        view->statusOpenError = LONG(error);
        Mark(ok ? StatusReady : StatusOpenFailed);
    }
    void Close() noexcept {
        if (view) UnmapViewOfFile(view);
        if (mapping) CloseHandle(mapping);
        view = nullptr;
        mapping = nullptr;
    }
};

struct Snapshot {
    DWORD milestones = 0;
    DWORD attachThread = 0;
    DWORD workerThread = 0;
    DWORD workerCreateError = 0;
    DWORD statusOpenError = 0;
};

inline bool Read(DWORD pid, Snapshot& out, DWORD& error) noexcept {
    out = {};
    wchar_t name[64];
    MakeName(pid, name);
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
    if (!mapping) { error = GetLastError(); return false; }
    const auto* view = static_cast<const Block*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(Block)));
    if (!view) { error = GetLastError(); CloseHandle(mapping); return false; }
    const bool valid = view->magic == Magic && view->version == Version && DWORD(view->pid) == pid;
    if (valid) {
        // Writers set each milestone after its associated fields. The flags
        // only accumulate, so DllMain and the worker cannot move progress back.
        out.milestones = DWORD(view->milestones);
        MemoryBarrier();
        out.attachThread = DWORD(view->attachThread);
        out.workerThread = DWORD(view->workerThread);
        out.workerCreateError = DWORD(view->workerCreateError);
        out.statusOpenError = DWORD(view->statusOpenError);
    }
    UnmapViewOfFile(view);
    CloseHandle(mapping);
    error = valid ? ERROR_SUCCESS : ERROR_INVALID_DATA;
    return valid;
}

} // namespace DXL::StartupDiagnostics
