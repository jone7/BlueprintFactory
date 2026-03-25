"""BlueprintFactory - 材质生成器
支持两种模式：
1. Type="Material" — 生成母材质（含节点图）
2. Type="MaterialInstance" 或无 Type — 生成材质实例

母材质 JSON 模板:
{
    "Name": "M_Landscape_Master",
    "Type": "Material",
    "OutputPath": "/Game/Art/Materials/",
    "Nodes": [
        {"Type": "TextureSample", "Name": "DiffuseTex", "Texture": "/Game/Art/Textures/T_Grass_D"},
        {"Type": "TextureSample", "Name": "NormalTex", "Texture": "/Game/Art/Textures/T_Grass_N"},
        {"Type": "Constant", "Name": "RoughnessVal", "Value": 0.8},
        {"Type": "Constant3Vector", "Name": "TintColor", "Value": [0.5, 0.8, 0.3]},
        {"Type": "TextureCoordinate", "Name": "UV", "UTiling": 4.0, "VTiling": 4.0},
        {"Type": "Multiply", "Name": "TiledDiffuse"}
    ],
    "Connections": [
        {"From": "UV", "To": "DiffuseTex.UVs"},
        {"From": "DiffuseTex.RGB", "To": "Material.BaseColor"},
        {"From": "NormalTex.RGB", "To": "Material.Normal"},
        {"From": "RoughnessVal", "To": "Material.Roughness"}
    ],
    "Properties": {
        "TwoSided": false,
        "BlendMode": "Opaque",
        "ShadingModel": "DefaultLit"
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


# ===================================================================
# 入口：根据 Type 分发
# ===================================================================

def generate_material(json_path: str):
    """从 JSON 模板生成材质（自动判断母材质或材质实例）"""
    if not os.path.isfile(json_path):
        _log_error(f"JSON 文件不存在: {json_path}")
        return False

    with open(json_path, "r", encoding="utf-8") as f:
        template = json.load(f)

    mat_type = template.get("Type", "MaterialInstance")

    if mat_type == "Material":
        return _generate_master_material(template)
    else:
        return _generate_material_instance(template)


# ===================================================================
# 母材质生成（含节点图）
# ===================================================================

# 材质输出引脚名 → UE 属性映射
MATERIAL_OUTPUTS = {
    "BaseColor": "MP_BaseColor",
    "Metallic": "MP_Metallic",
    "Specular": "MP_Specular",
    "Roughness": "MP_Roughness",
    "Normal": "MP_Normal",
    "EmissiveColor": "MP_EmissiveColor",
    "Opacity": "MP_Opacity",
    "OpacityMask": "MP_OpacityMask",
    "AmbientOcclusion": "MP_AmbientOcclusion",
}

# 节点类型 → UE Expression 类名
NODE_TYPE_MAP = {
    "TextureSample": "MaterialExpressionTextureSample",
    "TextureCoordinate": "MaterialExpressionTextureCoordinate",
    "Constant": "MaterialExpressionConstant",
    "Constant2Vector": "MaterialExpressionConstant2Vector",
    "Constant3Vector": "MaterialExpressionConstant3Vector",
    "Constant4Vector": "MaterialExpressionConstant4Vector",
    "Multiply": "MaterialExpressionMultiply",
    "Add": "MaterialExpressionAdd",
    "Subtract": "MaterialExpressionSubtract",
    "Divide": "MaterialExpressionDivide",
    "Lerp": "MaterialExpressionLinearInterpolate",
    "Power": "MaterialExpressionPower",
    "Clamp": "MaterialExpressionClamp",
    "OneMinus": "MaterialExpressionOneMinus",
    "Abs": "MaterialExpressionAbs",
    "Fresnel": "MaterialExpressionFresnel",
    "Panner": "MaterialExpressionPanner",
    "WorldPosition": "MaterialExpressionWorldPosition",
    "VertexColor": "MaterialExpressionVertexColor",
    "Time": "MaterialExpressionTime",
    "ScalarParameter": "MaterialExpressionScalarParameter",
    "VectorParameter": "MaterialExpressionVectorParameter",
    "TextureSampleParameter2D": "MaterialExpressionTextureSampleParameter2D",
}


def _generate_master_material(template):
    """生成母材质（含节点图）"""
    name = template.get("Name", "M_Generated")
    output_path = template.get("OutputPath", "/Game/Art/Materials/")
    nodes = template.get("Nodes", [])
    connections = template.get("Connections", [])
    properties = template.get("Properties", {})

    if not IN_UE:
        _log(f"非 UE 环境，跳过母材质生成: {name}")
        return False

    _log(f"生成母材质: {name}")

    # 创建材质资产
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    factory = unreal.MaterialFactoryNew()
    mat = asset_tools.create_asset(name, output_path, unreal.Material, factory)
    if not mat:
        _log_error(f"创建材质失败: {name}")
        return False

    mel = unreal.MaterialEditingLibrary

    # 设置材质属性
    _apply_material_properties(mat, properties)

    # 创建节点
    node_map = {}  # name → expression
    for node_data in nodes:
        expr = _create_node(mat, node_data)
        if expr:
            node_name = node_data.get("Name", "")
            node_map[node_name] = expr

    # 连接节点
    for conn in connections:
        _connect_nodes(mat, mel, node_map, conn)

    # 编译并保存
    mel.recompile_material(mat)
    asset_path = output_path + name
    unreal.EditorAssetLibrary.save_asset(asset_path)

    _log(f"母材质生成完成: {asset_path} ({len(nodes)} 个节点, {len(connections)} 条连线)")
    return True


def _apply_material_properties(mat, properties):
    """设置材质属性"""
    if not properties:
        return

    if "TwoSided" in properties:
        mat.set_editor_property("TwoSided", properties["TwoSided"])

    blend_mode = properties.get("BlendMode", "Opaque")
    blend_map = {
        "Opaque": unreal.BlendMode.BLEND_OPAQUE,
        "Masked": unreal.BlendMode.BLEND_MASKED,
        "Translucent": unreal.BlendMode.BLEND_TRANSLUCENT,
        "Additive": unreal.BlendMode.BLEND_ADDITIVE,
    }
    if blend_mode in blend_map:
        mat.set_editor_property("BlendMode", blend_map[blend_mode])

    shading = properties.get("ShadingModel", "DefaultLit")
    shading_map = {
        "DefaultLit": unreal.MaterialShadingModel.MSM_DEFAULT_LIT,
        "Unlit": unreal.MaterialShadingModel.MSM_UNLIT,
        "Subsurface": unreal.MaterialShadingModel.MSM_SUBSURFACE,
    }
    if shading in shading_map:
        mat.set_editor_property("ShadingModel", shading_map[shading])


def _create_node(mat, node_data):
    """创建材质节点"""
    node_type = node_data.get("Type", "")
    node_name = node_data.get("Name", "")
    ue_class = NODE_TYPE_MAP.get(node_type)

    if not ue_class:
        _log(f"  未知节点类型: {node_type}，跳过")
        return None

    mel = unreal.MaterialEditingLibrary
    expr = mel.create_material_expression(mat, getattr(unreal, ue_class, None) or unreal.load_class(None, f"/Script/Engine.{ue_class}"), -300, 0)

    if not expr:
        # 回退：用字符串方式创建
        try:
            expr = mel.create_material_expression(mat, unreal.load_class(None, f"/Script/Engine.{ue_class}"), -300, 0)
        except Exception:
            _log(f"  创建节点失败: {node_name} ({node_type})")
            return None

    if not expr:
        _log(f"  创建节点失败: {node_name} ({node_type})")
        return None

    # 设置节点属性
    if node_type == "TextureSample" or node_type == "TextureSampleParameter2D":
        tex_path = node_data.get("Texture", "")
        if tex_path:
            tex = unreal.load_asset(tex_path)
            if tex:
                expr.set_editor_property("Texture", tex)
        if node_type == "TextureSampleParameter2D":
            expr.set_editor_property("ParameterName", node_name)

    elif node_type == "TextureCoordinate":
        u_tiling = node_data.get("UTiling", 1.0)
        v_tiling = node_data.get("VTiling", 1.0)
        expr.set_editor_property("UTiling", u_tiling)
        expr.set_editor_property("VTiling", v_tiling)

    elif node_type == "Constant":
        expr.set_editor_property("R", float(node_data.get("Value", 0)))

    elif node_type == "Constant3Vector":
        val = node_data.get("Value", [0, 0, 0])
        expr.set_editor_property("Constant", unreal.LinearColor(val[0], val[1], val[2], 1.0))

    elif node_type == "Constant4Vector":
        val = node_data.get("Value", [0, 0, 0, 1])
        expr.set_editor_property("Constant", unreal.LinearColor(val[0], val[1], val[2], val[3]))

    elif node_type == "ScalarParameter":
        expr.set_editor_property("ParameterName", node_name)
        expr.set_editor_property("DefaultValue", float(node_data.get("Value", 0)))

    elif node_type == "VectorParameter":
        expr.set_editor_property("ParameterName", node_name)
        val = node_data.get("Value", [0, 0, 0, 1])
        expr.set_editor_property("DefaultValue", unreal.LinearColor(val[0], val[1], val[2], val[3] if len(val) > 3 else 1.0))

    _log(f"  节点: {node_name} ({node_type})")
    return expr


def _connect_nodes(mat, mel, node_map, conn):
    """连接两个节点"""
    from_str = conn.get("From", "")
    to_str = conn.get("To", "")

    if not from_str or not to_str:
        return

    # 解析 "NodeName.OutputPin" 格式
    from_parts = from_str.split(".")
    to_parts = to_str.split(".")

    from_node_name = from_parts[0]
    from_pin = from_parts[1] if len(from_parts) > 1 else ""

    to_node_name = to_parts[0]
    to_pin = to_parts[1] if len(to_parts) > 1 else ""

    # 连接到材质输出
    if to_node_name == "Material":
        from_expr = node_map.get(from_node_name)
        if not from_expr:
            _log(f"  连线失败: 找不到源节点 {from_node_name}")
            return

        # 确定输出引脚索引
        output_index = _get_output_index(from_pin)

        try:
            mel.connect_material_property(from_expr, to_pin, _get_material_property(to_pin))
            _log(f"  连线: {from_str} → {to_str}")
        except Exception as e:
            _log(f"  连线失败: {from_str} → {to_str}: {e}")
        return

    # 节点之间连接
    from_expr = node_map.get(from_node_name)
    to_expr = node_map.get(to_node_name)
    if not from_expr or not to_expr:
        _log(f"  连线失败: 找不到节点 {from_node_name} 或 {to_node_name}")
        return

    output_index = _get_output_index(from_pin)
    input_index = _get_input_index(to_pin)

    try:
        mel.connect_material_expressions(from_expr, from_pin, to_expr, to_pin)
        _log(f"  连线: {from_str} → {to_str}")
    except Exception as e:
        _log(f"  连线失败: {from_str} → {to_str}: {e}")


def _get_output_index(pin_name):
    """输出引脚名 → 索引"""
    pin_map = {"": 0, "RGB": 0, "R": 1, "G": 2, "B": 3, "A": 4}
    return pin_map.get(pin_name, 0)


def _get_input_index(pin_name):
    """输入引脚名 → 索引"""
    pin_map = {"": 0, "A": 0, "B": 1, "UVs": 0, "Alpha": 2}
    return pin_map.get(pin_name, 0)


def _get_material_property(pin_name):
    """材质输出引脚名 → MaterialProperty 枚举"""
    prop_map = {
        "BaseColor": unreal.MaterialProperty.MP_BASE_COLOR,
        "Metallic": unreal.MaterialProperty.MP_METALLIC,
        "Specular": unreal.MaterialProperty.MP_SPECULAR,
        "Roughness": unreal.MaterialProperty.MP_ROUGHNESS,
        "Normal": unreal.MaterialProperty.MP_NORMAL,
        "EmissiveColor": unreal.MaterialProperty.MP_EMISSIVE_COLOR,
        "Opacity": unreal.MaterialProperty.MP_OPACITY,
        "OpacityMask": unreal.MaterialProperty.MP_OPACITY_MASK,
        "AmbientOcclusion": unreal.MaterialProperty.MP_AMBIENT_OCCLUSION,
    }
    return prop_map.get(pin_name, unreal.MaterialProperty.MP_BASE_COLOR)


# ===================================================================
# 材质实例生成（原有功能）
# ===================================================================

def _generate_material_instance(template):
    """生成材质实例"""
    name = template.get("Name", "MI_Generated")
    parent_path = template.get("Parent", "/Engine/EngineMaterials/DefaultMaterial")
    output_path = template.get("OutputPath", "/Game/Art/Materials/Generated/")
    textures = template.get("Textures", {})
    parameters = template.get("Parameters", {})

    if not IN_UE:
        _log(f"非 UE 环境，跳过生成: {name}")
        return False

    _log(f"生成材质实例: {name}")

    parent_mat = unreal.load_asset(parent_path)
    if not parent_mat:
        _log_error(f"无法加载父材质: {parent_path}")
        return False

    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    factory = unreal.MaterialInstanceConstantFactoryNew()
    mi = asset_tools.create_asset(name, output_path, unreal.MaterialInstanceConstant, factory)
    if not mi:
        _log_error(f"创建材质实例失败: {name}")
        return False

    mi.set_editor_property("Parent", parent_mat)

    mel = unreal.MaterialEditingLibrary
    for param_name, tex_path in textures.items():
        tex = unreal.load_asset(tex_path)
        if tex:
            mel.set_material_instance_texture_parameter_value(mi, param_name, tex)
            _log(f"  纹理参数: {param_name} = {tex_path}")

    for param_name, value in parameters.items():
        if isinstance(value, (int, float)):
            mel.set_material_instance_scalar_parameter_value(mi, param_name, float(value))
            _log(f"  标量参数: {param_name} = {value}")
        elif isinstance(value, list) and len(value) >= 3:
            color = unreal.LinearColor(value[0], value[1], value[2], value[3] if len(value) > 3 else 1.0)
            mel.set_material_instance_vector_parameter_value(mi, param_name, color)
            _log(f"  向量参数: {param_name} = {value}")

    asset_path = output_path + name
    unreal.EditorAssetLibrary.save_asset(asset_path)
    _log(f"材质实例生成完成: {asset_path}")
    return True


# ===================================================================
# 反向导出
# ===================================================================

def export_material(asset_path: str, json_path: str):
    """将已有材质或材质实例导出为 JSON 模板"""
    if not IN_UE:
        _log("非 UE 环境，无法导出")
        return False

    asset = unreal.load_asset(asset_path)
    if not asset:
        _log_error(f"无法加载: {asset_path}")
        return False

    if isinstance(asset, unreal.MaterialInstanceConstant):
        return _export_material_instance(asset, asset_path, json_path)
    elif isinstance(asset, unreal.Material):
        return _export_master_material(asset, asset_path, json_path)
    else:
        _log_error(f"不支持的资产类型: {type(asset)}")
        return False


def _export_material_instance(mi, asset_path, json_path):
    """导出材质实例"""
    template = {
        "Name": mi.get_name(),
        "Type": "MaterialInstance",
        "Parent": "",
        "OutputPath": str(asset_path).rsplit("/", 1)[0] + "/",
        "Textures": {},
        "Parameters": {},
    }

    parent = mi.get_editor_property("Parent")
    if parent:
        template["Parent"] = parent.get_path_name()

    mel = unreal.MaterialEditingLibrary
    try:
        tex_params = mel.get_texture_parameter_names(mi)
        for p in tex_params:
            tex = mel.get_material_instance_texture_parameter_value(mi, p)
            if tex:
                template["Textures"][str(p)] = tex.get_path_name()
    except Exception:
        pass

    try:
        scalar_params = mel.get_scalar_parameter_names(mi)
        for p in scalar_params:
            template["Parameters"][str(p)] = round(mel.get_material_instance_scalar_parameter_value(mi, p), 4)
    except Exception:
        pass

    os.makedirs(os.path.dirname(json_path), exist_ok=True)
    with open(json_path, "w", encoding="utf-8") as f:
        json.dump(template, f, indent=2, ensure_ascii=False)

    _log(f"材质实例导出完成: {json_path}")
    return True


def _export_master_material(mat, asset_path, json_path):
    """导出母材质（节点图）"""
    mel = unreal.MaterialEditingLibrary
    expressions = mel.get_material_expressions(mat)

    template = {
        "Name": mat.get_name(),
        "Type": "Material",
        "OutputPath": str(asset_path).rsplit("/", 1)[0] + "/",
        "Nodes": [],
        "Connections": [],
        "Properties": {},
    }

    # 属性
    template["Properties"]["TwoSided"] = mat.get_editor_property("TwoSided")

    # 节点
    for expr in expressions:
        node_data = {
            "Type": expr.get_class().get_name().replace("MaterialExpression", ""),
            "Name": expr.get_name(),
        }

        # 提取常见属性
        if hasattr(expr, "Texture"):
            tex = expr.get_editor_property("Texture")
            if tex:
                node_data["Texture"] = tex.get_path_name()

        if hasattr(expr, "R"):
            try:
                node_data["Value"] = expr.get_editor_property("R")
            except Exception:
                pass

        if hasattr(expr, "ParameterName"):
            try:
                node_data["Name"] = str(expr.get_editor_property("ParameterName"))
            except Exception:
                pass

        template["Nodes"].append(node_data)

    os.makedirs(os.path.dirname(json_path), exist_ok=True)
    with open(json_path, "w", encoding="utf-8") as f:
        json.dump(template, f, indent=2, ensure_ascii=False)

    _log(f"母材质导出完成: {json_path} ({len(template['Nodes'])} 个节点)")
    return True
