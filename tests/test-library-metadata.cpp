#include "../src/ui/LibraryMetadata.h"
#include <fstream>
#include <cstdio>
#include <stdexcept>
namespace fs = std::filesystem;
namespace md = DXL::LibraryMetadata;
static void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static void Write(const fs::path& path, const std::string& contents) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary); file.write(contents.data(), contents.size());
}
int wmain() try {
    Check(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "COM initialization");
    const auto root = fs::temp_directory_path() / (L"DXL.MetadataTest." + std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(root);
    struct Cleanup { fs::path root; ~Cleanup() { std::error_code ec; fs::remove_all(root, ec); CoUninitialize(); } } cleanup{root};
    const auto common = root / L"steamapps/common";
    fs::create_directories(common / L"Game");
    fs::create_directories(common / L"Game Two");
    Write(root / L"steamapps/appmanifest_11.acf", "\"appid\" \"11\"\n\"installdir\" \"Game\"\n");
    Write(root / L"steamapps/appmanifest_111.acf", "\"appid\" \"111\"\n\"installdir\" \"Game Two\"\n");
    Write(root / L"steamapps/appmanifest_44.acf", "\"appid\" \"44\"\n\"installdir\" \"../..\"\n");
    Write(root / L"steamapps/appmanifest_55.acf", "\"appid\" \"../55\"\n\"installdir\" \"Game\"\n");
    std::vector<md::Install> installs;
    md::ReadSteamRoots({root.wstring()}, installs);
    Check(installs.size() == 2, "manifest traversal/invalid app id rejected");
    const auto* matched = md::Match(common / L"game/Binaries/Win64/game.exe", installs);
    Check(matched && matched->steamAppId == L"11", "case-insensitive nested executable match");
    matched = md::Match(common / L"Game Two/Game.exe", installs);
    Check(matched && matched->steamAppId == L"111", "similarly named games must not cross-match");
    Check(!md::Match(common / L"Game Backup/Game.exe", installs), "directory boundary enforced");
    Check(!md::Match(common / L"Game/../../outside.exe", installs), "normalized traversal rejected");
    Check(!md::Within(L"C:\\Game.exe", L"C:\\"), "whole volume is not a game installation");
    Check(!md::Match(L"relative/Game.exe", installs), "relative profiles rejected");
    installs.push_back({common / L"Game/Subgame", L"Epic", {}});
    matched = md::Match(common / L"Game/Subgame/a.exe", installs);
    Check(matched && matched->source == L"Epic", "most specific installation wins");
    const auto escaped = std::string("{\"AppCategories\":[\"games\"],\"InstallLocation\":\"") +
        "C:\\\\Games\\\\\\u6e38\\u620f\"}";
    Check(DXL::LaunchArguments::ReadField(escaped, "InstallLocation") == "C:\\Games\\游戏",
        "JSON path escape decoding preserves platform directories");
    const auto epicRoot = root / L"Epic Game";
    fs::create_directories(epicRoot);
    const auto utf8 = epicRoot.u8string();
    std::string jsonPath;
    for (const char8_t c : utf8) {
        if (c == u8'\\' || c == u8'"') jsonPath += '\\';
        jsonPath += static_cast<char>(c);
    }
    Write(root / L"manifests/game.item", "{\"AppCategories\":[\"games\"],\"InstallLocation\":\"" + jsonPath + "\"}");
    Write(root / L"manifests/engine.item", "{\"AppCategories\":[\"engines\"],\"InstallLocation\":\"" + jsonPath + "\"}");
    std::vector<md::Install> epicInstalls;
    md::ReadEpicRoots(root / L"manifests", epicInstalls);
    Check(epicInstalls.size() == 1 && epicInstalls.front().source == L"Epic" &&
        md::Match(epicRoot / L"Binaries/Game.exe", epicInstalls), "Epic metadata path decoding and engine filtering");

    // A real BMP probes WIC validation and copying without external image tools.
    std::string bmp(58, '\0'); bmp[0] = 'B'; bmp[1] = 'M'; bmp[2] = 58;
    bmp[10] = 54; bmp[14] = 40; bmp[18] = 1; bmp[22] = 1; bmp[26] = 1;
    bmp[28] = 24; bmp[34] = 4; bmp[54] = 10; bmp[55] = 40; bmp[56] = 120;
    const auto image = root / L"appcache/librarycache/11/library_600x900.bmp";
    Write(image, bmp);
    Write(root / L"appcache/librarycache/111_library_600x900.bmp", bmp);
    Check(md::FindSteamCover({root.wstring()}, L"11") == image, "nested Steam cover and appid isolation");
    Check(md::FindSteamCover({root.wstring()}, L"111").filename() == L"111_library_600x900.bmp", "legacy cover layout");
    Check(md::FindSteamCover({root.wstring()}, L"../11").empty(), "cover lookup rejects unsafe appid");
    const auto cache = root / L"web/covercache";
    const auto url = md::CacheImage(image, cache, L"../../profile|bad/../name");
    Check(url.rfind(L"covercache/cover_", 0) == 0, "safe cache URL returned");
    Check(url.find(L"..") == std::wstring::npos && url.find(L'\\') == std::wstring::npos, "profile ID never becomes path");
    Check(fs::is_regular_file(root / L"web" / url), "cover copied inside web cache");
    Check(fs::file_size(image) == bmp.size(), "original artwork retained");
    Write(root / L"bad.jpg", "<html>not an image</html>");
    Check(md::CacheImage(root / L"bad.jpg", cache, L"bad").empty(), "disguised non-image rejected");
    Write(root / L"bad.svg", "<svg/>");
    Check(!md::ValidateImage(root / L"bad.svg"), "active image formats rejected");
    puts("PASS installed-root metadata isolation, JSON paths, both Steam cover layouts, WIC validation and safe cover copying");
    return 0;
} catch (const std::exception& e) { printf("FAIL %s\n", e.what()); return 1; }
