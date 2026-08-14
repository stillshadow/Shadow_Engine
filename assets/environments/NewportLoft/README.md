# Newport Loft HDRI

`newport_loft.hdr` 是 LearnOpenGL 的 PBR / IBL 示例所使用的环境贴图。

- LearnOpenGL 示例：[Specular IBL](https://learnopengl.com/PBR/IBL/Specular-IBL)
- 文件来源：[LearnOpenGL GitHub 资源](https://github.com/JoeyDeVries/LearnOpenGL/tree/master/resources/textures/hdr)
- 原始环境来源：HDRI Haven（现为 Poly Haven）
- 许可：HDRI Haven / Poly Haven 的 HDRI 以 CC0 发布；仓库中的代码仍遵循 LearnOpenGL 自身许可。

Shadow Engine 在启动时将这张经纬度 HDR 图转换为六面的 D3D12 TextureCube，供天空、
Forward PBR、Deferred PBR 与天空共同使用。
