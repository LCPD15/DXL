#include "../src/common/StartupDiagnostics.h"
#include "../src/ui/StartupReport.h"
#include <cstdio>
#include <string>
#include <stdexcept>

namespace SD = DXL::StartupDiagnostics;

namespace {
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
std::wstring OwnExe() {
    wchar_t path[32768]{};
    const DWORD count = GetModuleFileNameW(nullptr, path, DWORD(std::size(path)));
    Require(count && count < std::size(path), "GetModuleFileNameW");
    return path;
}

int WriterMain(const wchar_t* mode, const wchar_t* readyName, const wchar_t* exitName) {
    HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, readyName);
    HANDLE stop = OpenEventW(SYNCHRONIZE, FALSE, exitName);
    if (!ready || !stop) return 11;
    SD::Writer diagnostic{};
    HMODULE library = nullptr;
    HANDLE malformed = nullptr;
    void* malformedView = nullptr;
    const std::wstring option(mode);
    if (option == L"dll") {
        const auto exe = OwnExe();
        const auto path = exe.substr(0, exe.find_last_of(L"\\/")) + L"\\startup_diagnostics_fixture.dll";
        library = LoadLibraryW(path.c_str());
        if (!library) return 12;
    } else if (option == L"worker-fail" || option == L"status-fail" || option == L"duplicate") {
        if (!diagnostic.Open()) return 13;
        diagnostic.Mark(SD::WorkerCreateRequested);
        if (option == L"worker-fail") {
            // Explicit synthetic fault: do not exhaust real system resources.
            diagnostic.Created(0, ERROR_NOT_ENOUGH_MEMORY);
        } else {
            diagnostic.Created(GetCurrentThreadId(), ERROR_SUCCESS);
            diagnostic.Entered();
            diagnostic.Mark(SD::LogOpenEntered | SD::LogOpenReturned | SD::StatusOpenEntered);
            diagnostic.StatusResult(option == L"duplicate", option == L"duplicate" ? ERROR_SUCCESS : ERROR_ACCESS_DENIED);
        }
        if (option == L"duplicate") {
            SD::Writer second{};
            if (second.Open()) { second.Close(); return 14; }
        }
    } else if (option != L"absent") {
        wchar_t name[64];
        SD::MakeName(GetCurrentProcessId(), name);
        malformed = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(SD::Block), name);
        if (!malformed) return 15;
        malformedView = MapViewOfFile(malformed, FILE_MAP_WRITE, 0, 0, sizeof(SD::Block));
        if (!malformedView) return 16;
        auto* block = static_cast<SD::Block*>(malformedView);
        block->magic = option == L"bad-magic" ? 0 : SD::Magic;
        block->version = option == L"bad-version" ? SD::Version + 1 : SD::Version;
        block->pid = option == L"bad-pid" ? 0 : LONG(GetCurrentProcessId());
    }
    if (!SetEvent(ready)) return 17;
    const DWORD stopped = WaitForSingleObject(stop, 15000);
    if (library) {
        using Finish = BOOL(*)();
        auto finish = reinterpret_cast<Finish>(GetProcAddress(library, "FinishStartupFixture"));
        if (!finish || !finish()) return 18;
        FreeLibrary(library);
    }
    diagnostic.Close();
    if (malformedView) UnmapViewOfFile(malformedView);
    if (malformed) CloseHandle(malformed);
    CloseHandle(ready);
    CloseHandle(stop);
    return stopped == WAIT_OBJECT_0 ? 0 : 19;
}

struct Child {
    PROCESS_INFORMATION process{};
    HANDLE ready = nullptr;
    HANDLE stop = nullptr;
    std::wstring expectedExe;

