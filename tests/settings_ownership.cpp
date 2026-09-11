#include "SettingsReader.h"
#include <cstdio>
#include <stdexcept>
using namespace DXL;
static void Check(bool ok) { if (!ok) throw std::runtime_error("settings ownership"); }
static void Put(const std::filesystem::path& path, const char* text) {
    FILE* f = nullptr; Check(_wfopen_s(&f, path.c_str(), L"wb") == 0 && f);
    fputs(text, f); fclose(f);
}
int main() try {
    const auto root = std::filesystem::temp_directory_path() / (L"dxl-settings-ownership-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(root);
    Put(root/L"profile.json", "{\"dlss5Enable\":true,\"masterEnabled\":true,\"dlss5AtEvaluate\":true,\"nrAutoRoute\":true,\"diagEavesdrop\":true,\"nrIntensity\":1,\"nrSemInt17\":1}");
    Put(root/L"params.json", "{\"dlss5Enable\":false,\"masterEnabled\":false,\"dlss5AtEvaluate\":false,\"nrAutoRoute\":false,\"diagEavesdrop\":false,\"nrIntensity\":0.25,\"nrColourStrength\":0,\"nrSelfLayers\":1.375,\"nrTrueLayers\":5,\"nrOpticalFlow\":false,\"nrSemInt17\":0,\"nrSemOn\":262143,\"nrSemanticFlipY\":true,\"nrSemanticFeather\":3.5,\"hkUiMods\":1}");
    Put(root/L"diag.json", "{\"nrIntensity\":0.5}");
    SettingsReader s; Check(s.Load(root/L"profile.json"));
    Check(s.LoadDiagOverlay(root/L"diag.json")); Check(s.LoadParamsOverlay(root/L"params.json"));
    for (const auto key : {"dlss5Enable","masterEnabled","dlss5AtEvaluate","nrAutoRoute","diagEavesdrop"}) Check(s.GetBool(key,false));
    Check(s.GetFloat("nrIntensity",0) == 0.5f);
    Check(s.GetFloat("nrColourStrength",1) == 0);
    Check(s.GetFloat("nrSelfLayers",0) == 1.375f);
    Check(s.GetInt("nrTrueLayers",0) == 5);
    Check(!s.GetBool("nrOpticalFlow",true));
    Check(s.GetFloat("nrSemInt17",1) == 0);
    Check(s.GetInt("nrSemOn",0) == 262143);
    Check(s.GetBool("nrSemanticFlipY",false));
    Check(s.GetFloat("nrSemanticFeather",0)==3.5f);
    Check(s.GetInt("hkUiMods",0) == 1);
    Put(root/L"profile.json", "{\"hotkeyRevision\":1788900000001,\"hkEnable\":46,\"hkEnableMods\":0,\"hkUi\":35,\"hkUiMods\":0}");
    Put(root/L"params.json", "{\"hotkeyRevision\":1788900000000,\"hkEnable\":119,\"hkEnableMods\":2,\"hkUi\":48,\"hkUiMods\":1,\"nrSelfLayers\":1.75}");
    SettingsReader newerLauncher; Check(newerLauncher.Load(root/L"profile.json")); Check(newerLauncher.LoadParamsOverlay(root/L"params.json"));
    Check(newerLauncher.GetInt("hkEnable",0) == 46 && newerLauncher.GetInt("hkUi",0) == 35);
    Check(newerLauncher.GetInt("hkEnableMods",1) == 0 && newerLauncher.GetInt("hkUiMods",1) == 0);
    Check(newerLauncher.GetUInt64("hotkeyRevision",0) == 1788900000001ULL);
    Check(newerLauncher.GetFloat("nrSelfLayers",0) == 1.75f);
    Put(root/L"params.json", "{\"hotkeyRevision\":1788900000002,\"hkEnable\":120,\"hkEnableMods\":10,\"hkUi\":121,\"hkUiMods\":0}");
    SettingsReader newerGame; Check(newerGame.Load(root/L"profile.json")); Check(newerGame.LoadParamsOverlay(root/L"params.json"));
    Check(newerGame.GetInt("hkEnable",0) == 120 && newerGame.GetInt("hkEnableMods",0) == 10);
    Check(newerGame.GetInt("hkUi",0) == 121 && newerGame.GetInt("hkUiMods",1) == 0);
    Check(newerGame.GetUInt64("hotkeyRevision",0) == 1788900000002ULL);
    Check(newerGame.GetUInt64("missing",17) == 17);
    puts("PASS hotkey ownership: exact 64-bit millisecond revisions, newer launcher / newer game, zero modifiers, unrelated parameters preserved");
    puts("PASS settings: diag > tunables > profile; legacy params cannot disable route/master; exact zero/false/fractional/all-groups");
    return 0;
} catch (const std::exception& e) { fprintf(stderr,"FAIL %s\n",e.what()); return 1; }
