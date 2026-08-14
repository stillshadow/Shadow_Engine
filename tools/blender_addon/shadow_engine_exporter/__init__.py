bl_info = {
    "name": "Shadow Engine Asset Tools",
    "author": "Shadow Engine",
    "version": (1, 0, 0),
    "blender": (4, 2, 0),
    "location": "View3D > Sidebar > Shadow Engine",
    "description": "Connect common PBR textures and export selected meshes to Shadow Engine as GLB",
    "category": "Import-Export",
}

from pathlib import Path
import re

import bpy
from bpy.props import StringProperty


IMAGE_EXTENSIONS = {".png", ".jpg", ".jpeg", ".tga", ".tif", ".tiff", ".exr"}
TEXTURE_TOKENS = {
    "base_color": ("basecolor", "base_color", "albedo", "diffuse", "color"),
    "normal": ("normal", "nor", "nrm"),
    "roughness": ("roughness", "rough"),
    "metallic": ("metallic", "metalness", "metal"),
}


def normalized_asset_name(value):
    """生成稳定的文件夹/文件名，避免 assetKey 因空格或特殊字符变得难读。"""
    cleaned = re.sub(r"[^A-Za-z0-9_-]+", "_", value.strip())
    return cleaned.strip("_")


def selected_meshes(context):
    return [obj for obj in context.selected_objects if obj.type == "MESH"]


def find_texture_files(directory):
    result = {}
    if not directory.exists():
        return result
    images = sorted(
        path for path in directory.rglob("*")
        if path.is_file() and path.suffix.lower() in IMAGE_EXTENSIONS)
    for role, tokens in TEXTURE_TOKENS.items():
        for path in images:
            compact_name = path.stem.lower().replace(" ", "_")
            if any(token in compact_name for token in tokens):
                result[role] = path
                break
    return result


def principled_input(node, name):
    socket = node.inputs.get(name)
    if socket is None:
        raise RuntimeError(f"当前 Blender 的 Principled BSDF 缺少输入：{name}")
    return socket


def create_image_node(nodes, image_path, label, non_color):
    image = bpy.data.images.load(str(image_path), check_existing=True)
    if non_color:
        image.colorspace_settings.name = "Non-Color"
    node = nodes.new("ShaderNodeTexImage")
    node.image = image
    node.label = label
    node.name = f"ShadowEngine_{label}"
    return node


class SHADOWENGINE_OT_setup_pbr(bpy.types.Operator):
    bl_idname = "shadow_engine.setup_pbr"
    bl_label = "自动连接 PBR 贴图"
    bl_description = "从资产 textures 文件夹识别并连接 BaseColor、Normal、Roughness、Metallic"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        meshes = selected_meshes(context)
        asset_name = normalized_asset_name(context.scene.shadow_engine_asset_name)
        texture_directory = Path(
            bpy.path.abspath(context.scene.shadow_engine_texture_directory)).resolve()
        if len(meshes) != 1:
            self.report({"ERROR"}, "自动贴图一次处理一个 Mesh；请选择需要设置材质的对象")
            return {"CANCELLED"}
        if not asset_name:
            self.report({"ERROR"}, "请填写 Asset Name")
            return {"CANCELLED"}

        textures = find_texture_files(texture_directory)
        if not textures:
            self.report({"ERROR"}, f"没有在 {texture_directory} 找到可识别贴图")
            return {"CANCELLED"}

        for obj in meshes:
            material = obj.active_material
            if material is None:
                material = bpy.data.materials.new(f"{asset_name}_Material")
                obj.data.materials.append(material)
            material.use_nodes = True
            nodes = material.node_tree.nodes
            links = material.node_tree.links
            principled = next(
                (node for node in nodes if node.type == "BSDF_PRINCIPLED"), None)
            if principled is None:
                principled = nodes.new("ShaderNodeBsdfPrincipled")

            if "base_color" in textures:
                node = create_image_node(
                    nodes, textures["base_color"], "Base Color", False)
                links.new(node.outputs["Color"], principled_input(principled, "Base Color"))
            if "roughness" in textures:
                node = create_image_node(
                    nodes, textures["roughness"], "Roughness", True)
                links.new(node.outputs["Color"], principled_input(principled, "Roughness"))
            if "metallic" in textures:
                node = create_image_node(
                    nodes, textures["metallic"], "Metallic", True)
                links.new(node.outputs["Color"], principled_input(principled, "Metallic"))
            if "normal" in textures:
                image_node = create_image_node(
                    nodes, textures["normal"], "Normal", True)
                normal_node = nodes.new("ShaderNodeNormalMap")
                normal_node.name = "ShadowEngine_NormalMap"
                links.new(image_node.outputs["Color"], normal_node.inputs["Color"])
                links.new(normal_node.outputs["Normal"], principled_input(principled, "Normal"))

        roles = ", ".join(sorted(textures.keys()))
        self.report({"INFO"}, f"已连接贴图：{roles}")
        return {"FINISHED"}