    ~Child() {
        if (stop) SetEvent(stop);
        if (process.hProcess) {
            if (WaitForSingleObject(process.hProcess, 5000) != WAIT_OBJECT_0) {
                // Only this exact child handle and verified fixture executable.
                wchar_t path[32768]{};
                DWORD count = DWORD(std::size(path));
                if (QueryFullProcessImageNameW(process.hProcess, 0, path, &count) && expectedExe == path) {
                    TerminateProcess(process.hProcess, 99);
                    WaitForSingleObject(process.hProcess, 3000);
                }
            }
            CloseHandle(process.hProcess);
        }
        if (process.hThread) CloseHandle(process.hThread);
        if (ready) CloseHandle(ready);
        if (stop) CloseHandle(stop);
    }
    void Start(const wchar_t* mode, unsigned ordinal) {
        expectedExe = OwnExe();
        const std::wstring base = L"Local\\DXL.StartupTest." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(ordinal);
        const auto readyName = base + L".ready";
        const auto exitName = base + L".exit";
        ready = CreateEventW(nullptr, TRUE, FALSE, readyName.c_str());
        stop = CreateEventW(nullptr, TRUE, FALSE, exitName.c_str());
        Require(ready && stop, "fixture events");
        std::wstring command = L"\"" + expectedExe + L"\" --writer " + mode + L" " + readyName + L" " + exitName;
        STARTUPINFOW startup{sizeof(startup)};
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        Require(CreateProcessW(expectedExe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, nullptr, &startup, &process) != FALSE, "CreateProcessW fixture");
        const HANDLE waits[]{ready, process.hProcess};
        Require(WaitForMultipleObjects(2, waits, FALSE, 5000) == WAIT_OBJECT_0, "fixture did not publish ready");
    }
    void Finish() {
        Require(SetEvent(stop) != FALSE, "SetEvent fixture exit");
        Require(WaitForSingleObject(process.hProcess, 5000) == WAIT_OBJECT_0, "fixture exit timeout");
        DWORD code = 0;
        Require(GetExitCodeProcess(process.hProcess, &code) && code == 0, "fixture exit error");
    }
};

bool Same(const SD::Snapshot& left, const SD::Snapshot& right) {
    return left.milestones == right.milestones && left.attachThread == right.attachThread &&
        left.workerThread == right.workerThread && left.workerCreateError == right.workerCreateError &&
        left.statusOpenError == right.statusOpenError;
}

void ReadOnlyView(DWORD pid) {
    wchar_t name[64]; SD::MakeName(pid, name);
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
    Require(mapping != nullptr, "OpenFileMapping FILE_MAP_READ");
    const auto* view = static_cast<const SD::Block*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(SD::Block)));
    MEMORY_BASIC_INFORMATION memory{};
    const bool readOnly = view && VirtualQuery(view, &memory, sizeof(memory)) == sizeof(memory) &&
        memory.Protect == PAGE_READONLY && view->magic == SD::Magic;
    if (view) UnmapViewOfFile(view);
    CloseHandle(mapping);
    Require(readOnly, "view must be read-only and readable");
}

