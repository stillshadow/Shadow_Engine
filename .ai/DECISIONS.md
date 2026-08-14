# Technical decisions

## 2026-08-09 - Use Direct3D 12 as the learning renderer API

- Decision: Build the renderer on Direct3D 12 with C++20 and HLSL 6.
- Context: The project should revisit LearnOpenGL rendering topics while also becoming a distinctive technical-art portfolio piece.
- Rationale: Direct3D 12 exposes modern resource, descriptor, command, and synchronization concepts while using HLSL directly. A focused renderer provides more portfolio value than a broad game-engine clone.
- Consequences: Early milestones contain more platform and synchronization code. Engine abstractions must remain small and teachable so visual work is not delayed indefinitely.
- Rejected alternatives: Direct3D 11 would reach visual features sooner but exposes fewer modern explicit-API concepts; Vulkan would add cross-platform value but moves the project away from a direct HLSL-first Windows workflow.

## 2026-08-09 - Keep the bootstrap dependency-free

- Decision: The initial window, swap chain, synchronization, clear, and resize path use only Win32 and Windows SDK libraries.
- Context: The repository starts empty and needs a verifiable baseline before introducing editor, model, or asset dependencies.
- Rationale: A dependency-free first milestone makes ownership and failure modes visible and reduces setup uncertainty.
- Consequences: Window and application-loop code is maintained locally. Dear ImGui and asset libraries will be evaluated in later tasks.
- Rejected alternatives: Introducing SDL, GLFW, or an editor framework at bootstrap would reduce platform code but obscure the first Windows/D3D12 lifecycle.

## 2026-08-09 - Use Chinese teaching comments and progressive chapter-based learning notes

- Decision: Source-code teaching comments use Chinese while identifiers and API names remain English. Each rendering milestone receives a simple, progressive Chinese tutorial under `docs/`.
- Context: The engine is both an implementation project and a structured review of rendering knowledge, following the explanatory style of LearnOpenGL.
- Rationale: Comments near the code preserve local intent and pitfalls. Chapters begin with the visible result, introduce only the concepts required by the milestone, follow execution order, and end with a short recap so the learning flow remains approachable rather than encyclopedic.
- Consequences: New rendering work is incomplete until its non-obvious ownership, state, and synchronization logic is commented and the corresponding learning chapter is updated. Trivial syntax should remain uncommented.
- Rejected alternatives: English-only comments reduce accessibility for the current learning goal; putting all explanations inside source files would make implementation harder to scan and maintain.

## 2026-08-10 - Treat DX12 infrastructure as a technical-art overview

- Decision: Merge the DX12 bootstrap and first HLSL triangle lessons into one chapter 0 overview. Subsequent numbered chapters start with rendering concepts and visual features, beginning with coordinate spaces and MVP transforms.
- Context: The project targets technical-art growth and portfolio work. Low-level DX12 setup is useful background but is rarely written directly in day-to-day Unreal Engine technical-art workflows.
- Rationale: A technical artist should understand CPU/GPU asynchrony, resource usage, and the vertex-to-pixel pipeline without spending course time memorizing Win32, adapter, swap-chain, fence, Root Signature, or PSO boilerplate. Unreal Engine mappings keep the standalone implementation connected to practical work.
- Consequences: Framework code remains explicit and commented for reference, but course depth is reserved for HLSL, coordinate spaces, materials, lighting, Render Pass relationships, performance, tools, and visual results. Each feature chapter includes a concise Unreal Engine mapping.
- Rejected alternatives: Keeping one detailed course chapter per low-level implementation milestone would overemphasize graphics-programmer responsibilities; removing the standalone framework entirely would hide useful pipeline context and reduce independent experimentation value.

## 2026-08-11 - Prioritize a portfolio vertical slice over prerequisite DX12 mastery