class SHADOWENGINE_OT_export_glb(bpy.types.Operator):
    bl_idname = "shadow_engine.export_glb"
    bl_label = "导出选中模型到 Shadow Engine"
    bl_description = "将选中 Mesh 与 Principled PBR 贴图导出为标准目录中的单个 GLB"

    def execute(self, context):
        meshes = selected_meshes(context)
        asset_name = normalized_asset_name(context.scene.shadow_engine_asset_name)
        repository = Path(bpy.path.abspath(context.scene.shadow_engine_repository)).resolve()
        if len(meshes) != 1:
            self.report({"ERROR"}, "当前工作流每个运行时资产请选择一个 Mesh 对象")
            return {"CANCELLED"}
        if not asset_name:
            self.report({"ERROR"}, "请填写 Asset Name")
            return {"CANCELLED"}
        if not (repository / "CMakeLists.txt").exists():
            self.report({"ERROR"}, "Repository Root 不是 Shadow Engine 仓库根目录")
            return {"CANCELLED"}

        warnings = []
        for obj in meshes:
            if not obj.data.uv_layers:
                warnings.append(f"{obj.name}: 没有 UV")
            if not obj.material_slots:
                warnings.append(f"{obj.name}: 没有材质")
            elif len(obj.material_slots) != 1:
                warnings.append(f"{obj.name}: 请只保留一个材质槽")
            if any(value <= 0.0 for value in obj.scale):
                warnings.append(f"{obj.name}: 当前引擎不支持负数或零 Scale")
            if any(abs(value) > 0.0001 for value in obj.location):
                warnings.append(f"{obj.name}: 请把 Location 归零")
            if any(abs(value) > 0.0001 for value in obj.rotation_euler):
                warnings.append(f"{obj.name}: 请 Ctrl+A 应用 Rotation")
            if any(abs(value - 1.0) > 0.0001 for value in obj.scale):
                warnings.append(f"{obj.name}: 请 Ctrl+A 应用 Scale")
        if warnings:
            self.report({"ERROR"}, "；".join(warnings[:4]))
            return {"CANCELLED"}

        output_directory = repository / "assets" / "models" / asset_name
        output_directory.mkdir(parents=True, exist_ok=True)
        output_path = output_directory / f"{asset_name}.glb"

        # Blender 版本之间少量 glTF Operator 参数名称会变化，只传当前版本实际暴露的属性。
        supported = set(bpy.ops.export_scene.gltf.get_rna_type().properties.keys())
        desired = {
            "filepath": str(output_path),
            "export_format": "GLB",
            "use_selection": True,
            "export_selected": True,
            "export_materials": "EXPORT",
            "export_texcoords": True,
            "export_normals": True,
            "export_tangents": True,
            "export_animations": False,
            "export_skins": False,
            "export_morph": False,
            "export_cameras": False,
            "export_lights": False,
        }
        arguments = {key: value for key, value in desired.items() if key in supported}
        result = bpy.ops.export_scene.gltf(**arguments)
        if "FINISHED" not in result:
            self.report({"ERROR"}, "Blender glTF Exporter 没有完成导出")
            return {"CANCELLED"}

        self.report({"INFO"}, f"已导出：{output_path}")
        return {"FINISHED"}


class SHADOWENGINE_PT_asset_tools(bpy.types.Panel):
    bl_label = "Shadow Engine Assets"
    bl_idname = "SHADOWENGINE_PT_asset_tools"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Shadow Engine"

    def draw(self, context):
        layout = self.layout
        layout.prop(context.scene, "shadow_engine_repository")
        layout.prop(context.scene, "shadow_engine_asset_name")
        layout.separator()
        layout.prop(context.scene, "shadow_engine_texture_directory")
        layout.operator("shadow_engine.setup_pbr", icon="MATERIAL")
        layout.separator()
        layout.label(text="选择需要导出的单个 Mesh")
        layout.operator("shadow_engine.export_glb", icon="EXPORT")


CLASSES = (
    SHADOWENGINE_OT_setup_pbr,
    SHADOWENGINE_OT_export_glb,
    SHADOWENGINE_PT_asset_tools,
)


def register():
    for cls in CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.Scene.shadow_engine_repository = StringProperty(
        name="Repository Root",
        description="Shadow Engine 仓库根目录",
        subtype="DIR_PATH",
    )
    bpy.types.Scene.shadow_engine_asset_name = StringProperty(
        name="Asset Name",
        description="输出文件夹与 GLB 的名称，建议使用英文、数字、下划线",
        default="RainAsset",
    )
    bpy.types.Scene.shadow_engine_texture_directory = StringProperty(
        name="Texture Folder",
        description="源贴图所在目录，可以位于任意 DCC 项目目录中",
        subtype="DIR_PATH",
    )


def unregister():
    del bpy.types.Scene.shadow_engine_texture_directory
    del bpy.types.Scene.shadow_engine_asset_name
    del bpy.types.Scene.shadow_engine_repository
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)


if __name__ == "__main__":
    register()
