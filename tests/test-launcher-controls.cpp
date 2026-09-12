#include "../src/ui/InjectionCoordination.h"
#include "../src/ui/Hotkey.h"
#include "../src/ui/GameListCleanup.h"
#include "../src/ui/LaunchArguments.h"
#include "../src/ui/ProfileCommandRouting.h"
#include "../src/ui/ProfileDeletion.h"
#include "../src/common/NrParameterEdit.h"
#include <limits>
#include <shellapi.h>
#include <cstdio>
#include <stdexcept>
#pragma comment(lib, "shell32.lib")

static void Check(bool pass, const char* message) {
    if (!pass) throw std::runtime_error(message);
}
int wmain() {
    using namespace DXL;
    const std::string deletionModel = R"({"profiles":[{"id":"default","exePath":"X:\\default.exe"},{"settings":{"id":"spoof","exePath":"X:\\nested.exe","value":1.2e-3},"id":"game","exePath":"X:\\Games\\Game.exe"},{"id":"empty"}],"note":"\"profiles\":[]"})";
    std::string deletionExe;
    Check(ProfileDeletion::ResolveExePath(deletionModel,"game",deletionExe) && deletionExe=="X:\\Games\\Game.exe", "driver cleanup resolves exact saved profile path");
    Check(ProfileDeletion::ResolveExePath(deletionModel,"empty",deletionExe) && deletionExe.empty(), "legacy empty path may skip driver cleanup");
    for (const auto* id : {"", "default", "missing", "spoof"})
        Check(!ProfileDeletion::ResolveExePath(deletionModel,id,deletionExe), "default/unmatched/nested identity cannot target driver cleanup");
    for (const auto* malformed : {
        R"({"profiles":[{"id":"game","exePath":"X:\\one.exe"},{"id":"game","exePath":"X:\\two.exe"}]})",
        R"({"profiles":[{"id":"game","id":"other","exePath":"X:\\one.exe"}]})",
        R"({"profiles":[{"id":"game","exePath":"X:\\one.exe","exePath":"X:\\two.exe"}]})",
        R"({"profiles":[{"id":"game","exePath":null}]})",
        R"({"profiles":[{"id":"game","exePath":"X:\\one\u0000.exe"}]})",
        R"({"profiles":[{"id":"game"}],"profiles":[]})",
        R"({"profiles":[{"id":"game"},]})",
        R"({"profiles":[{"id":"game"}] trailing})"
    }) Check(!ProfileDeletion::ResolveExePath(malformed,"game",deletionExe), "ambiguous/malformed target rejected");
    ProfileDeletion::SavedRequest deleteSave;
    deleteSave.Record(1,true,deletionModel);
    Check(!deleteSave.Take(2,"game",deletionExe), "unmatched save sequence rejected");
    Check(deleteSave.Take(1,"game",deletionExe) && deletionExe=="X:\\Games\\Game.exe", "matching successful save authorizes exact snapshot once");
    Check(!deleteSave.Take(1,"game",deletionExe), "save token cannot be replayed");
    deleteSave.Record(2,false,deletionModel);
    Check(!deleteSave.Take(2,"game",deletionExe), "failed model save cannot clean old driver target");
    deleteSave.Record(3,true,deletionModel);
    Check(!deleteSave.Take(3,"default",deletionExe), "default profile can never authorize cleanup");
    std::puts("PASS deletion prerequisites: saved target resolution, malformed/ambiguous input rejection, default exclusion and failed-save/replay isolation (no driver calls)");
    struct ProfileTarget { DWORD pid; std::wstring name; };
    const std::vector<ProfileTarget> targets{{11,L"A.exe"},{22,L"B.exe"},{33,L"a.EXE"}};
    std::vector<DWORD> commanded;
    auto result = DispatchProfileCommand(targets, L"a.exe.json", [&](DWORD pid) {
        commanded.push_back(pid); return pid != 33;
    });
    Check(commanded == std::vector<DWORD>{11,33}, "profile command cannot target another game");
    Check(result.matched == 2 && result.succeeded == 1, "all matching instances attempted, failure preserved");
    for (auto file : {L"default.json",L"missing.exe.json",L"A.exe.json.backup"}) {
        commanded.clear();
        result = DispatchProfileCommand(targets, file, [&](DWORD pid) { commanded.push_back(pid); return true; });
        Check(commanded.empty() && !result.matched, "unmatched/default profile cannot control selected game");
    }
    std::puts("PASS profile-directed control: case-insensitive game identity, multiple instances, offline/default isolation and failure accounting");
    for (const auto& spec : NrEditSpecs) {
        for (double value : {spec.low, spec.high}) {
            uint32_t packed = 0; unsigned index = 0; double decoded = 0;
            Check(EncodeNrEdit(spec.key,value,packed) && DecodeNrEdit(packed,index,decoded), "NR edit roundtrip");
            Check(NrEditSpecs[index].key == spec.key && std::abs(decoded-value) < 0.001, "NR edit identity/value");
        }
        uint32_t packed = 0;
        Check(!EncodeNrEdit(spec.key,spec.high+1,packed), "NR upper bound");
        Check(!EncodeNrEdit(spec.key,spec.low-1,packed), "NR lower bound");
    }
    uint32_t encoded = 0; unsigned decodedIndex = 0; double decodedValue = 0;
    Check(!EncodeNrEdit("notAParameter",1,encoded), "unknown NR key");
    Check(!EncodeNrEdit("nrIntensity",std::numeric_limits<double>::quiet_NaN(),encoded), "NaN NR value");
    Check(!EncodeNrEdit("nrTrueLayers",1.5,encoded), "fractional layer count");
    Check(!DecodeNrEdit(0xffffffff,decodedIndex,decodedValue), "invalid packed NR edit");
    std::puts("PASS NR parameter protocol: all bounds, identity, roundtrip and malformed input rejection");

    UINT mods = 0, key = 0;
    Check(ParseHotkey(L"Del", mods, key) && !mods && key == VK_DELETE, "bare Del");
    Check(ParseHotkey(L"End", mods, key) && !mods && key == VK_END, "bare End");
    Check(ParseHotkey(L"Ctrl + Shift + Delete", mods, key) &&
        mods == (MOD_CONTROL | MOD_SHIFT) && key == VK_DELETE, "modified Delete");
    Check(ParseHotkey(L"Win + F24", mods, key) && mods == MOD_WIN && key == VK_F24, "Win F24");
    Check(ParseHotkey(L"J", mods, key) && !mods && key == 'J', "bare letter");
    for (const auto* invalid : { L"", L"Ctrl", L"F25", L"F1junk", L"Alt + Del + End", L"Unknown", L"Del +" })
        Check(!ParseHotkey(invalid, mods, key), "invalid shortcut rejected");
    Check(!IsMissingPathError(ERROR_ACCESS_DENIED), "access denied is not missing");
    Check(!IsMissingPathError(ERROR_NOT_READY), "offline disk is not missing");
    Check(!IsMissingPathError(ERROR_BAD_NETPATH), "offline network is not missing");
    Check(InspectGameExecutable(L"") == GamePathState::Unknown, "empty path protected");
    Check(InspectGameExecutable(L"relative.exe") == GamePathState::Unknown, "relative path protected");
    wchar_t temp[MAX_PATH]{};
    Check(GetTempPathW(MAX_PATH, temp) != 0, "temp path");
    const auto fixture = std::filesystem::path(temp) / (L"DXL-cleanup-test-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    Check(CreateDirectoryW(fixture.c_str(), nullptr) != 0, "fixture directory");
    const auto executable = fixture / L"game.exe";
    HANDLE file = CreateFileW(executable.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file != INVALID_HANDLE_VALUE, "fixture file");
    CloseHandle(file);
    Check(InspectGameExecutable(executable.wstring()) == GamePathState::Exists, "existing exe kept");
    Check(DeleteFileW(executable.c_str()) != 0, "remove fixture file");
    Check(InspectGameExecutable(executable.wstring()) == GamePathState::Missing, "missing exe on ready volume");
    Check(RemoveDirectoryW(fixture.c_str()) != 0, "remove fixture directory");
    Check(InspectGameExecutable(executable.wstring()) == GamePathState::Missing, "uninstalled directory on ready volume");
    InjectionCoordination coordination;
    Check(coordination.QueueWatch(10, L"Game.exe", false, false, 100), "watch notification queued before launch");
    Check(coordination.BeginLaunch(L"GAME.exe"), "launch reserves game name");
    Check(!coordination.BeginLaunch(L"game.EXE"), "duplicate launch rejected case-insensitively");
    Check(!coordination.BeginWatch(10, L"Game.exe", false, false, 200), "queued watch cannot steal a pending launch");
    Check(!coordination.QueueWatch(11, L"game.exe", false, false, 300), "replacement process also belongs to launch");
    Check(coordination.QueueWatch(20, L"Other.exe", false, false, 300), "other games still monitored");
    Check(coordination.BeginWatch(20, L"Other.exe", false, false, 300), "other game can inject");
    Check(!coordination.QueueWatch(20, L"Other.exe", false, false, 400), "no repeated injection");
    coordination.EndLaunch(L"game.exe");
    Check(!coordination.QueueWatch(11, L"game.exe", true, false, 500), "late mode ignores launcher with no window");
    Check(!coordination.QueueWatch(11, L"game.exe", true, true, 1000), "late mode starts window timer");
    Check(!coordination.QueueWatch(11, L"game.exe", true, true, 2999), "late mode waits full settle time");
    Check(!coordination.QueueWatch(11, L"game.exe", true, false, 3000), "lost window resets settle time");
    Check(!coordination.QueueWatch(11, L"game.exe", true, true, 4000), "replacement window gets fresh timer");
    Check(coordination.QueueWatch(11, L"game.exe", true, true, 6000), "settled late game queues once");
    Check(!coordination.QueueWatch(11, L"game.exe", true, true, 6100), "no duplicate queued message");
    Check(coordination.BeginWatch(11, L"game.exe", true, true, 6200), "settled late game injects");
    Check(coordination.QueueWatch(30, L"Changed.exe", false, false, 7000), "auto-mode notification queued");
    Check(!coordination.BeginWatch(30, L"Changed.exe", true, true, 7100), "switching to late before dispatch cannot bypass timer");
    coordination.Prune({11});
    Check(coordination.QueueWatch(20, L"Other.exe", false, false, 8000), "exited PID can be reused safely");
    coordination.CancelWatch(20);
    Check(coordination.QueueWatch(20, L"Other.exe", false, false, 8100), "failed post is retryable");
    Check(coordination.BeginLaunch(L"finished.exe"), "second pending launch");
    Check(!coordination.QueueWatch(40, L"finished.exe", false, false, 9000), "pending startup shell observed");
    coordination.FinishLaunch(L"finished.exe", 41);
    Check(!coordination.QueueWatch(40, L"finished.exe", false, false, 9100), "finished launch shell is not reinjected");
    Check(!coordination.QueueWatch(41, L"finished.exe", false, false, 9100), "connected target is not reclassified as manual attachment");
    Check(coordination.BeginLaunch(L"finished.exe"), "completed reservation released");
    coordination.EndLaunch(L"finished.exe");
    Check(!coordination.NeedsWatch(41, L"finished.exe"), "handled process needs no window/module polling");
    Check(coordination.NeedsWatch(50, L"early.exe"), "new early target needs sampling");
    Check(!coordination.ObserveLoader(50, 0, 100), "unreadable loader is not ready");
    Check(!coordination.ObserveLoader(50, 20, 200), "first readable loader sample starts timer");
    Check(!coordination.ObserveLoader(50, 20, 899), "full loader quiet period required");
    Check(coordination.ObserveLoader(50, 20, 900), "stable loader becomes eligible");
    Check(coordination.QueueWatch(50, L"early.exe", false, false, 900), "early notification queued after quiet sample");
    Check(!coordination.NeedsWatch(50, L"early.exe"), "queued process is not repeatedly sampled by worker");
    Check(!coordination.ObserveLoader(50, 21, 901), "UI recheck detects loader resumed after notification");
    coordination.CancelWatch(50);
    Check(coordination.NeedsWatch(50, L"early.exe"), "canceled dispatch can be resampled");
    Check(!coordination.ObserveLoader(50, 0, 1601), "failed recheck never counts as stable");
    Check(!coordination.ObserveLoader(50, 21, 1700), "fresh sample after failure restarts timer");
    Check(coordination.ObserveLoader(50, 21, 2400), "recovered stable loader is ready");
    Check(coordination.BeginLaunch(L"early.exe"), "launch can reserve target during sampling");
    Check(!coordination.NeedsWatch(50, L"early.exe"), "pending launch stops loader polling");
    coordination.EndLaunch(L"early.exe");
    std::puts("PASS nonblocking loader sampling, dispatch recheck and zero polling after attachment");
    std::puts("PASS launch ownership, queued-message race, late window settling, replacement processes and PID reuse");

    std::puts("PASS bare/modified keys, malformed key rejection, cleanup existence and unavailable-path protections");

    const std::string launchJson = R"json({"type":"launch","payload":null,"exePath":"C:\\游戏 文件\\Game.exe","args":"-force-d3d12 -name \"two words\" -path \"C:\\Save Data\\slot\"","timing":"early"})json";
    Check(LaunchArguments::ReadField(launchJson, "exePath") == "C:\\游戏 文件\\Game.exe", "launch path JSON escapes decoded");
    Check(LaunchArguments::ReadField(launchJson, "args") == "-force-d3d12 -name \"two words\" -path \"C:\\Save Data\\slot\"", "quoted argument tail preserved");
    Check(LaunchArguments::ReadField(R"({"args":"\u6e38\u620f \ud83d\udcbe"})", "args") == "游戏 💾", "Unicode escapes and surrogate pairs");
    Check(LaunchArguments::ReadField(R"({"other":"\"args\":\"wrong\"","args":"right"})", "args") == "right", "value text cannot impersonate field");
    Check(LaunchArguments::ReadField(R"({"args":"\u0000"})", "args").empty(), "embedded NUL rejected");
    Check(LaunchArguments::ReadField(R"({"args":"\ud800"})", "args").empty(), "unpaired surrogate rejected");
    const std::wstring gamePath = L"C:\\游戏 文件\\Game.exe";
    const std::wstring args = LR"(-force-d3d12 -name "two words" -path "C:\Save Data\slot" -empty "")";
    const auto command = LaunchArguments::CommandLine(gamePath, args);
    int argc = 0;
    auto argv = CommandLineToArgvW(command.c_str(), &argc);
    Check(argv && argc == 8, "Windows command line argument count");
    Check(argv[0] == gamePath && std::wstring(argv[1]) == L"-force-d3d12" &&
        std::wstring(argv[3]) == L"two words" && std::wstring(argv[5]) == L"C:\\Save Data\\slot" &&
        std::wstring(argv[7]).empty(), "Windows parses executable, quoted values, paths and empty argument");
    LocalFree(argv);
    Check(LaunchArguments::CommandLine(gamePath, L"") == L"\"" + gamePath + L"\"", "empty launch tail");
    std::puts("PASS launch JSON decoding and Windows command-line parsing: Unicode, spaces, quoted values, paths and empty arguments");
}