- Decision: Continue building a compact hybrid-renderer portfolio slice immediately, teaching only the DX12 concepts required by each visible feature and returning to a focused framework review after the first complete visual slice.
- Context: The learner already has stronger Shader and rendering-pipeline knowledge than low-level DX12 framework experience and wants to finish a presentable technical-art work before undertaking a full API study.
- Rationale: Material math, HLSL, debug views, parameterization, visual quality, and Unreal Engine transfer provide the main technical-art value. The existing DX12 framework can remain an explicit, commented host without becoming a prerequisite course.
- Consequences: Milestones begin with a visible result, spend most teaching depth on pipeline and Shader behavior, and compress Descriptor, synchronization, and platform setup into concise supporting explanations. Each milestone still leaves enough named structure for the learner to locate and explain the relevant binding path.
- Rejected alternatives: Pausing feature work for a complete DX12 curriculum would delay the portfolio; switching to OpenGL would duplicate framework work and move Shader practice away from the existing HLSL and Unreal Engine workflow.

## 2026-08-12 - Use Dear ImGui for rendering-tool controls

- Decision: Integrate pinned Dear ImGui v1.92.9 from its official repository and compile only its core plus Win32 and DirectX 12 backends.
- Context: Material parameters now need a visible, extensible panel, and future PBR, lighting, GBuffer, and debug controls will add many more editable values.
- Rationale: Immediate-mode widgets map directly to per-frame renderer parameters and are widely suited to engine debug and technical-art tools. A pinned version keeps builds reproducible while avoiding a custom widget system.
- Consequences: The first configure requires network access to fetch the pinned source. The renderer owns a small shader-visible SRV heap and forwards Win32 input to the official backend. Third-party source warnings are isolated from project `/W4 /WX` rules.
- Rejected alternatives: Native Win32 controls would avoid a dependency but scale poorly for future material tooling; a custom D3D12 UI and font renderer would add substantial unrelated framework work.

## 2026-08-12 - Separate scene, editor, renderer, and persistence responsibilities

- Decision: Keep editable objects in a DX12-independent `Scene`, move Inspector/Gizmo state into `EditorLayer`, keep GPU work in `D3D12Renderer`, and serialize local scene state through `SceneSerializer` using JSON.
- Context: Transform editing, persistence, render-pass controls, and future lighting tools would otherwise continue growing the already broad renderer class.
- Rationale: The four-way split follows the actual data flow without introducing a speculative entity-component system. ImGuizmo supplies the viewport transform interaction, while nlohmann/json keeps persistence readable and validated.
- Consequences: ImGuizmo is pinned to commit `18cef5e031d8c6973d80284c67f60549fafd78c1`; nlohmann/json is pinned to v3.12.0. The local working scene is `assets/scenes/current.scene.json` and is ignored by Git. The teaching scene remains a fixed three-object array until dynamic object creation becomes a real requirement.
- Rejected alternatives: Keeping all editor state in `D3D12Renderer` would preserve fewer files but worsen ownership; implementing a custom gizmo and JSON parser would delay rendering work; introducing a full ECS, reflection system, or asset database would exceed the portfolio scope.

## 2026-08-12 - Add a small asset layer and reproducible procedural environment

- Decision: Use pinned TinyGLTF v2.9.7 for indexed static glTF/GLB primitives, identify reusable meshes with string asset keys, and implement the first HDR environment as an analytic procedural sky feeding the same diffuse/specular IBL interfaces used by prefiltered cubemaps.
- Context: The portfolio slice needs multiple PBR assets, saved instances, environment reflections, and shadows before multi-pass reflection work, while the code must remain approachable for later review and must not depend on unlicensed HDR assets.
- Rationale: AssetManager, MeshAsset, and GltfLoader form the smallest clear boundary between files, Scene instances, and GPU resources. A deterministic procedural HDR sky gives metals directional high-range reflections and exposes Roughness/Fresnel behavior without adding HDR decoding, cubemap conversion, convolution passes, and BRDF LUT generation in the same milestone.
- Consequences: Scene objects are now dynamic and persist `assetKey`; imported geometry and t0-t2 textures are rebuilt when asset revision changes. IBL has separate diffuse/specular debug views and saved intensity/rotation, but external HDRI import and physically precomputed irradiance/prefilter maps remain a future visual-quality upgrade. Directional shadows use a separate explicit 2048 depth pass with fixed orthographic coverage and 3x3 PCF.
- Rejected alternatives: A full asset database/ECS would add unrelated framework work; Assimp would broaden formats beyond the glTF-focused PBR pipeline; requiring a third-party HDRI would reduce repository reproducibility; implementing GPU cubemap convolution before the snow vertical slice would delay the main technical-art feature.

