// DRS cleanup transaction tests. Every NVAPI callback is an in-memory fake.
#include <initializer_list>
#include "../src/core/NvSmoothMotion.h"
#include <map>
#include <string>
#include <vector>
#include <stdexcept>
#include <cstdio>

namespace Sm = DXL::NvSmoothMotion;
namespace {
constexpr wchar_t kTarget[] = L"C:\\isolated games\\sample.exe";
constexpr unsigned kUnrelated = 0x12345678;
constexpr int kFailure = -1234;
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

Sm::DrsSetting Setting(unsigned id, unsigned value, unsigned location=Sm::kDrsCurrentProfileLocation,
    bool predefined=false) {
    Sm::DrsSetting setting{};
    setting.version=NVSM_MAKE_VERSION(Sm::DrsSetting,1);
    setting.settingId=id;
    setting.settingType=Sm::kDrsDwordType;
    setting.settingLocation=location;
    setting.isCurrentPredefined=predefined;
    setting.isPredefinedValid=predefined;
    setting.current.u32Value=value;
    setting.predefined.u32Value=value;
    return setting;
}
struct FakeDrs {
    std::map<unsigned,Sm::DrsSetting> profile,defaults,persisted;
    std::map<unsigned,unsigned> getCalls;
    std::vector<std::wstring> expectedQueries{kTarget},queries;
    std::vector<int> findResults{Sm::kOk};
    std::vector<unsigned> deleted,written;
    unsigned saves=0,creates=0;
    unsigned failGetId=0,failGetCall=0,failDeleteId=0;
    int saveError=Sm::kOk,setError=Sm::kOk;
    bool publicDeleteCannotSeeSmoothMotion=false;
    bool setDoesNothing=false,nullProfile=false,badContract=false;
    void* ProfileHandle() {return &persisted;}
    void Seed(unsigned id,unsigned value) {profile[id]=Setting(id,value);persisted=profile;}
    bool ProfileMatches(void* handle) {
        if(handle==ProfileHandle()) return true;
        badContract=true; return false;
    }
    unsigned Effective(unsigned id) const {
        if(auto found=profile.find(id);found!=profile.end()) return found->second.current.u32Value;
        if(auto found=defaults.find(id);found!=defaults.end()) return found->second.current.u32Value;
        return 0;
    }
    void CheckUntouched(unsigned expectedSaves=0) const {
        Check(!badContract,"NVAPI ABI/target contract mismatch");
        Check(creates==0,"cleanup created a profile/application");
        Check(saves==expectedSaves,"unexpected SaveSettings count");
        if(profile.count(kUnrelated)) {
            Check(profile.at(kUnrelated).current.u32Value==88,"unrelated in-session setting changed");
            Check(persisted.at(kUnrelated).current.u32Value==88,"unrelated persisted setting changed");
        }
        for(auto id:deleted) Check(id==Sm::kSmEnableId || id==Sm::kSmEnableApisId,"deleted unrelated setting");
        for(auto id:written) Check(id==Sm::kSmEnableId,"wrote API restriction or unrelated setting");
    }
};
int __cdecl Find(void* session,const wchar_t* name,void** profile,void* raw) {
    auto& fake=*static_cast<FakeDrs*>(session);
    const auto index=fake.queries.size();
    fake.queries.emplace_back(name ? name : L"");
    if(!raw || static_cast<Sm::DrsApplicationV4*>(raw)->version!=NVSM_MAKE_VERSION(Sm::DrsApplicationV4,4)
        || index>=fake.findResults.size() || index>=fake.expectedQueries.size()
        || fake.expectedQueries[index]!=fake.queries.back()) {
        fake.badContract=true; return Sm::kInvalidArgument;
    }
    *profile=fake.findResults[index]==Sm::kOk && !fake.nullProfile ? fake.ProfileHandle() : nullptr;
    return fake.findResults[index];
}
int __cdecl Get(void* session,void* profile,unsigned id,void* raw,unsigned* extra) {
    auto& fake=*static_cast<FakeDrs*>(session);
    auto* setting=static_cast<Sm::DrsSetting*>(raw);
    if(!fake.ProfileMatches(profile) || !setting || setting->version!=NVSM_MAKE_VERSION(Sm::DrsSetting,1)
        || setting->settingId!=id || !extra || *extra!=0) {
        fake.badContract=true; return Sm::kInvalidArgument;
    }
    const auto call=++fake.getCalls[id];
    if(id==fake.failGetId && call==fake.failGetCall) return kFailure;
    if(auto found=fake.profile.find(id);found!=fake.profile.end()) {*setting=found->second;return Sm::kOk;}
    if(auto found=fake.defaults.find(id);found!=fake.defaults.end()) {*setting=found->second;return Sm::kOk;}
    return Sm::kSettingNotFound;
}
int __cdecl Delete(void* session,void* profile,unsigned id) {
    auto& fake=*static_cast<FakeDrs*>(session);
    if(!fake.ProfileMatches(profile)) return Sm::kInvalidArgument;
    fake.deleted.push_back(id);
    if(fake.publicDeleteCannotSeeSmoothMotion) return Sm::kSettingNotFound;
    if(id==fake.failDeleteId) return kFailure;
    return fake.profile.erase(id) ? Sm::kOk : Sm::kSettingNotFound;
}
int __cdecl Set(void* session,void* profile,void* raw,unsigned x,unsigned y) {
    auto& fake=*static_cast<FakeDrs*>(session);
    auto* setting=static_cast<Sm::DrsSetting*>(raw);
    if(!fake.ProfileMatches(profile) || !setting || setting->version!=NVSM_MAKE_VERSION(Sm::DrsSetting,1)
        || setting->settingType!=Sm::kDrsDwordType || setting->settingLocation!=Sm::kDrsCurrentProfileLocation
        || setting->settingId!=Sm::kSmEnableId || setting->current.u32Value!=0 || x || y) {
        fake.badContract=true; return Sm::kInvalidArgument;
    }
    fake.written.push_back(setting->settingId);
    if(fake.setError!=Sm::kOk) return fake.setError;
    if(!fake.setDoesNothing) fake.profile[setting->settingId]=*setting;
    return Sm::kOk;
}
int __cdecl Save(void* session) {
    auto& fake=*static_cast<FakeDrs*>(session);++fake.saves;
    if(fake.saveError==Sm::kOk) fake.persisted=fake.profile;
    return fake.saveError;
}
int __cdecl CreateProfile(void* session,void*,void**) {++static_cast<FakeDrs*>(session)->creates;return kFailure;}
int __cdecl CreateApplication(void* session,void*,void*) {++static_cast<FakeDrs*>(session)->creates;return kFailure;}
void Bind(Sm::NvApi& api) {
    api.findApplicationByName=Find;api.getSetting=Get;api.deleteProfileSetting=Delete;
    api.setSetting=Set;api.saveSettings=Save;api.createProfile=CreateProfile;api.createApplication=CreateApplication;
    // module/initialize/unload remain null: no driver DLL is loaded or initialized.
}
Sm::SmResult Run(FakeDrs& fake,int& error) {
    Sm::NvApi api;Bind(api);error=Sm::kOk;
    return Sm::CleanupEnabledInSession(api,&fake,kTarget,&error);
}
void UserOn(FakeDrs& fake) {
    fake.Seed(Sm::kSmEnableId,1);fake.Seed(Sm::kSmEnableApisId,7);fake.Seed(kUnrelated,88);
    fake.defaults[Sm::kSmEnableId]=Setting(Sm::kSmEnableId,0,0,true);
    fake.defaults[Sm::kSmEnableApisId]=Setting(Sm::kSmEnableApisId,2,0,true);
}
void NoWrites(const FakeDrs& fake) {
    fake.CheckUntouched();
    Check(fake.deleted.empty() && fake.written.empty(),"unexpected write in a no-op/read-error case");
}
unsigned cases=0;
template<class Fn> void Case(const char* name,Fn action) {
    action();++cases;std::printf("PASS %s\n",name);
}
}