void Run(const wchar_t* mode, unsigned ordinal) {
    Child child;
    child.Start(mode, ordinal);
    SD::Snapshot snapshot{};
    DWORD error = 0;
    const std::wstring option(mode);
    const bool invalid = option == L"absent" || option == L"bad-magic" || option == L"bad-version" || option == L"bad-pid";
    if (invalid) {
        snapshot.milestones = 0xFFFFFFFF;
        Require(!SD::Read(child.process.dwProcessId, snapshot, error), "invalid channel unexpectedly readable");
        Require(error == DWORD(option == L"absent" ? ERROR_FILE_NOT_FOUND : ERROR_INVALID_DATA), "invalid channel error");
        Require(snapshot.milestones == 0, "failed Read must clear snapshot");
        const auto report = DXL::StartupReport(child.process.dwProcessId, L"test");
        Require(report.find(L"channel=unavailable") != std::wstring::npos, "invalid report classification");
    } else {
        const ULONGLONG deadline = GetTickCount64() + 3000;
        for (;;) {
            Require(SD::Read(child.process.dwProcessId, snapshot, error) && error == 0, "read cross-process startup");
            if (option != L"dll" || (snapshot.milestones & SD::StatusReady)) break;
            Require(GetTickCount64() < deadline, "real DLL worker milestone timeout");
            Sleep(5);
        }
        Require(snapshot.attachThread != 0 && (snapshot.milestones & SD::ProcessAttach), "attach milestone");
        if (option == L"worker-fail") {
            Require((snapshot.milestones & SD::WorkerCreateFailed) && !(snapshot.milestones & SD::WorkerCreated), "worker failure milestone");
            Require(snapshot.workerThread == 0 && snapshot.workerCreateError == ERROR_NOT_ENOUGH_MEMORY, "worker failure details");
        } else {
            Require(snapshot.milestones & SD::WorkerEntered, "worker entered");
            Require(snapshot.workerThread != 0 && snapshot.workerCreateError == 0, "worker thread details");
            if (option == L"status-fail")
                Require((snapshot.milestones & SD::StatusOpenFailed) && !(snapshot.milestones & SD::StatusReady) &&
                    snapshot.statusOpenError == ERROR_ACCESS_DENIED, "explicit synthetic status error");
            else
                Require((snapshot.milestones & SD::StatusReady) && snapshot.statusOpenError == 0, "status ready");
            if (option == L"dll") {
                constexpr DWORD complete = SD::ProcessAttach | SD::WorkerCreateRequested | SD::WorkerCreated |
                    SD::WorkerEntered | SD::LogOpenEntered | SD::LogOpenReturned | SD::StatusOpenEntered | SD::StatusReady;
                Require((snapshot.milestones & complete) == complete, "real DLL complete startup stage chain");
                Require(!(snapshot.milestones & (SD::WorkerCreateFailed | SD::StatusOpenFailed)), "real DLL unexpected failed stage");
                Require(snapshot.attachThread != snapshot.workerThread, "real CreateThread must use distinct thread");
            }
        }
        ReadOnlyView(child.process.dwProcessId);
        for (unsigned attempt = 0; attempt < 16; ++attempt) {
            SD::Snapshot repeated{};
            Require(SD::Read(child.process.dwProcessId, repeated, error) && Same(snapshot, repeated), "repeated read changed startup state");
        }
        const auto report = DXL::StartupReport(child.process.dwProcessId, L"test");
        Require(report.find(L"attach=1") != std::wstring::npos, "attach report");
        if (option == L"worker-fail") Require(report.find(L"workerError=8") != std::wstring::npos, "worker error report");
        if (option == L"status-fail") Require(report.find(L"statusError=5") != std::wstring::npos, "status error report");
    }
    child.Finish();
    printf("PASS startup mode=%ls pid=%lu\n", mode, child.process.dwProcessId);
}
}

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 5 && std::wstring(argv[1]) == L"--writer") return WriterMain(argv[2], argv[3], argv[4]);
        Require(argc == 1, "invalid fixture arguments");
        wchar_t name[64];
        SD::MakeName(0, name); Require(std::wstring(name) == L"Local\\DXL.Startup.0", "PID zero name");
        SD::MakeName(4294967295UL, name); Require(std::wstring(name) == L"Local\\DXL.Startup.4294967295", "PID maximum name");
        SD::MakeName(1000, name); Require(std::wstring(name) == L"Local\\DXL.Startup.1000", "PID decimal order");
        printf("PASS startup PID name boundaries\n");
        unsigned ordinal = 0;
        for (const auto* mode : {L"dll", L"duplicate", L"worker-fail", L"status-fail", L"bad-magic", L"bad-version", L"bad-pid", L"absent"})
            Run(mode, ++ordinal);
        printf("PASS startup diagnostics: 8 cross-process scenarios; real DLL attach/worker, read-only views, explicit synthetic faults\n");
        return 0;
    } catch (const std::exception& error) {
        fprintf(stderr, "FAIL startup diagnostics: %s (Win32=%lu)\n", error.what(), GetLastError());
        return 1;
    }
}
