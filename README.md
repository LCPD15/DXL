# DXL — DLSS eXtended Loader

**English** | [简体中文](README_ZH.md) · **Version 0.6**

DXL (DLSS eXtended Loader) expands on my previous RE DLSS5 Load Mod. It aims to make DLSS NR as easy as possible to enable across games. In supported games, it can use native motion vectors and work with frame generation for higher-quality results. Compatibility varies by game.

[Previous mod on GitHub](https://github.com/LCPD15/RE_DLSS5_Load_Mod) · [Previous mod on Nexus Mods](https://www.nexusmods.com/onimushawayofthesword/mods/32) · [DXL project](https://github.com/LCPD15/DXL)

[Download the complete package](https://github.com/LCPD15/DXL/releases/latest) · [English online guide](docs/DXL-Guide-EN.md)

<img width="1672" height="941" alt="image" src="https://github.com/user-attachments/assets/bcc09cc3-f160-464c-81b0-4dbf9f1f0d3c" />

## Features

- **Native motion vectors** — Uses the game's own DLSS motion data where supported, helping temporal processing follow the scene and reduce flicker.
- **Higher frame rates** — Lower the internal neural rendering resolution to save GPU time while keeping the image clear.
- **No configuration needed** — No DLL, ASI or addon files need to be installed in the game folder. Launch the game with this tool; by default, press Del in-game to toggle the effect and End to toggle the settings panel.
- **In-game menu** — Adjust every parameter and view debug information without leaving the game.

These effect and performance examples are from the previous mod and show its older interface. DXL defaults to Del / End. See the installation instructions below for RE Engine prerequisites.

![Previous mod: image quality and frame rate at different NR render scales](https://github.com/user-attachments/assets/b922778a-3d6e-4612-961a-fa8e3cfa08d0)

![Previous mod: native motion vectors combined with frame generation](https://github.com/user-attachments/assets/459a24ee-18bd-4652-a0af-f277ebe79c58)

<img width="1546" height="1043" alt="image" src="https://github.com/user-attachments/assets/fc9a0998-79e0-47a5-8d91-0c429f6d7007" />



## Install and start a game

Requires Windows x64, a compatible NVIDIA RTX GPU and Microsoft WebView2 Runtime. For RE Engine games, install [REFramework](https://github.com/praydog/REFramework-nightly/releases) and [ReShade](https://www.reshade.me/#download) as indicated in the profile.

1. Extract the complete release package and run DXL.exe. Keep all included files together.
2. Scan for installed games in Library, or use Add game to select the actual game EXE. Open its cover to check the path. Enter launch arguments only if the game requires them.
3. Start with Auto and choose Launch from tool. Alternatively, keep DXL and global monitoring on, then launch an added game through its platform or official launcher.
4. In the game, press Del to enable NR and End to open the panel. The master switch starts off for new profiles; your saved state is retained afterward. Parameters save automatically.

## Game shortcuts

Del: toggle effects. End: toggle the panel. Alt + F8: try loading into the current game window. Esc or the panel’s × closes the panel. Change bindings in Global settings.

If loading has no effect or startup fails, exit the game and retry in Compatibility mode. This may miss native DLSS data. For frame generation, enable in-game DLSS upscaling and try Auto or Early first.

## Adjust the NR effect

Press End, change one setting at a time in the same scene, then press Del to compare. Defaults below apply to new profiles.

| Setting | Range and default | What it does |
| --- | --- | --- |
| Intensity | 0–1 / 1 | Controls enhancement strength. Lowering it does not stop model computation. |
| Colour Strength | 0–1 / 1 | 0 retains original colour ratios while keeping NR brightness and detail; 1 keeps NR colours. |
| Style / Preset | Cinematic Preset 0 | Compare Default, Natural and Cinematic styles. Leave the preset at its default to start. |
| Local Tone / Local Structure | 0–2 / 1 | Adjust local tone response and structural enhancement separately. |
| Skin Strength | Panel: 0–1 Default: 0.6 | Adjusts skin-related structure. Higher is not always more natural. |
| NR Render Scale | 0.5–1 / 1 | Lowers NR processing resolution to save GPU time. Final game output resolution stays the same. |
| Self Layers | 1–3 continuous Default: 1 | Amplifies the NR effect difference without extra model evaluations. |
| True NR Layers | 1–5 whole numbers Default: 1 | Runs NR repeatedly in sequence, increasing GPU time and memory use. |
| Optical Flow | On by default Balanced | Uses enabled, available native vectors first; otherwise estimates flow. Choose Performance, Balanced or Quality. |

For better performance, set True NR Layers to 1 first, then lower NR Render Scale or flow quality. Leave Auto Mask and UI Correction at their defaults to start. Set Debug view to Off for normal viewing.

## Color grading and filters

Press End and select **Color Grading** beside **DLSSNR**. Enable switches sit next to their parameter values. Reset restores only the default value and keeps the switch state. Settings are saved per game; disabled effects skip processing.

Basic grading provides exposure, contrast, saturation, temperature, tint, shadows, midtones and highlights at the game-image stage. On the native SR route this is usually before game UI, and later game exposure or tone mapping may affect the result.

LUTs, sharpening, bloom, vignette, film grain, monochrome and custom effects process the final image, including game UI. Post-processing works independently of NR and its menu pauses when a safe presentation route is available. Frame-generation compatibility safeguards still apply.

Sharpening and bloom strength range from 0 to 3. Expand Bloom details for Highlight threshold (0–2; default 0.65), Threshold softness (0–1; default 0.5), Glow radius (0.25–3; default 1), Scatter (0–1; default 0.7), and Glow saturation (0–2; default 1). These controls follow the bloom enable switch; each Reset restores only its value.

Put PNG lookup tables in the `lut` folder beside `DXL.exe`, then select a file, enable LUT and set its strength. The included `Neutral-16.png` is a neutral template. Save custom LUTs under new filenames; do not resize or crop them. Ordinary photos are not LUTs. See `lut/README.md` for layouts.

In **Extras → Post-processing**, open `post-processing`, add native ReShade `.fx` files with their required `.fxh` includes and textures, and retain relative folders. Click **Reload custom FX** in **Color Grading → Custom FX**. Each technique has its own toggle; parameters appear automatically and save per game. Reset changes values only. Originals are neither converted nor archived, and existing DXL FX examples remain supported. DXL effects run first, followed by ReShade files in alphabetical order. See `post-processing/README.md` for usage and authoring.

ReShade multi-pass rendering and external textures are supported. Game depth, motion vectors, add-ons and keyboard/mouse source uniforms are not currently provided; effects requiring them are outside this compatibility scope. Save edited examples under new filenames; updates preserve files you add yourself.


## Semantic Mask is experimental

The complete package includes the recognition model. This feature starts off. Enable it, select categories such as People or Vehicles, then set their strength. Unselected objects use Background strength. Try Background 1 and People 0, and check alignment with Mask preview (R).

Strength ranges from 0 to 1. Mask feather defaults to 8 to soften edges. Recognition may be poor, especially for anime scenes, occlusion or small objects. Turn it off if it misidentifies objects or produces unnatural edges.

[Build and dependencies](DEPENDENCIES.md) · [Third-party components](THIRD_PARTY_NOTICES.md) · [Component licenses](LICENSE_STATUS.md)

## License

DXL is licensed under [AGPL-3.0-only](LICENSE). Copyright (C) 2026 LCPD15. Third-party components retain their own licenses. [Corresponding source](SOURCE_CODE.md).

[Automatic updates](docs/UPDATES.md): checks in the background at startup; install and restart after downloading, or keep the package for later.
