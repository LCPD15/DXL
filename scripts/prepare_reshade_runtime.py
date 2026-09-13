"""Download immutable upstream sources and apply DXL's no-hook hosting patch."""
from __future__ import annotations
import concurrent.futures
import hashlib
import json
import pathlib
import shutil
import sys
import urllib.request
import zipfile

REVISION = '18deaa52de0c425a78b329e9cb3c497281cd00ec'
MODULES = {
    'd3d12': ('microsoft/DirectX-Headers', '9e393d6d8a3b30dcc6f2806ef604ec16a27b0d7e'),
    'fpng': ('richgel999/fpng', '925796543b9d26b8edfcdcecd94c1dac280f29fc'),
    'glad': ('Dav1dde/glad', '27bed1181560211b55e39a9b132fef8c5846aae5'),
    'imgui': ('ocornut/imgui', '3912b3d9a9c1b3f17431aebafd86d2f40ee6e59c'),
    'jxl_simple_lossless': ('kampidh/simple-lossless-encoder', '8dc970fc771e35239db55dfbce8f46f83f8e9b73'),
    'minhook': ('TsudaKageyu/minhook', '8fda4f5481fed5797dc2651cd91e238e9b3928c6'),
    'openxr': ('KhronosGroup/OpenXR-SDK', '288d3a7ebc1ad959f62d51da75baa3d27438c499'),
    'spirv': ('KhronosGroup/SPIRV-Headers', '7845730cab6ebbdeb621e7349b7dc1a59c3377be'),
    'stb': ('nothings/stb', '28d546d5eb77d4585506a20480f4de2e706dff4c'),
    'utfcpp': ('nemtrif/utfcpp', '819011bb01628fe1aa2f1da9f2c842a48fd5680b'),
    'vma': ('GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator', '1076b348abd17859a116f4b111c43d58a588a086'),
}

def download_extract(package, repository, revision, target):
    archive = root / f'{package}-{revision}.zip'
    url = f'https://codeload.github.com/{repository}/zip/{revision}'
    if not archive.exists():
        with urllib.request.urlopen(url, timeout=90) as response:
            archive.write_bytes(response.read())
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    expected = {item['package']: item['archive_sha256'] for item in
                json.loads((repo/'third_party/reshade/SOURCES.json').read_text(encoding='utf-8-sig'))}
    if digest != expected.get(package):
        raise ValueError(f'Pinned archive hash mismatch: {package}')
    with zipfile.ZipFile(archive) as z:
        for item in z.infolist():
            relative = pathlib.PurePosixPath(item.filename)
            if len(relative.parts) < 2 or item.is_dir():
                continue
            dest = target.joinpath(*relative.parts[1:]).resolve()
            if not dest.is_relative_to(target.resolve()):
                raise ValueError('Unsafe source archive path')
            dest.parent.mkdir(parents=True, exist_ok=True)
            content = z.read(item)
            if not dest.exists() or dest.read_bytes() != content:
                dest.write_bytes(content)
    return dict(package=package, repository=repository, revision=revision,
                archive_sha256=digest)

def replace(path, old, new):
    file = source / path
    content = file.read_text(encoding='utf-8')
    if old not in content:
        raise ValueError(f'Pinned upstream patch no longer matches: {path}')
    file.write_text(content.replace(old, new), encoding='utf-8', newline='\n')

