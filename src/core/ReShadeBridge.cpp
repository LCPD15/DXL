#include "ReShadeBridge.h"
#include "../common/PostFxCatalog.h"
#include "../common/SettingsReader.h"
#include "../common/Log.h"
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <set>
#include <map>
#include <cwctype>
#include <cstdio>

namespace DXL {
using Microsoft::WRL::ComPtr;
namespace fs=std::filesystem;
namespace {
thread_local bool insideReShade=false;
struct InternalScope { bool before=insideReShade; InternalScope(){insideReShade=true;} ~InternalScope(){insideReShade=before;} };
std::string Utf8(const fs::path& p){return PostFxUtf8(p.wstring());}
bool PlainPath(const fs::path& path) {
    const DWORD flags=GetFileAttributesW(path.c_str());
    return flags!=INVALID_FILE_ATTRIBUTES && !(flags&FILE_ATTRIBUTE_REPARSE_POINT);
}
void EnsurePlainDirectory(const fs::path& path) {
    const auto absolute=fs::absolute(path).lexically_normal();
    auto current=absolute.root_path();
    for(const auto& component:absolute.relative_path()) {
        current/=component;
        const DWORD flags=GetFileAttributesW(current.c_str());
        if(flags!=INVALID_FILE_ATTRIBUTES) {
            if((flags&FILE_ATTRIBUTE_REPARSE_POINT)||!(flags&FILE_ATTRIBUTE_DIRECTORY))
                throw std::runtime_error("ReShade cache directory is not a plain directory");
        } else fs::create_directory(current);
    }
}
bool SupportFile(const fs::path& path) {
    auto ext=path.extension().wstring();for(auto& c:ext)c=towlower(c);
    return ext==L".fx"||ext==L".fxh"||ext==L".h"||ext==L".hlsl"||ext==L".png"||
        ext==L".jpg"||ext==L".jpeg"||ext==L".bmp"||ext==L".dds"||ext==L".tga"||ext==L".hdr";
}
uint64_t Fingerprint(const fs::path& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if(!GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&data))return 0;
    return ((uint64_t(data.ftLastWriteTime.dwHighDateTime)<<32)|data.ftLastWriteTime.dwLowDateTime) ^
        ((uint64_t(data.nFileSizeHigh)<<32)|data.nFileSizeLow)*1099511628211ull;
}
}
struct ReShadeBridge::State {
    using Create=bool(*)(reshade::api::device_api,void*,void*,void*,const char*,reshade::api::effect_runtime**);
    using Destroy=void(*)(reshade::api::effect_runtime*);
    using Present=void(*)(reshade::api::effect_runtime*);
    HMODULE library=nullptr;
    Create create=nullptr; Destroy destroy=nullptr; Present present=nullptr;
    void(*flush)()=nullptr;
    bool(*savePreset)(reshade::api::effect_runtime*)=nullptr;
    bool pendingSave=false;
    reshade::api::effect_runtime* runtime=nullptr;
    ComPtr<IUnknown> chainIdentity, deviceIdentity, queueIdentity;
    HWND runtimeWindow=nullptr;
    fs::path root, stateRoot, staged, config, preset;
    std::map<fs::path,uint64_t> files;
    std::map<fs::path,bool> dxlFiles;
    uint64_t nextScan=0, nextAttempt=0;
    bool hasEffects=false, changed=false, stopped=false, firstScan=true;
    std::string error;
    // Legacy OpenGL/DX9 is already captured into this D3D11 device by DXL.
    // ReShade owns a private, never-presented swapchain on that same device.
    HWND window=nullptr;
    ComPtr<IDXGISwapChain> bridge;
    ComPtr<ID3D11Device> bridgeDevice;
    D3D11_TEXTURE2D_DESC bridgeDesc{};
    std::vector<uint64_t> ordered;

