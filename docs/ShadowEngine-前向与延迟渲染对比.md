# 前向渲染与延迟渲染：同一套 PBR 的两条路径

本章只回答一个问题：同一个场景、同一套材质和灯光，为什么可以用两种方式生成几乎相同的画面？

## 1. 一键切换后发生了什么

Inspector 的 `Render Path` 有两个选项：

```text
Forward
  Mesh Draw
    └─ Vertex Shader → Pixel Shader 直接计算 PBR → HDR Target

Deferred
  Mesh Draw
    └─ GBuffer：BaseColor/Roughness、Normal/Metallic、WorldPosition
  Fullscreen Draw
    └─ DeferredLighting.hlsl 读取 GBuffer → 统一计算 PBR → HDR Target
```

两条路径之后都进入相同的 Bloom、Tone Mapping 和 Gamma 流程，因此对比重点是“光照在哪里发生”。

## 2. Forward 路径

Forward 模式使用 `MaterialPreview.hlsl`。每个物体 Draw 时，Pixel Shader 立即读取材质贴图，恢复法线，并遍历场景灯光计算最终 HDR 颜色。

优点：

- 流程直接，材质数据就在当前 Pixel Shader 中。
- 少量灯光和透明物体容易处理。
- 不需要保存多张 GBuffer。

代价：每个被覆盖的像素都要重新遍历灯光；物体互相遮挡产生的 Overdraw 也会重复执行昂贵光照。

普通 `Lit` 模式下，引擎会跳过 GBuffer 和 SSAO。只有主动切换 GBuffer Debug View 时，才临时执行 GBuffer Pass。

## 3. Deferred 路径

Deferred 模式先使用 `GBuffer.hlsl` 把可见表面写入三张纹理：

| GBuffer | 内容 |
| --- | --- |
| 0 | Base Color + Roughness |
| 1 | World Normal + Metallic |
| 2 | World Position + Valid Mask |

随后 `DeferredLighting.hlsl` 绘制一个覆盖全屏的三角形。每个屏幕像素读取 GBuffer，再遍历相同的 Directional、Point 和 Area Lights，计算 Cook-Torrance PBR、Shadow、SSAO 与 HDRI IBL。

## 为什么 Lit 画面几乎没有区别

这是正确结果。两条路径共用相同的材质输入、灯光、阴影、IBL、PBR 方程、曝光、Bloom 和 Tone Mapping；区别只是像素光照发生在 Mesh Draw 里，还是发生在读取 GBuffer 的全屏 Draw 里。

如果同一个不透明场景在切换后明显变色，反而应该检查 GBuffer 精度、法线编码、sRGB/Linear 转换或两份 Shader 公式是否不一致。作品集应通过 GBuffer Debug View 和 GPU 帧捕获展示管线区别，而不是故意制造不同的最终颜色。

优点：

- 几何材质只写一次，后续效果可以复用屏幕空间数据。
- 大量灯光时更容易扩展为 Light Volume、Tiled 或 Clustered Lighting。
- GBuffer、SSAO 和 Debug View 天然可检查。

代价：

- 占用更多显存和带宽。
- GBuffer 能保存的材质模型受到格式限制。
- 透明物体通常仍需要额外的 Forward Pass。

## 4. 为什么两条路径应当看起来接近

两条 Shader 使用相同的输入含义：

```text
Base Color / Roughness / Metallic / Normal
Camera Position
Directional / Point / Area Lights
Shadow Map
Newport Loft HDRI
IBL Specular Strength
```

它们也使用相同的 GGX、Smith、Schlick Fresnel 和环境光近似。若切换后差异很大，优先检查：

1. GBuffer 是否在线性空间保存 Base Color。
2. 法线编码和解码是否互为逆过程。
3. Roughness 与 Metallic 通道是否一致。
4. 两条路径是否读取同一套灯光和环境参数。
5. Deferred 独有的 SSAO 是否造成预期内的暗部差异。

## 5. 作品集如何展示

推荐录制同一机位的一键切换：

1. Forward Lit：说明每个物体的 Pixel Shader 直接计算光照。
2. Deferred Lit：说明几何先写 GBuffer，再由全屏 Pass 光照。
3. 展示三张 GBuffer 和 SSAO。
4. 增加 Point/Area Light，解释为何 Deferred 更适合继续扩展多灯光。
5. 在 PIX 或 RenderDoc 中对比 Draw Call 和 Render Target 顺序。

面试时可以这样概括：

> 我用同一套场景数据和 Cook-Torrance PBR 实现了 Forward 与 Deferred 两条光照路径。Forward 在物体 Draw 中直接着色；Deferred 先把材质和几何信息写入 GBuffer，再通过全屏 Pass 统一光照。编辑器可以一键切换，并用 Debug View 检查每个中间结果。