if __name__ == '__main__':
    root = pathlib.Path(sys.argv[1]).resolve()
    repo = pathlib.Path(__file__).resolve().parents[1]
    if root == repo or root.is_relative_to(repo):
        raise ValueError('ReShade dependency output must be outside source repository')
    root.mkdir(parents=True, exist_ok=True)
    source = root / f'reshade-{REVISION}'
    records = [download_extract('reshade', 'crosire/reshade', REVISION, source)]
    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
        futures = [pool.submit(download_extract, name, repository, revision, source/'deps'/name)
                   for name, (repository, revision) in MODULES.items()]
        records += [future.result() for future in futures]
    # Standalone host owns lifecycle, input, and update policy. Compiler and
    # effect renderer remain byte-for-byte upstream sources.
    shutil.copyfile(repo/'scripts/reshade_private_entry.cpp', source/'source/dll_main.cpp')
    (source/'res/version.h').write_text('''#pragma once
#define VERSION_FULL 6.8.0.0
#define VERSION_MAJOR 6
#define VERSION_MINOR 8
#define VERSION_REVISION 0
#define VERSION_BUILD 0
#define VERSION_STRING_FILE "6.8.0.0"
#define VERSION_STRING_PRODUCT "6.8.0 DXL PRIVATE HOST"
''', encoding='utf-8')
    replace('source/runtime.cpp', '\tcheck_for_update();', '\t// DXL owns update checks; no runtime network request.')
    replace('source/runtime.cpp', 'if (window != nullptr && !_is_vr)', 'if (false && window != nullptr && !_is_vr)')
    replace('source/runtime.cpp', 'tech.annotation_as_int("enabled") ||', 'false || /* DXL: activation is owned by the saved preset. */')
    replace('source/runtime.cpp', 'if (new_technique.annotation_as_int("enabled"))', 'if (false) /* DXL: newly discovered techniques start disabled. */')
    replace('source/runtime.hpp', '\t\tbool is_loading() const {',
            '\t\t// GPU pipeline creation may be queued after valid parameter edits. Only CPU metadata loading prevents saving.\n'
            '\t\tbool dxl_can_save_preset() const { return _frame_count != 0 && _reload_remaining_effects == std::numeric_limits<size_t>::max(); }\n\t\tbool is_loading() const {')
    replace('source/runtime.cpp', 'void reshade::runtime::save_current_preset(ini_file &preset) const\n{',
            'void reshade::runtime::save_current_preset(ini_file &preset) const\n{\n\t// DXL can reset while asynchronous loading is incomplete. Preserve the existing preset.\n\tif (!dxl_can_save_preset()) return;')
    replace('source/runtime.cpp', 'if (effect_list.find(effect_index) == effect_list.end())',
            'if (!_effects[effect_index].compiled) /* DXL UI also edits disabled effects. */')
    addons_file = source/'source/addon_manager.cpp'
    addons = addons_file.read_text(encoding='utf-8')
    start = addons.index('void reshade::load_addons()')
    end = addons.index('bool reshade::has_loaded_addons()', start)
    addons_file.write_text(addons[:start] +
        '// Private DXL host never loads executable or built-in add-ons.\n'
        'void reshade::load_addons() {}\nvoid reshade::unload_addons() {}\n\n' + addons[end:], encoding='utf-8')
    replace('source/ini_file.hpp', 'static bool flush_cache();', 'static bool flush_cache(bool force = false);')
    replace('source/ini_file.cpp', 'bool reshade::ini_file::flush_cache()', 'bool reshade::ini_file::flush_cache(bool force)')
    replace('source/ini_file.cpp', 'file.second->_modified && (std::filesystem::file_time_type::clock::now() - file.second->_modified_at) > std::chrono::seconds(1)',
            'file.second->_modified && (force || (std::filesystem::file_time_type::clock::now() - file.second->_modified_at) > std::chrono::seconds(1))')
    replace('CMakeLists.txt', '    RESHADE_GUI\n', '    RESHADE_GUI=0\n')
    replace('CMakeLists.txt', '    RESHADE_API_LIBRARY_EXPORT\n',
            '    RESHADE_API_LIBRARY_EXPORT\n    ReShadeRegisterAddon=DxlReShadeRegisterAddon\n    ReShadeUnregisterAddon=DxlReShadeUnregisterAddon\n    ReShadeVersion=DxlReShadeVersion\n')
    replace('CMakeLists.txt', '  examples/09-depth/generic_depth_addon.cpp\n', '')
    replace('CMakeLists.txt', '  examples/15-effect_runtime_sync/runtime_sync_addon.cpp\n', '')
    replace('CMakeLists.txt', 'set_target_properties(ReShade PROPERTIES SUFFIX ${RESHADE_SUFFIX}${CMAKE_SHARED_LIBRARY_SUFFIX})',
            'set_target_properties(ReShade PROPERTIES OUTPUT_NAME "DXL-ReShade" SUFFIX ${CMAKE_SHARED_LIBRARY_SUFFIX})')
    # Avoid resource-tool dependencies on compiled overlay shaders when GUI=0;
    # resource.rc already selects only resources used by this build.
    replace('deps/CMakeLists.txt', '    pip install -r requirements.txt\n',
            '    "${Python_EXECUTABLE}" -m pip install -r requirements.txt\n')
    replace('deps/CMakeLists.txt', '    python -m glad ', '    "${Python_EXECUTABLE}" -m glad ')
    (root/'sources.json').write_text(json.dumps(records, indent=2)+'\n', encoding='utf-8')
    print(source)
