#include "SettingsReader.h"
#include "FgDetectionSettings.h"
#include <cstdio>
#include <limits>
#include <stdexcept>
using namespace DXL;
static void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static void Put(const std::filesystem::path& path, const char* text) {
    FILE* f = nullptr; Check(_wfopen_s(&f, path.c_str(), L"wb") == 0 && f, "write fixture");
    fputs(text, f); fclose(f);
}
static unsigned Read(const SettingsReader& reader, std::wstring_view exe) {
    const unsigned fallback = DefaultFgBufferThreshold(exe);
    return NormalizeFgBufferThreshold(reader.GetFloat("fgBufferCountThreshold", float(fallback)), fallback);
}
int main() try {
    const auto root = std::filesystem::current_path() / (L"game-compatibility-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(root);
    const auto file = root / L"profile.json";
    SettingsReader reader;
    Put(file, "{}"); Check(reader.Load(file), "load missing value");
    Check(Read(reader,L"G:\\游戏\\YYSLS.EXE") == 8, "exact uppercase game default");
    for (const auto name : {L"yysls_launcher.exe", L"my_yysls.exe", L"yysls.exe.backup", L"other.exe", L""})
        Check(Read(reader,name) == 4, "lookalike inherited game default");
    for (const auto value : {2,4,8,12,16}) {
        const std::string json = "{\"fgBufferCountThreshold\":" + std::to_string(value) + "}";
        Put(file,json.c_str()); Check(reader.Load(file), "load explicit value");
        Check(Read(reader,L"yysls.exe") == unsigned(value), "explicit custom value replaced");
        Check(Read(reader,L"other.exe") == unsigned(value), "ordinary game custom value replaced");
    }
    for (const char* invalid : {"-1", "0", "1", "17", "999", "2.5", "\"bad\"", "null", "1e999"}) {
        const std::string json = std::string("{\"fgBufferCountThreshold\":") + invalid + "}";
        Put(file,json.c_str()); Check(reader.Load(file), "load invalid value");
        Check(Read(reader,L"yysls.exe") == 8 && Read(reader,L"other.exe") == 4, "invalid fallback ignores game");
    }
    Check(NormalizeFgBufferThreshold(std::numeric_limits<double>::quiet_NaN(),8)==8,"NaN fallback");
    Put(file,"{\"fgBufferCountThreshold\":4}"); Check(reader.Load(file), "legacy flat four");
    Check(Read(reader,L"yysls.exe")==4,"unmarked legacy explicit four must be preserved");
    // New launcher flattened defaults omit the key, so it follows current
    // host identity even if this is profiles/default.json.
    Put(file,"{\"masterEnabled\":true}"); Check(reader.Load(file), "new flattened default");
    Check(Read(reader,L"yysls.exe")==8 && Read(reader,L"another.exe")==4,"flattened default resolution");
    Put(root/L"diag.json","{\"fgBufferCountThreshold\":6}");
    Check(reader.LoadDiagOverlay(root/L"diag.json"),"load diagnostic override");
    Check(Read(reader,L"yysls.exe")==6,"diagnostic custom value replaced");
    puts("PASS game FG defaults: exact executable/case/path, missing and invalid values, explicit 4/custom values, old flat preservation, new omitted defaults, diagnostic precedence");
    return 0;
} catch (const std::exception& e) { fprintf(stderr,"FAIL %s\n",e.what()); return 1; }
