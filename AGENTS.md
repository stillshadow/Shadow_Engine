# Shadow Engine project guide

## Project goal

Shadow Engine is a Windows real-time rendering study engine and technical-art portfolio project. It should make rendering concepts inspectable and produce polished, reproducible visual demonstrations.

The project focuses on rendering, materials, lighting, GPU effects, and their supporting tools. It is not intended to become a general-purpose game engine with gameplay, networking, or a full asset pipeline.

## Confirmed stack

- Language: C++20
- Platform: Windows 10/11, x64
- Graphics API: Direct3D 12
- Shader language: HLSL 6, compiled with DXC when shaders are introduced
- Build system: CMake
- First-party platform dependencies: Windows SDK, Direct3D 12, DXGI

Third-party libraries must be introduced only when a feature needs them and documented in the relevant decision record.

## Repository layout

- `src/Core/`: application lifecycle, platform integration, diagnostics, and shared utilities
- `src/Renderer/`: Direct3D 12 renderer and future render systems
- `docs/`: Chinese, chapter-style rendering tutorials tied to implemented milestones
- `shaders/`: HLSL source files (created when the first programmable pipeline is introduced)
- `assets/`: redistributable demo assets (created when asset loading is introduced)
- `.ai/`: shared project state, tasks, decisions, and handoff records

Generated files belong under `build/` and must not be edited or committed.

## Build and run

From a Visual Studio Developer PowerShell or another shell with CMake and MSVC available:

```powershell
cmake -S . -B build
cmake --build build --config Debug
.\build\Debug\ShadowEngine.exe
```

For single-config generators, the executable location may differ. Do not hard-code a machine-specific compiler or SDK path into project files.

## Development rules

- The active development branch is `dev`. The stable/release branch is not yet confirmed.
- Keep graphics API ownership explicit; prefer RAII and `Microsoft::WRL::ComPtr` for COM resources.
- Check every fallible Direct3D/DXGI call and preserve useful failure context.
- Do not hide synchronization, resource barriers, or descriptor lifetime behind opaque abstractions during the learning stages.
- Source-code teaching comments must use Chinese while identifiers and API names remain in their original English form. Comments should explain intent, rendering concepts, ownership, synchronization, and pitfalls rather than restating syntax.
- Each course chapter starts with what the program visibly does, introduces only the few concepts required for that milestone, uses one consistent analogy or flow, walks through the key code in execution order, and ends with a short recap. Avoid encyclopedia-style API inventories.
- Low-level DX12 bootstrap and first-triangle infrastructure belong in a single technical-art overview chapter: explain their purpose without teaching API memorization. Feature chapters focus on rendering-pipeline knowledge, HLSL, visual results, performance, and a concise Unreal Engine concept mapping.
- Avoid unrelated engine subsystems and speculative generic frameworks.
- Add shader debug views and GPU markers alongside non-trivial visual features when practical.
- Warnings are treated as errors for project code in MSVC builds.

## Validation

Run the narrowest applicable checks first, followed by:

```powershell
cmake --build build --config Debug
git diff --check
```

Rendering milestones also require a manual run with the D3D12 Debug Layer enabled. Use PIX or RenderDoc when a task's acceptance criteria explicitly calls for frame inspection.

## Collaboration state

Read `.ai/STATE.md`, `.ai/TASKS.md`, `.ai/DECISIONS.md`, and the active task file before modifying business files. Current progress and handoff details belong in `.ai/`, not in this file.
