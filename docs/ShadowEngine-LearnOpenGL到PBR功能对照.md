# Shadow Engine：LearnOpenGL 到 PBR 的真实功能审计

这份文档按 LearnOpenGL 的目录重新核对代码，不把“有相似画面”当作“完整实现”。结论分成两种口径：

- 如果目标是掌握进入 PBR 所必需的核心知识，Shadow Engine 已经覆盖主体。
- 如果目标是逐章复刻 LearnOpenGL 从入门到 PBR 的全部案例，目前没有全部完成，也没有必要为了凑数量实现所有 API 专题。

## 一、Getting Started

| 主题 | 状态 | Shadow Engine 对应内容 |
| --- | --- | --- |
| Window、Render Loop | 已实现 | Win32 Window、非阻塞消息循环、D3D12 每帧提交 |
| Triangle、Shader | 已实现 | HLSL 6、DXC、VS/PS、Root Signature、PSO |
| Texture | 已实现 | GLB 内嵌 Base Color、Normal、Metallic-Roughness 纹理与 SRV |
| Transform、Coordinate Systems | 已实现 | Model/View/Projection、世界空间法线、非均匀缩放 Normal Matrix |
| Camera | 已实现 | Blender 风格 Orbit/Pan/Zoom 相机 |

这一部分虽然 API 从 OpenGL 换成 D3D12，但顶点如何经过坐标空间并进入光栅化的概念已经覆盖。

## 二、Lighting 与 Model Loading

| 主题 | 状态 | 说明 |
| --- | --- | --- |
| Basic Lighting、Material | 已实现并升级为 PBR | Lambert 漫反射概念保留在 Cook-Torrance 的 Diffuse 项中 |
| Lighting Maps | 已实现为 PBR 贴图 | Base Color、Normal、Metallic-Roughness |
| Directional / Point Light | 已实现 | 最多八盏场景灯 |
| Spot Light | 未实现 | 当前额外实现了 Area Light；Spot Light 不是 PBR 的必要条件 |
| Multiple Lights | 已实现 | Forward 和 Deferred 都遍历相同 Light 数据 |
| Model Loading | 部分实现 | 使用 glTF/GLB，不用 Assimp；作品集契约限制为单 Mesh、单材质 |

## 三、Advanced OpenGL 专题

| 主题 | 状态 | 说明 |
| --- | --- | --- |
| Depth Testing | 已实现 | 主深度缓冲与 Depth PSO |
| Stencil Testing | 未实现 | 没有轮廓/模板遮罩示例 |
| Blending / Transparency | 未实现 | 当前材质按 Opaque 渲染；Deferred 也没有透明 Forward Pass |
| Face Culling | 未启用 | 当前主 Rasterizer PSO 使用 `CullMode = NONE`，双面都参与光栅化 |
| Framebuffers | 已实现为 D3D12 Render Target | HDR、GBuffer、SSAO、Bloom、Shadow Map |
| Cubemap | 已实现 | HDR 经纬图转换为 TextureCube，并用于天空和 IBL |
| Uniform Buffer | 已实现为 Constant Buffer | 每帧、每物体常量切片 |
| Geometry Shader | 未实现 | 不属于当前作品集需要的管线阶段 |
| Instancing | 未实现 | 当前每个 SceneObject 独立 Draw |
| MSAA | 未实现 | 当前所有 Render Target 为单采样 |

这些未实现项是独立的 API/效果练习，不是 Cook-Torrance PBR 成立的必要条件。作品集可以明确写“选择性覆盖”，不能写“逐章完整复刻”。

## 四、Advanced Lighting