int main() try {
    const HMODULE driverBefore=GetModuleHandleW(L"nvapi64.dll");
    Case("invalid targets return before loading NVAPI",[]{
        for(const wchar_t* target:std::initializer_list<const wchar_t*>{nullptr,L"",L"sample.dll",L"C:\\isolated\\sample"}) {
            int error=0;
            Check(Sm::CleanupEnabledSmoothMotionForExe(target,&error)==Sm::SmResult::InvalidTarget,"invalid target accepted");
            Check(error==Sm::kInvalidArgument,"invalid target error missing");
        }
    });
    Case("disabled and absent settings do not write",[]{
        for(bool absent:{false,true}) {FakeDrs fake;fake.Seed(kUnrelated,88);if(!absent)fake.Seed(Sm::kSmEnableId,0);
            int error;Check(Run(fake,error)==Sm::SmResult::Ok,"no-op failed");NoWrites(fake);
            Check(fake.getCalls[Sm::kSmEnableApisId]==0,"disabled cleanup read or touched API restriction");}
    });
    Case("missing profile never creates or saves",[]{
        for(int missing:{Sm::kExecutableNotFound,Sm::kProfileNotFound}) {FakeDrs fake;fake.expectedQueries={kTarget,L"sample.exe"};fake.findResults={missing,missing};
            int error;Check(Run(fake,error)==Sm::SmResult::Ok,"missing profile not treated as no-op");NoWrites(fake);Check(fake.queries.size()==2,"missing profile lookup count");}
    });
    Case("full path errors never fall back to a namesake",[]{
        FakeDrs fake;fake.findResults={kFailure};int error;
        Check(Run(fake,error)==Sm::SmResult::ReadFailed && error==kFailure,"full path error concealed");
        NoWrites(fake);Check(fake.queries.size()==1,"unexpected filename fallback after an API error");
    });
    Case("full path miss falls back to the executable name",[]{
        for(int missing:{Sm::kExecutableNotFound,Sm::kProfileNotFound}) {FakeDrs fake;UserOn(fake);fake.expectedQueries={kTarget,L"sample.exe"};fake.findResults={missing,Sm::kOk};
            int error;Check(Run(fake,error)==Sm::SmResult::Ok,"filename fallback failed");fake.CheckUntouched(1);Check(fake.queries.size()==2 && fake.Effective(Sm::kSmEnableId)==0,"fallback target not cleaned");}
    });
    Case("filename fallback errors propagate without writes",[]{
        FakeDrs fake;fake.expectedQueries={kTarget,L"sample.exe"};fake.findResults={Sm::kExecutableNotFound,kFailure};int error;
        Check(Run(fake,error)==Sm::SmResult::ReadFailed && error==kFailure,"fallback error concealed");NoWrites(fake);
    });
    Case("null successful profile handle is rejected",[]{
        FakeDrs fake;fake.nullProfile=true;int error;
        Check(Run(fake,error)==Sm::SmResult::ReadFailed && error==Sm::kInvalidArgument,"null profile accepted");NoWrites(fake);
    });
    Case("on overrides delete both settings and save once",[]{
        FakeDrs fake;UserOn(fake);int error;
        Check(Run(fake,error)==Sm::SmResult::Ok,"override cleanup failed");fake.CheckUntouched(1);
        Check(fake.deleted==std::vector<unsigned>{Sm::kSmEnableId,Sm::kSmEnableApisId},"wrong delete list");
        Check(fake.written.empty(),"unnecessary override written");
        Check(!fake.persisted.count(Sm::kSmEnableId) && !fake.persisted.count(Sm::kSmEnableApisId),"overrides persisted");
        Check(fake.Effective(Sm::kSmEnableApisId)==2,"legacy API 7 did not restore driver API 2 restriction");
    });
    Case("missing API setting still removes enable override",[]{
        FakeDrs fake;fake.Seed(Sm::kSmEnableId,1);fake.Seed(kUnrelated,88);int error;
        Check(Run(fake,error)==Sm::SmResult::Ok,"missing API setting rejected");fake.CheckUntouched(1);
        Check(fake.deleted==std::vector<unsigned>{Sm::kSmEnableId} && fake.written.empty(),"wrong operations for absent API setting");
    });
    Case("driver reads ON but public delete reports setting not found",[]{
        for(bool withApiOverride:{false,true}) {
            FakeDrs fake;fake.Seed(Sm::kSmEnableId,1);fake.Seed(kUnrelated,88);
            if(withApiOverride)fake.Seed(Sm::kSmEnableApisId,7);
            fake.publicDeleteCannotSeeSmoothMotion=true;int error;
            Check(Run(fake,error)==Sm::SmResult::Ok && error==Sm::kOk,"driver delete mismatch blocked cleanup");
            fake.CheckUntouched(1);
            Check(fake.persisted.at(Sm::kSmEnableId).current.u32Value==0,"delete miss left Smooth Motion on");
            Check(fake.written==std::vector<unsigned>{Sm::kSmEnableId},"delete fallback guessed an API default");
        }
    });
    Case("delete not-found fallback still requires a verified OFF write",[]{
        for(bool ineffective:{false,true}) {
            FakeDrs fake;fake.Seed(Sm::kSmEnableId,1);fake.publicDeleteCannotSeeSmoothMotion=true;
            if(ineffective)fake.setDoesNothing=true;else fake.setError=kFailure;
            int error;Check(Run(fake,error)==Sm::SmResult::WriteFailed,"failed fallback treated as success");
            fake.CheckUntouched();
            Check(fake.persisted.at(Sm::kSmEnableId).current.u32Value==1,"failed fallback committed settings");
        }
    });
    Case("inherited and predefined on are disabled per application",[]{
        for(unsigned location:{0u,1u,2u}) {FakeDrs fake;fake.Seed(kUnrelated,88);
            fake.defaults[Sm::kSmEnableId]=Setting(Sm::kSmEnableId,1,location,location==0);
            fake.defaults[Sm::kSmEnableApisId]=Setting(Sm::kSmEnableApisId,4,location,location==0);int error;
            Check(Run(fake,error)==Sm::SmResult::Ok,"inherited enable cleanup failed");fake.CheckUntouched(1);
            Check(fake.deleted.empty() && fake.written==std::vector<unsigned>{Sm::kSmEnableId},"inherited/global setting was deleted");
            Check(fake.defaults.at(Sm::kSmEnableId).current.u32Value==1 && fake.defaults.at(Sm::kSmEnableApisId).current.u32Value==4,"global/predefined settings changed");
            Check(fake.persisted.at(Sm::kSmEnableId).current.u32Value==0,"per-app off not persisted");}
    });
    Case("deleting overrides cannot re-enable inherited Smooth Motion",[]{
        FakeDrs fake;UserOn(fake);fake.defaults[Sm::kSmEnableId]=Setting(Sm::kSmEnableId,1,1);int error;
        Check(Run(fake,error)==Sm::SmResult::Ok,"inherited restoration failed");fake.CheckUntouched(1);
        Check(fake.deleted.size()==2 && fake.written.size()==1 && fake.Effective(Sm::kSmEnableId)==0,"inherited on left enabled");
        Check(fake.Effective(Sm::kSmEnableApisId)==2,"API restriction not restored");
    });
    Case("initial and API reads fail without mutation or save",[]{
        for(unsigned id:{Sm::kSmEnableId,Sm::kSmEnableApisId}) {FakeDrs fake;UserOn(fake);fake.failGetId=id;fake.failGetCall=1;int error;
            Check(Run(fake,error)==Sm::SmResult::ReadFailed && error==kFailure,"read failure concealed");NoWrites(fake);}
    });
    Case("non-DWORD driver setting is rejected without writes",[]{
        FakeDrs fake;UserOn(fake);fake.profile[Sm::kSmEnableId].settingType=1;int error;
        Check(Run(fake,error)==Sm::SmResult::ReadFailed && error==Sm::kInvalidArgument,"invalid type accepted");NoWrites(fake);
    });
    Case("each delete failure prevents saving any partial cleanup",[]{
        for(unsigned id:{Sm::kSmEnableId,Sm::kSmEnableApisId}) {FakeDrs fake;UserOn(fake);fake.failDeleteId=id;int error;
            Check(Run(fake,error)==Sm::SmResult::WriteFailed && error==kFailure,"delete failure concealed");fake.CheckUntouched();
            Check(fake.persisted.at(Sm::kSmEnableId).current.u32Value==1 && fake.persisted.at(Sm::kSmEnableApisId).current.u32Value==7,"partial delete was committed");}
    });
    Case("read after delete failure prevents saving partial cleanup",[]{
        FakeDrs fake;UserOn(fake);fake.failGetId=Sm::kSmEnableId;fake.failGetCall=2;int error;
        Check(Run(fake,error)==Sm::SmResult::ReadFailed && error==kFailure,"post-delete failure concealed");fake.CheckUntouched();
        Check(fake.persisted.at(Sm::kSmEnableId).current.u32Value==1,"delete committed despite failed verification");
    });
    Case("per-application disable failure prevents save",[]{
        FakeDrs fake;fake.defaults[Sm::kSmEnableId]=Setting(Sm::kSmEnableId,1,1);fake.setError=kFailure;int error;
        Check(Run(fake,error)==Sm::SmResult::WriteFailed && error==kFailure,"write failure concealed");fake.CheckUntouched();Check(fake.persisted.empty(),"failed write persisted");
    });
    Case("ineffective per-application disable cannot report success",[]{
        FakeDrs fake;fake.defaults[Sm::kSmEnableId]=Setting(Sm::kSmEnableId,1,1);fake.setDoesNothing=true;int error;
        Check(Run(fake,error)==Sm::SmResult::WriteFailed && error==Sm::kInvalidArgument,"ineffective write accepted");fake.CheckUntouched();
    });
    Case("verification read after write failure prevents save",[]{
        FakeDrs fake;fake.defaults[Sm::kSmEnableId]=Setting(Sm::kSmEnableId,1,1);fake.failGetId=Sm::kSmEnableId;fake.failGetCall=3;int error;
        Check(Run(fake,error)==Sm::SmResult::WriteFailed && error==kFailure,"post-write read failure concealed");fake.CheckUntouched();Check(fake.persisted.empty(),"failed verification persisted");
    });
    Case("save failure is reported and leaves durable settings unchanged",[]{
        FakeDrs fake;UserOn(fake);fake.saveError=kFailure;int error;
        Check(Run(fake,error)==Sm::SmResult::WriteFailed && error==kFailure,"save failure concealed");fake.CheckUntouched(1);
        Check(fake.persisted.at(Sm::kSmEnableId).current.u32Value==1 && fake.persisted.at(Sm::kSmEnableApisId).current.u32Value==7,"failed save changed durable settings");
    });
    Case("missing API entry points fail before any mutation",[]{
        for(unsigned missing=0;missing<4;++missing) {FakeDrs fake;UserOn(fake);Sm::NvApi api;Bind(api);int error=0;
            if(missing==0)api.getSetting=nullptr;if(missing==1)api.setSetting=nullptr;if(missing==2)api.deleteProfileSetting=nullptr;if(missing==3)api.saveSettings=nullptr;
            const auto result=Sm::CleanupEnabledInSession(api,&fake,kTarget,&error);
            Check(result==(missing==0?Sm::SmResult::ReadFailed:Sm::SmResult::NvApiUnavailable) && error==Sm::kUnavailable,"missing entry point misreported");NoWrites(fake);}
    });
    Case("disabled settings require no write API entry points",[]{
        FakeDrs fake;fake.Seed(Sm::kSmEnableId,0);Sm::NvApi api;Bind(api);
        api.deleteProfileSetting=nullptr;api.setSetting=nullptr;api.saveSettings=nullptr;int error=0;
        Check(Sm::CleanupEnabledInSession(api,&fake,kTarget,&error)==Sm::SmResult::Ok,"disabled setting requires write capability");NoWrites(fake);
    });
    Check(GetModuleHandleW(L"nvapi64.dll")==driverBefore,"test loaded the real NVIDIA driver library");
    std::printf("PASS Smooth Motion cleanup: %u isolated cases; no real NVAPI initialization, settings or saves\n",cases);
    return 0;
} catch(const std::exception& error) {std::printf("FAIL Smooth Motion cleanup: %s\n",error.what());return 1;}
