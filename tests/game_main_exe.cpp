#include "../src/ui/GameScan.h"
#include <fstream>
#include <stdexcept>
#include <cstdio>
static void Check(bool yes,const char* message){if(!yes)throw std::runtime_error(message);}
static void File(const std::filesystem::path& path,size_t size=16){
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path,std::ios::binary);file<<std::string(size,'x');
}
static void ManagedHost(const std::filesystem::path& root){
    const auto name=root.filename().wstring();
    for(const auto suffix:{L".exe",L".dll",L".runtimeconfig.json",L".deps.json"})File(root/(name+suffix));
}
int wmain(int argc,wchar_t** argv)try{
    if(argc<2)return 2;
    const std::filesystem::path output=argv[1];
    const auto managed=output/L"Small MonoGame";ManagedHost(managed);
    File(managed/L"x64/dav2_test.exe",4096);
    File(managed/L"ModdingAPI.exe",2048);File(managed/L"ModdingAPI.runtimeconfig.json");File(managed/L"ModdingAPI.deps.json");File(managed/L"ModdingAPI.dll");
    Check(DXL::scan_detail::PickMainExe(managed)==(managed/L"Small MonoGame.exe").wstring(),"small managed game lost to bundled utility or mod host");
    const auto native=output/L"Native Game";File(native/L"Native Game.exe");File(native/L"Native Game.runtimeconfig.json");File(native/L"Native Game.deps.json");File(native/L"actual-game.exe",4096);
    Check(DXL::scan_detail::PickMainExe(native)==(native/L"actual-game.exe").wstring(),"incomplete managed markers overrode native fallback");
    const auto unreal=output/L"UE Game";ManagedHost(unreal);File(unreal/L"Game/Binaries/Win64/Game-Win64-Shipping.exe",2048);
    Check(std::filesystem::equivalent(DXL::scan_detail::PickMainExe(unreal),unreal/L"Game/Binaries/Win64/Game-Win64-Shipping.exe"),"managed shell overrode UE Shipping");
    const auto unity=output/L"Unity Game";ManagedHost(unity);File(unity/L"Game.exe",32);std::filesystem::create_directories(unity/L"Game_Data");
    Check(DXL::scan_detail::PickMainExe(unity)==(unity/L"Game.exe").wstring(),"managed shell overrode Unity data marker");
    puts("PASS managed install-name host beats larger tools, complete companion requirement, preserved UE/Unity precedence");
    if(argc>2)wprintf(L"INSTALLED_MAIN=%ls\n",DXL::scan_detail::PickMainExe(argv[2]).c_str());
    return 0;
}catch(const std::exception& e){printf("FAIL %s\n",e.what());return 1;}
