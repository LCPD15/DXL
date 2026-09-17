#include "../src/core/ForcedNrOutput.h"
#include "../src/common/SettingsReader.h"
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <cstdio>
static void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main() try {
    using namespace DXL;
    Check(ForcedNrAdmission(false,false,true,false,false)==ForcedNrBlock::Off,"off dominates admission");
    Check(ForcedNrAdmission(true,false,false,true,true)==ForcedNrBlock::Busy,"in-flight Evaluate work must retire");
    Check(ForcedNrAdmission(true,true,true,true,true)==ForcedNrBlock::Paused,"resize pause retained");
    Check(ForcedNrAdmission(true,true,false,false,true)==ForcedNrBlock::WaitingOutput,"wrapper is not final output");
    Check(ForcedNrAdmission(true,true,false,true,false)==ForcedNrBlock::WaitingOutput,"unverified queue is not accepted");
    Check(ForcedNrAdmission(true,true,false,true,true)==ForcedNrBlock::Running,"verified final output admitted");
    NrSettings saved; saved.enabled=true; saved.useRealDepth=saved.useRealMotion=true; saved.opticalFlow=true;
    saved.grading.exposure={true,.5f}; saved.grading.bloom={true,2}; saved.trueLayers=2; saved.renderScale=.75f;
    auto effective=ForcedNrSettings(saved,true);
    Check(!effective.useRealDepth&&!effective.useRealMotion&&effective.forceReset&&effective.opticalFlow,"no stale guides on handoff");
    Check(effective.grading.exposure.enabled&&!effective.grading.bloom.enabled&&effective.trueLayers==2&&effective.renderScale==.75f,"NR tuning and basic grading preserved; final filters separate");
    Check(saved.useRealDepth&&saved.useRealMotion&&saved.grading.bloom.enabled,"saved automatic-route preferences unchanged");
    Check(!ForcedNrSettings(saved,false).forceReset,"stable optical frames retain history");
    saved.opticalFlow=false; Check(ForcedNrSettings(saved,false).forceReset,"no-motion fallback resets history");
    const auto root=std::filesystem::temp_directory_path()/(L"DXL-forced-settings-"+std::to_wstring(GetCurrentProcessId()));
    Check(!std::filesystem::exists(root),"fixture directory must be new");
    std::filesystem::create_directory(root);
    struct Cleanup { std::filesystem::path path; ~Cleanup(){std::error_code ec;std::filesystem::remove_all(path,ec);} } cleanup{root};
    std::ofstream(root/L"a.json")<<"{\"nrForceCompatibility\":false,\"dlss5AtEvaluate\":true,\"nrAutoRoute\":false}";
    std::ofstream(root/L"a.params.json")<<"{\"nrForceCompatibility\":true}";
    std::ofstream(root/L"b.json")<<"{}";
    SettingsReader a,b; Check(a.Load(root/L"a.json")&&a.LoadParamsOverlay(root/L"a.params.json")&&b.Load(root/L"b.json"),"read profile layers");
    Check(a.GetBool("nrForceCompatibility",false)&&!b.GetBool("nrForceCompatibility",false),"per-game overlay saved; other/old profiles default off");
    Check(a.GetBool("dlss5AtEvaluate",false)&&!a.GetBool("nrAutoRoute",true),"forced setting does not rewrite normal routing");
    std::ofstream(root/L"a.params.json")<<"{\"nrForceCompatibility\":false}";
    SettingsReader restored;
    Check(restored.Load(root/L"a.json")&&restored.LoadParamsOverlay(root/L"a.params.json")&&!restored.GetBool("nrForceCompatibility",true),"saved off restores normal mode");
    puts("PASS forced NR: queue/resize/drain guards, native-guide isolation, reset/history, profile isolation and mode restoration intent");
    return 0;
} catch(const std::exception& e) { printf("FAIL %s\n",e.what()); return 1; }
