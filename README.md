# Shadow Engine

Shadow Engine is a Windows real-time rendering study engine built with C++20,
Direct3D 12, and HLSL. Its two goals are to make modern rendering concepts
inspectable and to grow into a polished technical-art portfolio project.

The current portfolio foundation can import reusable glTF/GLB meshes, render textured Cook-Torrance
metallic/roughness materials, light them with LearnOpenGL's Newport Loft HDRI Cubemap, and cast
directional-light PCF shadows. A small scene editor provides Blender-style navigation, picking,
in-viewport transform gizmos, material/environment controls, debug views, and JSON scene persistence.
The portfolio direction is an inspectable Forward/Deferred renderer. Both paths use the same scene,
Cook-Torrance PBR materials, lights, shadows, and HDRI; the editor switches between direct per-object lighting
and GBuffer-driven fullscreen lighting with one control.

The completed learning path now also includes tangent-space normal strength and lightweight parallax, a GBuffer-driven SSAO pass,
an HDR intermediate target, exposure, bright-pass Bloom, separable blur, and final Tone Mapping/Gamma. The exact mapping and
intentional gaps compared with LearnOpenGL through PBR are listed in `docs/ShadowEngine-LearnOpenGL到PBR功能对照.md`.

The same `assets/environments/NewportLoft/newport_loft.hdr` resource drives the visible sky, raster PBR IBL,
and both raster lighting paths. It is converted from equirectangular HDR data to a D3D12 TextureCube during startup.


The editor exposes a two-window scene browser/inspector with manually controlled Directional, Point, and Area
lights. The first Directional light drives the existing shadow map; Point and Area lights currently use
the direct PBR pass without separate shadow maps. See `docs/ShadowEngine-场景光源与面板.md`.

Press `Tab` to toggle the complete editor overlay. This hides both panels, light markers, and Gizmos for a
clean render preview; press `Tab` again to restore them.

## Requirements

- Windows 10 or Windows 11
- Visual Studio 2022 with the Desktop development with C++ workload
- Windows 10/11 SDK
- CMake 3.24 or newer

## Build

Run from a Visual Studio Developer PowerShell:

```powershell
cmake -S . -B build
cmake --build build --config Debug
.\build\Debug\ShadowEngine.exe
```

## 在 VS Code 中运行

仓库已经包含统一的 VS Code 构建与调试配置。首次打开项目后，请先确认 VS Code
已经识别到 `CMake`、`C/C++` 和 `HLSL Tools` 扩展。

1. 使用 VS Code 打开仓库根目录，而不是单独打开某个 `.cpp` 文件。
2. 按 `Ctrl+Shift+B` 配置并构建 Debug 版本。
3. 按 `F5` 构建、启动程序并附加 Visual Studio Windows Debugger。
4. 只想运行而不调试时，打开命令面板，选择
   `Tasks: Run Task` -> `Shadow Engine: Run Debug`。

VS Code 使用 `CMakePresets.json`，将产物统一放在
`build/vscode/Debug/ShadowEngine.exe`。如果刚安装完 CMake 后 VS Code 仍提示找不到
`cmake`，请完全退出并重新打开 VS Code，让它读取更新后的系统 `PATH`。

等价的终端命令是：

```powershell
cmake --preset vs2022-x64
cmake --build --preset debug
.\build\vscode\Debug\ShadowEngine.exe
```

The Debug build enables the D3D12 Debug Layer when the Graphics Tools optional
Windows feature is installed.

## Asset workflow

- Runtime models with embedded textures: `assets/models/<AssetName>/<AssetName>.glb`
- Blender export add-on: `tools/blender_addon/shadow_engine_exporter/`

The repository does not require `.blend` files or source textures. Keep editable DCC projects wherever you normally
work; the Blender add-on lets you select any texture folder and exports only the runtime GLB into this repository.
The engine discovers GLB files under `assets/models/` at startup. The Editor shows one entry per GLB and creates one
scene object from a supported single-Mesh, single-material asset. This intentionally small contract keeps portfolio
work focused on PBR, Forward/Deferred render passes, lighting, and visual debugging.

The checked-in `assets/scenes/current.scene.json` is the final PBR demo: it uses the embedded-texture
`assets/models/DamagedHelmet/DamagedHelmet.glb`, a ground, and a manually authored fill light. The asset credits and
license notes are next to the GLB in `assets/models/DamagedHelmet/README.md`.

Manual runtime placement follows `assets/models/<AssetName>/<AssetName>.glb`. A GLB should embed its textures, so no
separate engine texture folder is required. Named scenes are saved as `assets/scenes/<SceneName>.scene.json`; the
Editor can load a selected scene, save the active scene, or save the current state under a new name.

Project workflow and current milestones are documented in `AGENTS.md` and
`.ai/`.

## 技术美术学习导读

原先按开发里程碑拆分的课程已合并为一份当前代码导读。它按技术美术实际需要组织：架构、数据流、
HLSL、PBR、IBL、阴影、编辑器、调试、日常修改和作品集面试。

- [Shadow Engine 技术美术完整导读](docs/ShadowEngine-技术美术完整导读.md)
- [前向渲染与延迟渲染：同一套 PBR 的两条路径](docs/ShadowEngine-前向与延迟渲染对比.md)