| 主题 | 状态 | 说明 |
| --- | --- | --- |
| Gamma Correction | 已实现 | Base Color 使用 sRGB SRV；最终输出统一 Gamma |
| Directional Shadow Mapping | 已实现 | 独立深度 Pass、Bias、3×3 PCF |
| Point Shadow | 未实现 | 没有深度 Cubemap |
| Normal Mapping | 已实现 | glTF Tangent、TBN、Normal Strength |
| Parallax Mapping | 教学近似 | 使用单次 UV Offset，并临时复用 Normal 纹理 R；不是独立 Height Map/POM |
| HDR、Exposure、Tone Mapping | 已实现 | 线性 HDR 中间目标与最终后处理 |
| Bloom | 已实现 | 亮部提取、横纵模糊、最终合成 |
| Deferred Shading | 已实现 | 三张 GBuffer 与全屏 PBR Lighting |
| SSAO | 教学近似 | 屏幕空间近似版本，不是 LearnOpenGL 的 Kernel + Noise 标准实现 |

## 五、PBR

| 主题 | 状态 | 说明 |
| --- | --- | --- |
| Metallic/Roughness Workflow | 已实现 | Base Color、Metallic、Roughness、Normal |
| Cook-Torrance BRDF | 已实现 | GGX NDF、Smith Geometry、Schlick Fresnel、能量守恒 |
| PBR Direct Lighting | 已实现 | Directional、Point、Area Lights |
| HDR Environment | 已实现 | Newport Loft HDRI 与 TextureCube |
| Diffuse IBL | 教学近似 | Shader 中对 Cubemap 做少量半球方向采样 |
| Specular IBL | 教学近似 | Roughness 多方向采样与解析 BRDF 近似 |
| Irradiance Cubemap | 未实现 |
| GGX Prefilter Mip Chain | 未实现 |
| BRDF Integration LUT | 未实现 |

LearnOpenGL 的完整 Specular IBL 会读取预过滤环境贴图的 Roughness Mip，并用 `NdotV/Roughness` 查询 BRDF LUT。Shadow Engine 目前没有真正生成这三类预计算资源，所以准确说法是：

> 已完成 Cook-Torrance PBR 主体和可运行的教学版 IBL，但尚未完成 LearnOpenGL 标准 Split-Sum IBL 资源链。

## Forward 与 Deferred 为什么看起来相同

两条路径使用相同材质、灯光、阴影、IBL、Cook-Torrance 公式和后处理，因此最终 Lit 画面应该非常接近。它们对比的是“光照发生在哪里”，不是两种美术风格：

```text
Forward：Mesh Draw → Pixel Shader 直接光照 → HDR

Deferred：Mesh Draw → GBuffer
                     → SSAO
                     → Fullscreen PBR Lighting → HDR
```

要观察真实差异，应切换 GBuffer/SSAO Debug View，或在 PIX/RenderDoc 中比较 Pass、Render Target、Draw Call 与资源状态。不要故意把两条路径调成不同颜色来制造区别，那会破坏对比的可信度。

## 当前可以怎样描述作品集

推荐描述：

> 使用 C++20、D3D12 与 HLSL 实现的可检查 PBR 学习渲染器；同一场景支持 Forward / Deferred 一键切换，包含 glTF PBR 材质、HDRI、方向光阴影、GBuffer Debug、SSAO、HDR、Bloom 与 Tone Mapping。

暂时不要描述为：

> 完整复刻 LearnOpenGL 全教程，或完整实现标准 Split-Sum IBL。

## 官方参考

- [LearnOpenGL 课程目录](https://learnopengl.com/Getting-started)
- [PBR Theory](https://learnopengl.com/PBR/Theory)
- [PBR Lighting](https://learnopengl.com/PBR/Lighting)
- [Diffuse irradiance](https://learnopengl.com/PBR/IBL/Diffuse-irradiance)
- [Specular IBL](https://learnopengl.com/PBR/IBL/Specular-IBL)
- [Shadow Mapping](https://learnopengl.com/Advanced-Lighting/Shadows/Shadow-Mapping)
- [Normal Mapping](https://learnopengl.com/Advanced-Lighting/Normal-Mapping)
- [Parallax Mapping](https://learnopengl.com/Advanced-Lighting/Parallax-Mapping)
