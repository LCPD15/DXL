#include "../src/common/StartupDiagnostics.h"

namespace {
DXL::StartupDiagnostics::Writer diagnostic{};
HANDLE worker = nullptr;

DWORD WINAPI Initialize(LPVOID) {
    using namespace DXL::StartupDiagnostics;
    diagnostic.Entered();
    // No rendering or actual logging is needed to exercise startup transport.
    diagnostic.Mark(LogOpenEntered | LogOpenReturned | StatusOpenEntered);
    diagnostic.StatusResult(true, ERROR_SUCCESS);
    return 0;
}
}

// Called by the fixture host outside loader lock before unloading the DLL.
extern "C" __declspec(dllexport) BOOL FinishStartupFixture() {
    if (!worker) return FALSE;
    const DWORD wait = WaitForSingleObject(worker, 3000);
    if (wait != WAIT_OBJECT_0) return FALSE;
    CloseHandle(worker);
    worker = nullptr;
    return TRUE;
}

BOOL WINAPI DllMain(HMODULE, DWORD reason, LPVOID) {
    using namespace DXL::StartupDiagnostics;
    if (reason == DLL_PROCESS_ATTACH) {
        if (!diagnostic.Open()) return FALSE;
        diagnostic.Mark(WorkerCreateRequested);
        DWORD tid = 0;
        worker = CreateThread(nullptr, 0, Initialize, nullptr, 0, &tid);
        const DWORD error = worker ? ERROR_SUCCESS : GetLastError();
        diagnostic.Created(tid, error);
        return worker != nullptr;
    }
    if (reason == DLL_PROCESS_DETACH) {
        diagnostic.Mark(ProcessDetach);
        diagnostic.Close();
    }
    return TRUE;
}