## 2026-08-13 - Keep the portfolio asset contract single-Mesh and single-material

- Decision: Support one Mesh primitive, one material, and one SceneObject per runtime GLB until the hybrid-renderer vertical slice is complete.
- Context: Multi-Mesh support required part hierarchy, node-transform handling, per-part editor state, multiple draw persistence, and broader Blender validation before it improved the core visual result.
- Rationale: The project exists to demonstrate rendering, HLSL, material behavior, lighting, and technical-art diagnostics. A direct SceneObject-to-draw relationship is easier to learn, explain, and maintain while building the portfolio effect.
- Consequences: Blender export rejects multiple selected Mesh objects or material slots and requires applied transforms. Complex source assets must be simplified or replaced with suitable single-Mesh assets.
- Rejected alternatives: Keeping the completed multi-part architecture would increase engine complexity before it contributed to the hybrid-renderer result; silently importing only one part would produce unreliable scenes.

## 2026-08-13 - Store named scenes as direct JSON files

- Decision: Keep each scene as an independent `assets/scenes/<Name>.scene.json` file and expose a small Editor list with load, save-current, and save-as-new actions.
- Context: The portfolio needs alternate scene arrangements, but does not need an asset database, scene registry, hierarchy browser, or project launcher.
- Rationale: Direct files preserve the existing readable serializer and make scene variants easy to inspect, copy, version, and explain.
- Consequences: named scenes and the reproducible `current.scene.json` may be committed as portfolio assets. If the current scene is missing, the editor uses an in-memory default and does not recreate the file until the user explicitly saves. Switching scenes is explicit, and Save As refuses to overwrite an existing name.
- Rejected alternatives: A scene database or manifest would add synchronization and identity problems without helping the wet-surface result; silently overwriting named scenes would risk losing arrangements.

## 2026-08-14 - Keep the portfolio presentation focused on renderable units

- Decision: Abandon the authored full-corner scene workflow and keep the engine's portfolio input focused on individual reusable meshes/materials and isolated rendering demonstrations.
- Context: The abandoned full-corner import added hundreds of source files, scene-layout concerns, and format-conversion debugging without improving the technical-art effect itself.
- Rationale: A small, controlled render target makes HLSL, PBR, GBuffer data, Forward/Deferred differences, debug views, and performance tradeoffs easier to explain and present.
- Consequences: The discarded scene assets and scene-specific importer, shadow, and light experiments were removed. The generic GLB discovery and named-scene infrastructure remain available for later isolated assets.
- Rejected alternatives: Keeping a full authored corner would continue the asset-cleanup and coordinate-debugging work that is outside the core portfolio effect.

## 2026-08-14 - Present one scene through Forward and Deferred pipelines

- Decision: Keep two complete raster paths and expose a one-click Forward/Deferred switch for the same scene, camera, materials, and lights.
- Context: A feature that the learner cannot yet explain weakens the portfolio. The project needs to demonstrate rendering-pipeline understanding through code that can be reviewed, compared, and presented confidently.
- Rationale: Forward and Deferred make geometry, material data, GBuffer storage, fullscreen lighting, pass dependencies, and resource transitions directly comparable. They build on the learner's existing PBR/HLSL knowledge without requiring an unrelated advanced API feature.
- Consequences: Forward writes lit HDR color per object. Deferred writes three MRTs, evaluates SSAO, and performs one fullscreen PBR lighting pass. Both retain the same shadows, environment, lights, post-process, and editor scene. Transparent Deferred materials and tiled light culling remain future extensions.
- Rejected alternatives: Keeping an advanced reflection experiment would add portfolio surface area the learner cannot explain deeply; a material-only effect would expose fewer pipeline tradeoffs; a full path tracer would exceed the current teaching scope.

