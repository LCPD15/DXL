# ReShade SDK and private runtime host

Headers in `include/` are unmodified ReShade 6.8.0 SDK headers (API 20), pinned to
`18deaa52de0c425a78b329e9cb3c497281cd00ec`:
https://github.com/crosire/reshade/tree/18deaa52de0c425a78b329e9cb3c497281cd00ec

Build the private `DXL-ReShade.dll` with `scripts/build_reshade_runtime.ps1`.
The immutable source and submodule revisions are listed in
`scripts/prepare_reshade_runtime.py`. Downloaded sources and build products stay
under the external DXL workspace, not in the code checkout.

Hosting changes (compiler and effect renderer are unchanged):

- Private DLL entry point: no graphics, input, network or proxy hook installation.
- No executable add-on auto-loading, ReShade overlay or ReShade update request.
- DXL owns effect runtime creation, present calls, destruction and parameter UI.
- Relative paths start at `DXL_RESHADE_BASE_PATH` when set before DLL loading,
  otherwise `%LOCALAPPDATA%\DXL\ReShade`. The game's ReShade.ini is never read
  for the private runtime base path.
- Unique `DxlReShadeVersion` identity and renamed add-on registration exports
  avoid being detected as the game's existing ReShade installation. Callers
  resolve the effect-runtime exports on the exact private DLL module handle and
  verify the exported `DxlReShadeApiVersion` is 20 before using SDK virtual APIs.
- Logging is opt-in via `DxlReShadeOpenLog(const wchar_t *path)` after DLL load.
- `DxlReShadeFlushIni()` bypasses the usual delayed INI write, preserving edits
  immediately. Saving during initial/asynchronous loading is ignored to protect
  the last complete preset. Parameter values for disabled effects are retained.
- `DxlReShadeSavePreset(effect_runtime *)` reports whether a complete preset
  could be saved, allowing the host to retry after asynchronous CPU loading.
  Queued GPU pipeline creation does not prevent immediate parameter saving.
- Newly discovered techniques start off even when their shader has an `enabled`
  annotation; the per-game preset controls activation.

The official standalone creation entry point supports D3D9, D3D10, D3D11 and
D3D12 swapchains in this revision. It does not accept a bare texture or native
OpenGL/Vulkan swapchain. DXL therefore needs a compatible presentation bridge for
those other backends; the SDK comment alone should not be read as support.

License: `licenses/ReShade-LICENSE.txt`; runtime dependency notices:
`licenses/ReShade-ThirdParty-NOTICES.txt`. DXL hosting code follows DXL's license.
