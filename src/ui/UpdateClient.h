#pragma once

// Included after the host's path, JSON and UI transport helpers.
constexpr UINT WM_APP_UPDATE_DONE=WM_APP+7;
std::atomic<bool> g_updateBusy{false};
bool g_updateChecked=false;

void StartUpdateAction(const std::string& action, const std::string& expected = {}) {
    if (g_updateBusy.exchange(true)) return;
    try {
        const auto cache=ConfigDir()/L"updates";
        const auto job=cache/L"jobs"/(std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(job/L"updater");
        // Run a private copy so installation can replace the original updater.
        std::filesystem::copy_file(ExeDir()/L"DXL-update.exe",job/L"DXL-update.exe");
        for (const auto name : {L"Update.ps1",L"UpdateEngine.psm1"})
            std::filesystem::copy_file(ExeDir()/L"updater"/name,job/L"updater"/name);
        const auto request=job/L"request.json";
        const auto result=job/L"result.json";
        const std::string json="{\"action\":\""+action+"\",\"current\":\"0.2\",\"cache\":"+JsonQuoted(cache.wstring())+
            ",\"expected\":"+JsonQuoted(Utf8ToWide(expected))+",\"install\":"+JsonQuoted(ExeDir().wstring())+",\"parentPid\":"+std::to_string(GetCurrentProcessId())+
            ",\"lang\":\""+(g_uiLang.load()==2?"en":"zh")+"\"}";
        if (!WriteFileUtf8(request,json)) throw std::runtime_error("Cannot save update request");
        const auto helper=job/L"DXL-update.exe";
        auto cmd=L"\""+helper.wstring()+L"\" \""+request.wstring()+L"\"";
        STARTUPINFOW si{sizeof(si)}; si.dwFlags=STARTF_USESHOWWINDOW; si.wShowWindow=SW_HIDE;
        PROCESS_INFORMATION pi{};
        bool needsElevation=false;
        if (action=="install") {
            const auto probe=ExeDir()/(L".dxl-update-"+std::to_wstring(GetCurrentProcessId())+L".tmp");
            HANDLE file=CreateFileW(probe.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,nullptr);
            if (file!=INVALID_HANDLE_VALUE) CloseHandle(file);
            else needsElevation=GetLastError()==ERROR_ACCESS_DENIED;
        }
        if (needsElevation) {
            const auto parameters=L"\""+request.wstring()+L"\"";
            SHELLEXECUTEINFOW launch{sizeof(launch)};
            launch.fMask=SEE_MASK_NOCLOSEPROCESS; launch.hwnd=g_window; launch.lpVerb=L"runas";
            launch.lpFile=helper.c_str(); launch.lpParameters=parameters.c_str(); launch.lpDirectory=job.c_str(); launch.nShow=SW_HIDE;
            if (!ShellExecuteExW(&launch)) throw std::runtime_error("Updater elevation was canceled or failed");
            pi.hProcess=launch.hProcess;
        } else {
            if (!CreateProcessW(helper.c_str(),cmd.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,job.c_str(),&si,&pi))
                throw std::runtime_error("Cannot start updater");
            CloseHandle(pi.hThread);
        }
        if (action=="install") {
            CloseHandle(pi.hProcess);
            PostMessageW(g_window,WM_CLOSE,0,0);
            return;
        }
        const HWND window=g_window;
        std::thread([process=pi.hProcess,result,window,action] {
            WaitForSingleObject(process,INFINITE); CloseHandle(process);
            auto response=ReadFileUtf8(result);
            if (response.empty()) response=action=="check"?"{\"state\":\"none\"}":"{\"state\":\"error\"}";
            auto message=new std::string("{\"type\":\"update\",\"payload\":"+response+"}");
            if (!PostMessageW(window,WM_APP_UPDATE_DONE,0,reinterpret_cast<LPARAM>(message))) delete message;
        }).detach();
    } catch (...) {
        g_updateBusy=false;
        if (action!="check") PostToUi("{\"type\":\"update\",\"payload\":{\"state\":\"error\"}}");
    }
}
