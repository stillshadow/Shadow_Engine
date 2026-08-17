# 场景光源与编辑器面板

这一章解决两个编辑器问题：物体只保留一个选择入口，以及光源不再由代码偷偷旋转，而是作为场景数据由用户控制。

## 1. 面板怎样组织

编辑器现在使用左右两个独立窗口，分别贴在视口左右边缘；中间区域保留给渲染画面。

- 左侧 `Scene Browser` 只负责 `Scene Objects`、`Lights` 的选择，以及添加 Point/Area Light。
- 右侧 `Inspector` 只显示当前选中项的参数和场景文件操作，因此不会再出现重复的 `Current Scene Models` 列表。
- 选中物体时，右侧显示 Transform、PBR 和调试视图；选中光源时，右侧切换到对应的光源参数。

默认场景始终在渲染器内部保留一盏 `Key Directional`，负责基础照明和阴影。它不出现在编辑器列表中，也不提供任何直射光 UI。

左侧只提供 `Add Point` 和 `Add Area`。新增光源后会自动选中，参数立即显示在右侧；所有灯仍受 8 盏上限约束。

选中 Point 或 Area Light 后按 `W`，视口中会显示移动 Gizmo，可以直接拖动光源位置。

Point Light 的圆形图标、Area Light 的整块矩形现在都可以用左键直接选中。光源图标位于物体前方时，编辑器会优先选择光源，避免点击被物体包围盒抢走。

## 7. 视口中的光源图形

为了让可编辑光源不再只是列表中的一行文字，编辑器会把它们投影到视口上：

- Point Light 显示为圆形十字，并用外圈表示 `Range` 的大致屏幕投影。
- Area Light 显示为带填充的矩形面，矩形中心的线段表示发光方向。

选中的光源使用更亮、更粗的颜色。Area Light 选中后按 `E` 切换旋转 Gizmo，拖动旋转环会直接更新它的 `Emission Direction`；按 `W` 仍然是移动。

## 8. 一键隐藏编辑器叠加层

左侧 `Status` 区域的 `Hide Editor UI (Tab)` 会关闭两个编辑器窗口、光源可视化图形和所有 Gizmo，适合检查最终画面或录制作品集视频。隐藏后按键盘 `Tab` 即可恢复完整 UI；这个快捷键不依赖当前是否有窗口可见。

## 单步撤销

按 `Ctrl+Z`，或者点击左侧 `Undo Last Scene Change`，可以撤销最近一次会改变 Scene 数据的操作。连续拖动 Slider、Transform 或 Gizmo 被当成一次操作，会直接回到拖动开始前。

撤销只保存一个快照；进行下一次编辑后，上一个快照会被覆盖。相机浏览和切换选中项不修改 Scene，因此不进入撤销记录。

## 2. 三种光源

### Directional

Directional Light 没有位置，作为渲染器内部的默认主光，不在当前编辑器 UI 中暴露。第一盏 Directional Light 会生成当前的 2048×2048 Shadow Map。

### Point

Point Light 使用 `Light Position`、`Intensity` 和 `Range`。Shader 根据像素到光源的距离计算有限范围衰减：距离越远，能量越低，超过 Range 后不再贡献光照。

### Area

Area Light 使用位置、发光方向、宽度、高度和范围。当前版本在矩形内部进行 4x4 分层采样，再平均为一个近似的面光源结果；它不再把矩形四角当成几盏独立点光源。它不是完整的 LTC 面光源模型，但能表现比点光更连续、更有面积感的高光，并且适合展示 Shader 推导过程。

## 3. 数据流

```text
ImGui Add/Edit Point/Area Light
        ↓
Scene::SceneLight
        ↓
SceneSerializer JSON
        ↓
D3D12Renderer::UpdateObjectConstants
        ↓
ObjectConstants.lights[8]
        ↓
MaterialPreview.hlsl
        ↓
Directional / Point / Area direct lighting
```

每个 SceneObject 都会读取同一组场景光源，写入自己的常量缓冲切片。这样一个材质仍然只需要一次 Draw Call，但每个像素可以遍历最多 8 盏光源。

## 4. 为什么光源数量先限制为 8

这是学习阶段的显式边界。光源数组放在现有 `ObjectConstants` 中，C++ 与 HLSL 共享固定布局，代码容易在调试器和 RenderDoc 中检查。真正的大场景通常会使用 Forward+、Clustered Lighting 或 GPU Light Culling，把光源列表从“每个物体复制”改成“每个 tile/cluster 共享”。当前作品集阶段先保留可读性。

## 5. 场景保存兼容

新场景会增加：

```json
"lights": [
  {
    "name": "Key Directional",
    "type": 0,
    "direction": [0.35, 0.8, 0.45],
    "position": [0.0, 5.0, 4.0],
    "color": [1.0, 0.92, 0.8],
    "intensity": 4.0,
    "range": 8.0,
    "width": 2.0,
    "height": 2.0
  }
]
```

旧场景没有 `lights` 字段时，加载器会保留默认 Directional Light，所以不需要手动修改过去的 JSON。

## 6. 目前的边界

- 只有第一盏 Directional Light 使用 Shadow Map；Point 和 Area 暂无阴影；
- Area Light 是矩形 4x4 分层采样近似，不是完整的 LTC 物理面光源积分；
- 光源编辑发生在 CPU Scene 层，每帧复制到对象常量缓冲；
- 默认 ImGui 字体不包含中文字符，因此本面板使用英文 UI 文本，避免出现 `????`。源代码教学注释仍保持中文。

这些边界是有意保留的：下一步可以在不改变编辑器结构的前提下，增加点光源阴影、LTC 或 Forward+，而不是重新设计场景数据。