    void Save() {
        if(runtime){pendingSave=true;if(savePreset&&savePreset(runtime))pendingSave=false;}
    }
    void Reset() {
        if(runtime){Save();destroy(runtime);runtime=nullptr;}
        chainIdentity.Reset(); deviceIdentity.Reset(); queueIdentity.Reset(); ordered.clear();runtimeWindow=nullptr;pendingSave=false;
    }
    void ResetBridge() {
        Reset(); bridge.Reset(); bridgeDevice.Reset(); bridgeDesc={};
        if(window){DestroyWindow(window);window=nullptr;}
    }
    void SetPaths(HMODULE module) {
        if(!root.empty())return;
        root=PostFxRoot(module);
#ifdef DXL_TEST_RESHADE_PATHS
        stateRoot=fs::current_path()/L"state"/L"ReShade"/L"fixture";
        staged=stateRoot/L"effects";config=stateRoot/L"DXL-ReShade.ini";
        preset=fs::current_path()/L"state"/L"profiles"/L"fixture.reshade.ini";
#else
        const auto params=SettingsReader::FindParamsPath(module);
        if(params.empty()||root.empty())throw std::runtime_error("Cannot locate ReShade settings directory");
        auto name=params.filename().wstring();
        name.resize(name.size()-std::wstring(L".params.json").size());
        const auto settings=params.parent_path().parent_path();
        stateRoot=settings/L"ReShade"/name;
        staged=stateRoot/L"effects";config=stateRoot/L"DXL-ReShade.ini";
        preset=params.parent_path()/(name+L".reshade.ini");
#endif
        EnsurePlainDirectory(staged);EnsurePlainDirectory(preset.parent_path());
        if(fs::exists(preset)&&!PlainPath(preset))throw std::runtime_error("ReShade preset cannot be a link");
        if(!fs::exists(preset))std::ofstream(preset)<<"Techniques=\nTechniqueSorting=\n";
    }
    void Scan(HMODULE module) {
        if(stopped)return;
        const auto now=GetTickCount64();if(now<nextScan)return;nextScan=now+3000;
        SetPaths(module);
        std::map<fs::path,uint64_t> current;
        std::set<std::wstring> effectNames;
        bool found=false;
        if(PlainPath(root)&&fs::is_directory(root)) {
            size_t entries=0;
            for(fs::recursive_directory_iterator it(root,fs::directory_options::skip_permission_denied),end;it!=end;++it) {
                if(++entries>16384)throw std::runtime_error("Too many post-processing files (maximum 16384)");
                if(!PlainPath(it->path())){if(it->is_directory())it.disable_recursion_pending();continue;}
                if(!it->is_regular_file()||!SupportFile(it->path()))continue;
                const auto relative=it->path().lexically_relative(root);
                const auto fingerprint=Fingerprint(it->path());
                auto ext=it->path().extension().wstring();for(auto& c:ext)c=towlower(c);
                if(ext==L".fx") {
                    bool isDxl=false;
                    const auto previous=files.find(relative);
                    if(previous!=files.end()&&previous->second==fingerprint)isDxl=dxlFiles[relative];
                    else {
                        if(it->file_size()>8*1024*1024)throw std::runtime_error("ReShade effect exceeds 8 MiB");
                        std::ifstream stream(it->path(),std::ios::binary);
                        if(!stream)throw std::runtime_error("Cannot read ReShade effect source");
                        std::string source((std::istreambuf_iterator<char>(stream)),{});
                        isDxl=HasDxlFxEntry(source);dxlFiles[relative]=isDxl;
                    }
                    if(isDxl)continue;
                    auto name=it->path().filename().wstring();for(auto& c:name)c=towlower(c);
                    if(!effectNames.insert(name).second)throw std::runtime_error("ReShade .fx filenames must be unique across subfolders");
                    found=true;
                }
                current.emplace(relative,fingerprint);
            }
        }
        if(firstScan||current!=files) {
            // Join outstanding compiler workers before replacing their inputs.
            Reset();
            EnsurePlainDirectory(staged);
            // Reconcile the on-disk cache too: files removed while the game was
            // closed must not reappear as compiled effects next time it starts.
            // Remove before copying so a case-only Windows filename rename does
            // not delete the newly copied destination.
            for(fs::recursive_directory_iterator it(staged,fs::directory_options::skip_permission_denied),end;it!=end;++it) {
                if(!PlainPath(it->path()))throw std::runtime_error("ReShade cache contains a link");
                if(!it->is_regular_file())continue;
                const auto relative=it->path().lexically_relative(staged);
                if(SupportFile(it->path())&&!current.contains(relative))fs::remove(it->path());
            }
            for(const auto& [relative,fingerprint]:current) {
                const auto target=staged/relative;
                EnsurePlainDirectory(target.parent_path());
                if(fs::exists(target)&&!PlainPath(target))throw std::runtime_error("ReShade cache file cannot be a link");
                if(!files.contains(relative)||files[relative]!=fingerprint||!fs::exists(target))
                    fs::copy_file(root/relative,target,fs::copy_options::overwrite_existing);
            }
            files=std::move(current);changed=true;nextAttempt=0;firstScan=false;
        }
        hasEffects=found;
    }
    bool LoadLibrary(HMODULE module) {
        if(library)return create!=nullptr;
        auto file=PostFxRoot(module).parent_path()/L"DXL-ReShade.dll";
        library=LoadLibraryExW(file.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!library){error="DXL-ReShade.dll is missing or could not be loaded";return false;}
        create=reinterpret_cast<Create>(GetProcAddress(library,"ReShadeCreateEffectRuntime"));
        destroy=reinterpret_cast<Destroy>(GetProcAddress(library,"ReShadeDestroyEffectRuntime"));
        present=reinterpret_cast<Present>(GetProcAddress(library,"ReShadeUpdateAndPresentEffectRuntime"));
        flush=reinterpret_cast<void(*)()>(GetProcAddress(library,"DxlReShadeFlushIni"));
        savePreset=reinterpret_cast<bool(*)(reshade::api::effect_runtime*)>(GetProcAddress(library,"DxlReShadeSavePreset"));
        const auto apiVersion=reinterpret_cast<const unsigned int*>(GetProcAddress(library,"DxlReShadeApiVersion"));
        if(!GetProcAddress(library,"DxlReShadeVersion")||!apiVersion||*apiVersion!=20||!create||!destroy||!present||!flush||!savePreset) {
            FreeLibrary(library);library=nullptr;create=nullptr;destroy=nullptr;present=nullptr;flush=nullptr;
            error="Incompatible DXL ReShade runtime; reinstall the complete DXL package";return false;
        }
        if(auto open=reinterpret_cast<bool(*)(const wchar_t*)>(GetProcAddress(library,"DxlReShadeOpenLog"))) {
            const auto logs=stateRoot.parent_path().parent_path()/L"logs";fs::create_directories(logs);
            open((logs/(L"DXL-ReShade-"+std::to_wstring(GetCurrentProcessId())+L".log")).c_str());
        }
        return true;
    }
    void Config() {
        EnsurePlainDirectory(config.parent_path());
        EnsurePlainDirectory(stateRoot/L"cache");
        if(fs::exists(config)&&!PlainPath(config))throw std::runtime_error("ReShade runtime configuration cannot be a link");
        std::ofstream out(config,std::ios::binary|std::ios::trunc);
        out<<"[GENERAL]\nEffectSearchPaths="<<Utf8(staged/L"**")<<"\nTextureSearchPaths="<<Utf8(staged/L"**")
            <<"\nIntermediateCachePath="<<Utf8(stateRoot/L"cache")<<"\nPresetPath="<<Utf8(preset)
            <<"\nNoReloadOnInit=0\nSkipLoadingDisabledEffects=0\nPerformanceMode=0\nPresetTransitionDuration=0\n"
            <<"[INPUT]\nKeyEffects=0,0,0,0\nKeyNextPreset=0,0,0,0\nKeyPreviousPreset=0,0,0,0\nKeyReload=0,0,0,0\nKeyScreenshot=0,0,0,0\n";
        if(!out)throw std::runtime_error("Cannot write ReShade runtime configuration");
    }
    bool Render(IDXGISwapChain* chain,IUnknown* device,ID3D12CommandQueue* queue,HMODULE module,bool enabled) {
        Scan(module);if(!hasEffects||stopped||!chain||!device)return false;
        ComPtr<IUnknown> identity,devIdentity,qIdentity;
        chain->QueryInterface(IID_PPV_ARGS(&identity));device->QueryInterface(IID_PPV_ARGS(&devIdentity));
        if(queue)queue->QueryInterface(IID_PPV_ARGS(&qIdentity));
        if(runtime&&(chainIdentity!=identity||deviceIdentity!=devIdentity||queueIdentity!=qIdentity))Reset();
        if(!runtime) {
            const auto now=GetTickCount64();if(now<nextAttempt)return false;nextAttempt=now+10000;
            if(!LoadLibrary(module)||!create)return false;
            Config();
            const auto api=queue?reshade::api::device_api::d3d12:reshade::api::device_api::d3d11;
            if(!create(api,device,queue,chain,Utf8(config).c_str(),&runtime)||!runtime) {
                error="ReShade runtime could not initialize this presentation buffer";return false;
            }
            chainIdentity=identity;deviceIdentity=devIdentity;queueIdentity=qIdentity;changed=false;
            DXGI_SWAP_CHAIN_DESC desc{};if(SUCCEEDED(chain->GetDesc(&desc)))runtimeWindow=desc.OutputWindow;
            error.clear();D5_LOG_INFO(L"DXL ReShade runtime attached to final output; API=%u",unsigned(api));
        }
        runtime->set_effects_state(enabled);
        present(runtime);
        if(pendingSave)Save();
        // Keep files alphabetical while retaining declared pass/technique order
        // inside a file. ReShade handles each technique's multipass dependencies.
        struct Entry {reshade::api::effect_technique handle;std::string file;};std::vector<Entry> entries;
        runtime->enumerate_techniques(nullptr,[](reshade::api::effect_runtime* r,reshade::api::effect_technique h,void* user){
            size_t size=0;r->get_technique_effect_name(h,nullptr,&size);std::string name(size,'\0');
            if(size)r->get_technique_effect_name(h,name.data(),&size);if(!name.empty()&&name.back()=='\0')name.pop_back();
            static_cast<std::vector<Entry>*>(user)->push_back({h,std::move(name)});
        },&entries);
        std::stable_sort(entries.begin(),entries.end(),[](const Entry& a,const Entry& b){return PostFxFilenameLess(a.file,b.file);});
        std::vector<uint64_t> signature;std::vector<reshade::api::effect_technique> order;
        for(const auto& entry:entries){signature.push_back(entry.handle.handle);order.push_back(entry.handle);}
        if(signature!=ordered){runtime->reorder_techniques(order.size(),order.data());ordered=std::move(signature);}
        return enabled&&!entries.empty();
    }
};
bool ReShadeBridge::InternalCall() noexcept{return insideReShade;}
bool ReShadeBridge::Needed(HMODULE module) noexcept {
    try {if(!_state)_state=new State;InternalScope scope;_state->Scan(module);return _state->hasEffects&&!_state->stopped;}
    catch(const std::exception& e){if(_state)_state->error=e.what();return false;}
    catch(...){return false;}
}
bool ReShadeBridge::Render(IDXGISwapChain* chain,IUnknown* device,ID3D12CommandQueue* queue,HMODULE module,bool enabled) noexcept {
    try {if(!_state)_state=new State;InternalScope scope;return _state->Render(chain,device,queue,module,enabled);}
    catch(const std::exception& e){if(_state)_state->error=e.what();return false;}
    catch(...){return false;}
}
bool ReShadeBridge::RenderTexture11(ID3D11Device* device,ID3D11Texture2D* image,HMODULE module,bool enabled) noexcept {
    if(!device||!image||!Needed(module))return false;
    try {
        InternalScope scope;auto& s=*_state;D3D11_TEXTURE2D_DESC desc{};image->GetDesc(&desc);
        if(desc.SampleDesc.Count!=1||desc.ArraySize!=1||desc.MipLevels!=1){s.error="Unsupported legacy ReShade input texture";return false;}
        if(s.bridge&&(s.bridgeDevice.Get()!=device||s.bridgeDesc.Width!=desc.Width||s.bridgeDesc.Height!=desc.Height||s.bridgeDesc.Format!=desc.Format))s.ResetBridge();
        if(!s.bridge) {
            // Built-in STATIC window class avoids any custom WndProc lifetime.
            s.window=CreateWindowExW(0,L"STATIC",L"DXL ReShade offscreen",WS_POPUP,0,0,desc.Width,desc.Height,nullptr,nullptr,module,nullptr);
            if(!s.window){s.error="Cannot create the offscreen ReShade window";return false;}
            ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;ComPtr<IDXGIFactory> factory;
            if(FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi)))||FAILED(dxgi->GetAdapter(&adapter))||FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))){s.ResetBridge();return false;}
            DXGI_SWAP_CHAIN_DESC sc{};sc.BufferDesc.Width=desc.Width;sc.BufferDesc.Height=desc.Height;sc.BufferDesc.Format=desc.Format;
            sc.SampleDesc.Count=1;sc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT|DXGI_USAGE_SHADER_INPUT;sc.BufferCount=1;
            sc.OutputWindow=s.window;sc.Windowed=TRUE;sc.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
            if(FAILED(factory->CreateSwapChain(device,&sc,&s.bridge))){s.ResetBridge();s.error="Cannot create the offscreen ReShade swapchain";return false;}
            factory->MakeWindowAssociation(s.window,DXGI_MWA_NO_ALT_ENTER|DXGI_MWA_NO_WINDOW_CHANGES);
            s.bridgeDevice=device;s.bridgeDesc=desc;
        }
        ComPtr<ID3D11Texture2D> back;ComPtr<ID3D11DeviceContext> context;device->GetImmediateContext(&context);
        if(FAILED(s.bridge->GetBuffer(0,IID_PPV_ARGS(&back))))return false;
        context->CopyResource(back.Get(),image);
        const bool applied=s.Render(s.bridge.Get(),device,nullptr,module,enabled);
        if(applied)context->CopyResource(image,back.Get());
        return applied;
    }catch(const std::exception& e){if(_state)_state->error=e.what();return false;}catch(...){return false;}
}
void ReShadeBridge::BeforeResize(IDXGISwapChain* chain) noexcept {
    if(!_state||!_state->runtime||!chain)return;
    ComPtr<IUnknown> identity;chain->QueryInterface(IID_PPV_ARGS(&identity));
    if(identity==_state->chainIdentity){InternalScope scope;_state->Reset();_state->nextAttempt=0;}
}
void ReShadeBridge::BeforeCreate(HWND window) noexcept {
    // A flip-model swapchain must release all buffers before another chain can
    // be created for the same HWND. Drop our old chain reference at that point.
    if(_state&&window&&_state->runtimeWindow==window) {
        InternalScope scope;_state->Reset();_state->nextAttempt=0;
    }
}
void ReShadeBridge::Shutdown() noexcept {if(_state){InternalScope scope;_state->ResetBridge();_state->stopped=true;}}
void ReShadeBridge::Reload() noexcept {if(_state){InternalScope scope;_state->Reset();_state->nextScan=0;_state->nextAttempt=0;}}
void ReShadeBridge::Save() noexcept {if(_state){InternalScope scope;_state->Save();}}
reshade::api::effect_runtime* ReShadeBridge::Runtime() const noexcept {return _state?_state->runtime:nullptr;}
const char* ReShadeBridge::Error() const noexcept {return _state?_state->error.c_str():"";}
}
