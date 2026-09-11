#include "../src/ui/GameSession.h"
#include <cstdio>
#include <fstream>
#include <stdexcept>

using namespace DXL::InjectionDiagnostics;
static void Check(bool ok, const char* reason) {
    if (!ok) throw std::runtime_error(reason);
}
static unsigned frees = 0;
static BOOL WINAPI CountFree(HANDLE, LPVOID memory, SIZE_T size, DWORD type) {
    Check(memory != nullptr && size == 0 && type == MEM_RELEASE, "remote allocation free contract");
    ++frees; return TRUE;
}
static BOOL WINAPI FailFree(HANDLE, LPVOID, SIZE_T, DWORD) {
    ++frees; SetLastError(ERROR_ACCESS_DENIED); return FALSE;
}
struct WaitingReader { HANDLE release; const DWORD* value; };
static DWORD WINAPI ReadAfterWait(void* opaque) {
    auto& reader = *static_cast<WaitingReader*>(opaque);
    return WaitForSingleObject(reader.release, 5000) == WAIT_OBJECT_0 ? *reader.value : 0;
}

int wmain(int argc, wchar_t** argv) try {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX | SEM_NOGPFAULTERRORBOX);
    Check(argc == 2, "pass the absolute test DLL path");
    const std::filesystem::path dll = std::filesystem::absolute(argv[1]);
    Check(std::filesystem::is_regular_file(dll), "test DLL missing");
    const auto pid = GetCurrentProcessId();
    std::wstring error;

    // Run the production injector only against this disposable test process.
    // The fixture DLL deliberately has no DXL status block: this tests loading
    // confirmation, not conflating module presence with core initialization.
    Check(DXL::InjectCore(pid, dll, error), "ordinary test DLL should load and be confirmed");
    Check(error.empty(), "successful injection retained stale error");
    const auto present = InspectModule(GetCurrentProcess(), dll.wstring());
    Check(present.state == ModuleState::Confirmed && present.base != 0, "actual full-path module missing");
    Check(LoadConfirmed(WAIT_OBJECT_0, true, present), "confirmed module + stopped thread");
    const auto getValue = reinterpret_cast<int(*)()>(GetProcAddress(reinterpret_cast<HMODULE>(present.base), "InjectionDiagnosticFixture"));
    Check(getValue && getValue() == 42, "test DLL export was not callable");
    Check(!DXL::IsCoreInjected(pid), "fixture must not create a real core status block");
    puts("PASS actual production LoadLibrary: full-path module + callable fixture; no core IPC fabricated");

    const auto alternate = dll.parent_path() / L"different-directory" / dll.filename();
    const auto absent = InspectModule(GetCurrentProcess(), alternate.wstring());
    Check(absent.state == ModuleState::Absent, "same basename in a different directory must not match");
    Check(!LoadConfirmed(WAIT_OBJECT_0, true, absent), "ended thread without matching DLL is not loading success");
    const auto unknown = InspectModule(nullptr, dll.wstring());
    Check(unknown.state == ModuleState::Unknown && unknown.error != ERROR_SUCCESS, "failed enumeration must be unknown, not absent");
    Check(!LoadConfirmed(WAIT_OBJECT_0, true, unknown), "unavailable module evidence must not be called success");
    Check(!LoadConfirmed(WAIT_TIMEOUT, true, present), "module presence cannot resolve an unfinished thread");
    Check(!LoadConfirmed(WAIT_OBJECT_0, false, present), "failed exit-code query must remain unconfirmed");
    DWORD keyError = 0;
    const auto plain = PathKey(dll.wstring(), keyError);
    const auto extended = PathKey(L"\\\\?\\" + dll.wstring(), keyError);
    Check(!keyError && SamePath(plain, extended), "extended path spelling must compare equal");
    const auto unc = PathKey(L"\\\\server\\share\\folder\\sample.dll", keyError);
    const auto uncExtended = PathKey(L"\\\\?\\UNC\\server\\share\\folder\\sample.dll", keyError);
    Check(!keyError && SamePath(unc, uncExtended), "UNC extended spelling must compare equal");
    puts("PASS module evidence: wrong full path rejected; enumeration failure unknown; completed/timeout/query-failure distinctions");

    // A real failing LoadLibrary call must not become a false 'loaded' result.
    // The tiny invalid DLL stays in this test's build directory only.
    const auto invalid = dll.parent_path() / L"invalid-diagnostic-fixture.dll";
    { std::ofstream out(invalid, std::ios::binary); out << "Not a PE image"; }
    Check(!DXL::InjectCore(pid, invalid, error), "invalid DLL was incorrectly reported loaded");
    Check(error.find(L"0x00000000") != std::wstring::npos, "failure must expose the raw thread return code");
    Check(InspectModule(GetCurrentProcess(), invalid.wstring()).state == ModuleState::Absent, "invalid fixture appeared in modules");
    puts("PASS actual failed LoadLibrary: raw 0x00000000 and no matching module");

    const auto dummyProcess = reinterpret_cast<HANDLE>(uintptr_t(0x1234));
    auto* dummyMemory = reinterpret_cast<void*>(uintptr_t(0x5678));
    frees = 0;
    { RemoteArgument argument(dummyProcess, dummyMemory, &CountFree); }
    Check(frees == 1, "failure before a thread starts must release its argument");
    { RemoteArgument argument(dummyProcess, dummyMemory, &CountFree); argument.ThreadStarted(); }
    Check(frees == 1, "timeout/pending thread freed a still-readable argument");
    { RemoteArgument argument(dummyProcess, dummyMemory, &CountFree); argument.ThreadStarted(); argument.ThreadStopped(); }
    Check(frees == 2, "signaled thread must release its argument");
    {
        RemoteArgument argument(dummyProcess, dummyMemory, &CountFree);
        argument.ThreadStarted(); DWORD freeError = 999;
        Check(!argument.Release(freeError) && argument.Retained(), "explicit cleanup released a pending argument");
    }
    Check(frees == 2, "pending-thread cleanup must never call VirtualFreeEx");
    {
        RemoteArgument argument(dummyProcess, dummyMemory, &FailFree);
        DWORD freeError = 0;
        Check(!argument.Release(freeError) && freeError == ERROR_ACCESS_DENIED, "GetLastError from failed free was lost");
        SetLastError(ERROR_SUCCESS); // A later cleanup must not change captured evidence.
        Check(freeError == ERROR_ACCESS_DENIED, "saved API failure changed during cleanup");
    }
    Check(frees == 3, "failed free was retried after its process handle could be closed");
    puts("PASS remote argument lifetime: before-start cleanup, pending retention, completed cleanup, original Win32 error");

    // Real pending-thread lifetime in this test process: the blocked reader
    // must still be able to consume the parameter after the guard sees timeout.
    auto* parameter = static_cast<DWORD*>(VirtualAlloc(nullptr, sizeof(DWORD), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    Check(parameter != nullptr, "local timeout fixture allocation"); *parameter = 42;
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr); Check(release != nullptr, "reader release event");
    WaitingReader reader{release, parameter};
    HANDLE waiting = CreateThread(nullptr, 0, &ReadAfterWait, &reader, 0, nullptr);
    Check(waiting != nullptr, "local waiting reader");
    {
        RemoteArgument argument(GetCurrentProcess(), parameter);
        argument.ThreadStarted();
        Check(WaitForSingleObject(waiting, 0) == WAIT_TIMEOUT, "fixture reader was not waiting");
    }
    MEMORY_BASIC_INFORMATION memory{};
    Check(VirtualQuery(parameter, &memory, sizeof(memory)) == sizeof(memory) && memory.State == MEM_COMMIT,
        "timeout cleanup freed the pending reader's real argument");
    SetEvent(release); Check(WaitForSingleObject(waiting, 5000) == WAIT_OBJECT_0, "reader failed to finish");
    DWORD readValue = 0; Check(GetExitCodeThread(waiting, &readValue) && readValue == 42, "retained argument could not be read after timeout");
    CloseHandle(waiting); CloseHandle(release);
    Check(VirtualFree(parameter, 0, MEM_RELEASE) != FALSE, "local completed reader cleanup");
    puts("PASS actual timeout lifetime: local blocked thread reads retained parameter after timeout cleanup");

    Check(FreeLibrary(reinterpret_cast<HMODULE>(present.base)) != FALSE, "unload local fixture");
    std::error_code cleanupError; std::filesystem::remove(invalid, cleanupError);
    Check(!cleanupError, "invalid fixture cleanup");
    puts("PASS injection diagnostics (test process only; no games, no alternate loader, no retries)");
    return 0;
} catch (const std::exception& e) { printf("FAIL injection diagnostics: %s\n", e.what()); return 1; }
