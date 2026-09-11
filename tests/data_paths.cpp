#include "../src/common/DataPaths.h"
#include <cstdio>
#include <stdexcept>
using namespace DXL::DataPaths;
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Put(const fs::path& p, const char* text) { fs::create_directories(p.parent_path()); std::ofstream(p) << text; }
std::string Read(const fs::path& p) { std::ifstream f(p); return {std::istreambuf_iterator<char>(f), {}}; }
int main() {
    // Use OS temporary storage even when invoked from the source directory.
    const auto base = fs::temp_directory_path() / ("dxl-data-path-tests-" + std::to_string(GetCurrentProcessId()));
    fs::create_directories(base);
    const auto local = base / "local";
    const auto legacy = local / "DLSS5-Quick";
    Put(legacy / "settings.json", "{\"theme\":\"dark\"}");
    Put(legacy / "launcher.json", "{\"adminLaunch\":true}");
    Put(legacy / "profiles/game.exe.params.json", "{\"nrSelfLayers\":1.5}");
    Put(legacy / "secret.log", "not configuration");
    Put(legacy / "profiles/not-config.dll", "not a profile");
    const auto target = ResolveConfigRoot(local, base / "empty");
    Check(target == local / "DXL", "new directory");
    Check(Read(target / "settings.json") == Read(legacy / "settings.json"), "legacy settings migrated");
    Check(RegularFile(target / "profiles/game.exe.params.json"), "game overlay parameters migrated");
    Check(RegularFile(legacy / "settings.json"), "source preserved");
    Check(!fs::exists(target / "secret.log") && !fs::exists(target / "profiles/not-config.dll"), "data allowlist");
    Put(target / "settings.json", "new user settings");
    ResolveConfigRoot(local, legacy);
    Check(Read(target / "settings.json") == "new user settings", "new settings not overwritten");
    const auto portable = base / "portable";
    const auto local2 = base / "local2";
    Put(portable / "settings.json", "portable");
    Put(local2 / "DLSS5-Quick/settings.json", "old appdata");
    Check(Read(ResolveConfigRoot(local2, portable) / "settings.json") == "portable", "portable migration priority");
    Check(Read(portable / "settings.json") == "portable", "portable source preserved");
    const auto local3 = base / "local3";
    Put(local3 / "DXL/launcher.json", "existing preference");
    ImportSettings(portable, local3 / "DXL");
    Check(Read(local3 / "DXL/launcher.json") == "existing preference", "partial import does not overwrite");
    Check(ResolveConfigRoot(base / "fresh", {}) == base / "fresh/DXL", "new installation");
    puts("PASS DXL AppData migration, portable import, non-overwrite and data allowlist");
}