## 2026-08-14 - Keep scene lighting explicit and capped at eight lights

- Decision: Store Directional, Point, and Area lights as a small `Scene::SceneLight` vector, copy up to eight entries into each object constant-buffer slice, and evaluate them in the forward PBR pixel shader.
- Context: The editor needed manual light control and additional light types, but the project still targets a readable technical-art study renderer rather than a general lighting framework.
- Rationale: A fixed array keeps the CPU/HLSL layout inspectable. The first Directional light can reuse the existing shadow pass, while Point and Area lights can be added without introducing a second resource binding system.
- Consequences: Point and Area lights currently have no shadow maps; Area lights use four rectangular samples; larger scenes will eventually need Forward+ or clustered light culling.
- Rejected alternatives: Keeping an animated hard-coded Directional light hides scene authoring; implementing tiled/clustered lighting now would add GPU culling and synchronization complexity before the material portfolio slice needs it.

## 2026-08-14 - Use a self-contained Khronos GLB for the final PBR demo

- Decision: Make `DamagedHelmet.glb` the default saved demonstration asset and keep its PBR maps embedded in the GLB.
- Context: The final scene needs a visually legible material that exercises base color, normal, and metallic-roughness loading without reintroducing a large authored environment.
- Rationale: A single GLB fits the current single-Mesh/single-material contract and makes the asset path reproducible in VS Code and on another machine. Khronos provides a well-documented reference asset and attribution information.
- Consequences: The repository now carries a 3.7 MB binary asset and an attribution file. Public or commercial redistribution must follow the mixed CC BY / CC BY-NC credits or replace the asset with a permissive CC0/CC BY alternative.
- Rejected alternatives: Loose texture files would require a new image-decoder and material-sidecar pipeline; the abandoned full-corner scene would obscure the render passes and increase asset/debugging scope.

## 2026-08-14 - Complete the technical-art PBR path with inspectable post-process passes

- Decision: Add material normal strength/parallax controls, an SSAO full-screen pass, an HDR scene target, exposure, Bloom extraction/blur, and final Tone Mapping; document exact LearnOpenGL coverage and the remaining non-core gaps.
- Context: The portfolio needs to demonstrate the path from GBuffer data to a displayable image, not only a direct forward shader. The learner also needs a reliable checklist for comparing the engine with LearnOpenGL through PBR.
- Rationale: These passes are small enough to read in HLSL, visible in the editor, and directly map to technical-art debugging. Procedural IBL remains the reproducible substitute for true cubemap convolution.
- Consequences: Forward uses shadow → HDR object lighting → Bloom → post-process. Deferred uses GBuffer → SSAO → shadow → fullscreen HDR lighting → Bloom → post-process. The saved demo enables exposure, SSAO, and Bloom.
- Rejected alternatives: Adding every OpenGL tutorial exercise would dilute the portfolio and create unrelated framework code; implementing a full HDRI import/convolution toolchain before the core path would add assets and GPU passes without improving the first demonstrable result.

## 2026-08-14 - Use one real HDRI across both raster paths

- Decision: Replace the analytic procedural environment with LearnOpenGL's Newport Loft HDRI, convert it to a D3D12 TextureCube at startup, and bind the same SRV to the sky and both PBR lighting paths.
- Context: The final image needed a recognizable environment and reflections whose source matched the visible background. The learner specifically requested the Cubemap used by LearnOpenGL.
- Rationale: One shared resource makes environment rotation and intensity coherent across passes and provides a concrete, inspectable HDR asset flow. CPU conversion keeps the first implementation readable and avoids adding a separate GPU bake tool before portfolio capture.
- Consequences: The repository carries a roughly 3.8 MB HDR asset and attribution. Diffuse and rough-specular IBL currently use compact runtime multi-sampling; a precomputed irradiance Cubemap, GGX prefilter mip chain, and BRDF LUT remain a documented quality/performance upgrade.
- Rejected alternatives: Keeping the procedural sky would hide the asset-to-GPU path; implementing a full offline IBL baker in the same milestone would delay the final demonstration and add substantially more infrastructure.
