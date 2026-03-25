"""BlueprintFactory - 材质生成器
从 JSON 模板生成 Material Instance。

JSON 模板格式:
{
    "Name": "MI_Ground_Farm",
    "Parent": "/Game/Art/Materials/M_Landscape_Master",
    "OutputPath": "/Game/Art/Materials/Landscape/",
    "Textures": {
        "BaseColor": "/Game/Art/Textures/Landscape/T_Ground_Farm_D",
        "Normal": "/Game/Art/Textures/Landscape/T_Ground_Farm_N"
    },
    "Parameters": {
        "Roughness": 0.8,
        "Tiling": 4.0
    }
}
"""
import json
import os

try:
    import unreal
    IN_UE = True
except ImportError:
    IN_UE = False


def _log(msg):
    if IN_UE:
        unreal.log(f"[MatFactory] {msg}")
    else:
        print(f"[MatFactory] {msg}")


def _log_error(msg):
    if IN_UE:
        unreal.log_error(f"[MatFactory] {msg}")
    else:
        print(f"[MatFactory ERROR] {msg}")


def generate_material(json_path: str):
    """从 JSON 模板生成材质实例"""
    if not os.path.isfile(json_path):
        _log_error(f"JSON 文件不存在: {json_path}")
        return False

    with open(json_path, "r", encoding="utf-8") as f:
        template = json.load(f)

    name = template.get("Name", "MI_Generated")
    parent_path = template.get("Parent", "/Engine/EngineMaterials/DefaultMaterial")
    output_path = template.get("OutputPath", "/Game/Art/Materials/Generated/")
    textures = template.get("Textures", {})
    parameters = template.get("Parameters", {})

    if not IN_UE:
        _log(f"非 UE 环境，跳过生成: {name}")
        return False

    _log(f"生成材质: {name}")

    # 加载父材质
    parent_mat = unreal.load_asset(parent_path)
    if not parent_mat:
        _log_error(f"无法加载父材质: {parent_path}")
        return False

    # 创建材质实例
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    factory = unreal.MaterialInstanceConstantFactoryNew()
    mi = asset_tools.create_asset(name, output_path, unreal.MaterialInstanceConstant, factory)
    if not mi:
        _log_error(f"创建材质实例失败: {name}")
        return False

    # 设置父材质
    mi.set_editor_property("Parent", parent_mat)

    # 设置纹理参数
    mel = unreal.MaterialEditingLibrary
    for param_name, tex_path in textures.items():
        tex = unreal.load_asset(tex_path)
        if tex:
            mel.set_material_instance_texture_parameter_value(mi, param_name, tex)
            _log(f"  纹理参数: {param_name} = {tex_path}")
        else:
            _log(f"  纹理加载失败: {tex_path}")

    # 设置标量参数
    for param_name, value in parameters.items():
        if isinstance(value, (int, float)):
            mel.set_material_instance_scalar_parameter_value(mi, param_name, float(value))
            _log(f"  标量参数: {param_name} = {value}")
        elif isinstance(value, list) and len(value) >= 3:
            color = unreal.LinearColor(value[0], value[1], value[2], value[3] if len(value) > 3 else 1.0)
            mel.set_material_instance_vector_parameter_value(mi, param_name, color)
            _log(f"  向量参数: {param_name} = {value}")

    # 保存
    asset_path = output_path + name
    unreal.EditorAssetLibrary.save_asset(asset_path)
    _log(f"材质生成完成: {asset_path}")
    return True
