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
# 短名别名 → 完整 UE 类名后缀（仅用于无法直接拼接的情况）
NODE_TYPE_ALIASES = {
    "Lerp": "LinearInterpolate",
}

# 需要特殊构造逻辑的类型（不能纯靠 set_editor_property）
SPECIAL_NODE_TYPES = {"LandscapeLayerBlend", "TextureSample", "TextureSampleParameter2D",
                      "Constant", "Constant3Vector", "Constant4Vector",
                      "ScalarParameter", "VectorParameter", "Custom"}

# set_editor_property 时跳过的保留字段
_RESERVED_FIELDS = {"Type", "Name", "Texture", "Layers", "Value"}


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

    asset_path = output_path + name
    mel = unreal.MaterialEditingLibrary

    # 检查材质是否已存在，存在则更新而不是重建（保留引用）
    mat = unreal.load_asset(asset_path)
    if mat and isinstance(mat, unreal.Material):
        _log(f"  材质已存在，删除后重建: {asset_path}")
        unreal.EditorAssetLibrary.delete_asset(asset_path)
        mat = None

    if not mat:
        asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
        factory = unreal.MaterialFactoryNew()
        mat = asset_tools.create_asset(name, output_path, unreal.Material, factory)
        if not mat:
            _log_error(f"创建材质失败: {name}")
            return False

    # 设置材质属性
    _apply_material_properties(mat, properties)

    # 清除所有材质输出引脚的连线（防止旧连线残留）
    for prop_name, prop_enum in MATERIAL_OUTPUTS.items():
        try:
            mel.clear_material_property(mat, _get_material_property(prop_name))
        except Exception:
            pass

    # 创建节点
    node_map = {}  # name → expression
    for idx, node_data in enumerate(nodes):
        expr = _create_node(mat, node_data, idx)
        if expr:
            node_name = node_data.get("Name", "")
            node_map[node_name] = expr
            _log(f"  node_map['{node_name}'] = {expr.get_class().get_name()} @ {id(expr)}")

    # 连接节点
    for conn in connections:
        _connect_nodes(mat, mel, node_map, conn)

    # 如果包含 LandscapeLayerBlend 节点，启用 Landscape 用途
    has_landscape = any(n.get("Type", "").startswith("Landscape") for n in nodes)
    if has_landscape:
        mat.set_editor_property("bUsedWithLandscape", True)
        _log("  启用 Used with Landscape")

    # 编译并保存
    mel.recompile_material(mat)
    mat.post_edit_change()
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

    tlm = properties.get("TranslucencyLightingMode", "")
    tlm_map = {
        "TLM_VolumetricNonDirectional": unreal.TranslucencyLightingMode.TLM_VOLUMETRIC_NON_DIRECTIONAL,
        "TLM_VolumetricDirectional": unreal.TranslucencyLightingMode.TLM_VOLUMETRIC_DIRECTIONAL,
        "TLM_VolumetricPerVertexNonDirectional": unreal.TranslucencyLightingMode.TLM_VOLUMETRIC_PER_VERTEX_NON_DIRECTIONAL,
        "TLM_VolumetricPerVertexDirectional": unreal.TranslucencyLightingMode.TLM_VOLUMETRIC_PER_VERTEX_DIRECTIONAL,
        "TLM_Surface": unreal.TranslucencyLightingMode.TLM_SURFACE,
        "TLM_SurfacePerPixelLighting": unreal.TranslucencyLightingMode.TLM_SURFACE_PER_PIXEL_LIGHTING,
    }
    if tlm in tlm_map:
        mat.set_editor_property("TranslucencyLightingMode", tlm_map[tlm])
        _log(f"  TranslucencyLightingMode: {tlm}")


def _create_node(mat, node_data, index=0):
    """创建材质节点（动态反射模式）"""
    node_type = node_data.get("Type", "")
    node_name = node_data.get("Name", "")

    # 动态解析 UE 类名：MaterialExpression{Type}
    resolved_type = NODE_TYPE_ALIASES.get(node_type, node_type)
    ue_class_name = f"MaterialExpression{resolved_type}"

    mel = unreal.MaterialEditingLibrary

    # 按节点类型分区域排列，从左到右流向材质输出
    # 纹理采样节点放中间偏左，UV/Panner放最左，Custom放中间
    _NODE_POSITIONS = {}  # 由 JSON 模板的 "Position" 字段或自动计算

    node_pos = node_data.get("Position", None)
    if node_pos:
        pos_x = node_pos[0]
        pos_y = node_pos[1]
    else:
        # 自动布局：按类型分列
        if node_type in ("TextureCoordinate",):
            pos_x = -1200
            pos_y = -200 + index * 200
        elif node_type in ("Panner",):
            pos_x = -900
            pos_y = -200 + index * 200
        elif node_type in ("TextureSample", "TextureSampleParameter2D"):
            pos_x = -600
            pos_y = -200 + index * 200
        elif node_type in ("Custom",):
            pos_x = -300
            pos_y = 0
        else:
            col = index % 2
            row = index // 2
            pos_x = -600 - col * 400
            pos_y = -300 + row * 250

    # 动态加载类
    expr_class = getattr(unreal, ue_class_name, None)
    if not expr_class:
        try:
            expr_class = unreal.load_class(None, f"/Script/Engine.{ue_class_name}")
        except Exception:
            pass
    if not expr_class:
        _log(f"  找不到节点类: {ue_class_name}，跳过 {node_name}")
        return None

    expr = mel.create_material_expression(mat, expr_class, pos_x, pos_y)
    if not expr:
        _log(f"  创建节点失败: {node_name} ({node_type})")
        return None

    # === 特殊类型处理（需要非标准属性设置） ===
    if node_type in ("TextureSample", "TextureSampleParameter2D"):
        tex_path = node_data.get("Texture", "")
        if tex_path:
            tex = unreal.load_asset(tex_path)
            if not tex:
                base_name = tex_path.rsplit("/", 1)[-1] if "/" in tex_path else tex_path
                tex = unreal.load_asset(f"{tex_path}.{base_name}")
            if tex:
                expr.set_editor_property("Texture", tex)
                _log(f"  纹理已设置: {node_name} = {tex_path}")
        if node_type == "TextureSampleParameter2D":
            expr.set_editor_property("ParameterName", node_name)
            # 设置 SamplerType（Normal 贴图需要 SAMPLERTYPE_NORMAL）
            sampler_type = node_data.get("SamplerType", "")
            if sampler_type == "Normal":
                expr.set_editor_property("SamplerType", unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL)

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

    elif node_type == "Custom":
        code = node_data.get("Code", "return 0;")
        expr.set_editor_property("Code", code)
        expr.set_editor_property("Description", node_name)
        # OutputType: try enum, fallback to int
        output_type_str = str(node_data.get("OutputType", "float3")).lower()
        ot_int = {"float": 0, "float1": 0, "float2": 1, "float3": 2, "float4": 3}.get(output_type_str, 2)
        try:
            ot_enum = getattr(unreal.CustomMaterialOutputType, ["CMOT_FLOAT1","CMOT_FLOAT2","CMOT_FLOAT3","CMOT_FLOAT4"][ot_int])
            expr.set_editor_property("OutputType", ot_enum)
        except Exception:
            try:
                expr.set_editor_property("OutputType", ot_int)
            except Exception as e2:
                _log(f"  OutputType 设置失败: {e2}")
        # Additional outputs
        additional_outputs = node_data.get("AdditionalOutputs", [])
        if additional_outputs:
            ao_array = []
            for ao in additional_outputs:
                co = unreal.CustomOutput()
                co.set_editor_property("OutputName", ao.get("Name", ""))
                ao_str = str(ao.get("Type", "float3")).lower()
                ao_int = {"float": 0, "float1": 0, "float2": 1, "float3": 2, "float4": 3}.get(ao_str, 2)
                try:
                    ao_enum = getattr(unreal.CustomMaterialOutputType, ["CMOT_FLOAT1","CMOT_FLOAT2","CMOT_FLOAT3","CMOT_FLOAT4"][ao_int])
                    co.set_editor_property("OutputType", ao_enum)
                except Exception:
                    try:
                        co.set_editor_property("OutputType", ao_int)
                    except Exception:
                        pass
                ao_array.append(co)
            expr.set_editor_property("AdditionalOutputs", ao_array)
        # Inputs
        inputs = node_data.get("Inputs", [])
        if inputs:
            input_array = []
            for inp in inputs:
                ci = unreal.CustomInput()
                ci.set_editor_property("InputName", inp.get("Name", ""))
                input_array.append(ci)
            expr.set_editor_property("Inputs", input_array)
        _log(f"  Custom 节点: {node_name}, {len(inputs)} 输入, code={len(code)} chars")

    elif node_type == "LandscapeLayerBlend":
        layers = node_data.get("Layers", [])
        if layers:
            layer_infos = []
            for layer_data in layers:
                layer_info = unreal.LayerBlendInput()
                layer_info.set_editor_property("layer_name", layer_data.get("LayerName", ""))
                blend_type_str = layer_data.get("BlendType", "LB_WeightBlend")
                if blend_type_str == "LB_HeightBlend":
                    layer_info.set_editor_property("blend_type", unreal.LandscapeLayerBlendType.LB_HEIGHT_BLEND)
                elif blend_type_str == "LB_AlphaBlend":
                    layer_info.set_editor_property("blend_type", unreal.LandscapeLayerBlendType.LB_ALPHA_BLEND)
                else:
                    layer_info.set_editor_property("blend_type", unreal.LandscapeLayerBlendType.LB_WEIGHT_BLEND)
                layer_info.set_editor_property("preview_weight", layer_data.get("PreviewWeight", 0.0))
                layer_infos.append(layer_info)
            expr.set_editor_property("Layers", layer_infos)
            _log(f"  LandscapeLayerBlend: {len(layer_infos)} 层")

    # === 通用属性设置：JSON 里非保留字段自动 set_editor_property ===
    if node_type not in SPECIAL_NODE_TYPES:
        for key, val in node_data.items():
            if key in _RESERVED_FIELDS:
                continue
            try:
                if isinstance(val, bool):
                    expr.set_editor_property(key, val)
                elif isinstance(val, (int, float)):
                    expr.set_editor_property(key, float(val))
                else:
                    expr.set_editor_property(key, val)
            except Exception:
                # 属性可能是 protected 或不存在，静默跳过
                _log(f"  属性跳过（protected/不存在）: {node_name}.{key}")

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

        mat_prop = _get_material_property(to_pin)
        _log(f"  连线材质输出: {from_node_name}({from_expr.get_class().get_name()}) → Material.{to_pin} (prop={mat_prop})")

        try:
            # connect_material_property(from_expression, output_name, property)
            # output_name 对于单输出节点用空字符串
            result = mel.connect_material_property(from_expr, from_pin if from_pin else "", mat_prop)
            if result:
                _log(f"  连线成功: {from_str} → Material.{to_pin}")
            else:
                _log(f"  连线返回 False: {from_str} → Material.{to_pin} (prop={mat_prop})")
        except Exception as e:
            _log(f"  连线异常: {from_str} → Material.{to_pin}: {e}")
            # 尝试不带 output_name
            try:
                result = mel.connect_material_property(from_expr, "", mat_prop)
                if result:
                    _log(f"  连线成功(空pin): {from_str} → Material.{to_pin}")
            except Exception as e2:
                _log(f"  连线再次失败: {e2}")
        return

    # 节点之间连接
    from_expr = node_map.get(from_node_name)
    to_expr = node_map.get(to_node_name)
    if not from_expr or not to_expr:
        _log(f"  连线失败: 找不到节点 {from_node_name} 或 {to_node_name}")
        return

    # LandscapeLayerBlend input pins: try multiple name formats
    actual_to_pin = to_pin
    is_layer_blend = False
    if to_expr and hasattr(to_expr, 'get_class'):
        class_name = str(to_expr.get_class().get_name())
        if 'LandscapeLayerBlend' in class_name:
            is_layer_blend = True

    if is_layer_blend and to_pin:
        # Debug: list all input pins
        try:
            inputs = to_expr.get_editor_property("Layers")
            _log(f"  LayerBlend 层数: {len(inputs)}")
            for i, layer in enumerate(inputs):
                ln = layer.get_editor_property("layer_name")
                _log(f"  LayerBlend 层[{i}]: {ln}")
        except Exception as dbg_e:
            _log(f"  LayerBlend 调试失败: {dbg_e}")

        actual_from_pin = from_pin if from_pin else ""
        pin_formats = [
            f"Layer {to_pin}",
            to_pin,
            f"Layer_{to_pin}",
            f"Layer {to_pin} ",
        ]
        connected = False
        for pin_name in pin_formats:
            try:
                result = mel.connect_material_expressions(from_expr, actual_from_pin, to_expr, pin_name)
                if result:
                    _log(f"  连线成功(真): {from_str} → {to_node_name}.{pin_name}")
                    connected = True
                    break
                else:
                    _log(f"  连线返回False: {from_str} → {to_node_name}.{pin_name}")
            except Exception as ce:
                _log(f"  连线异常: {pin_name}: {ce}")
                continue
        if not connected:
            _log(f"  连线全部失败: {from_str} → {to_node_name}.{to_pin}")
        return

    output_index = _get_output_index(from_pin)
    input_index = _get_input_index(actual_to_pin)

    try:
        result = mel.connect_material_expressions(from_expr, from_pin, to_expr, actual_to_pin)
        if result:
            _log(f"  连线OK: {from_str} → {to_node_name}.{actual_to_pin}")
        else:
            _log(f"  连线返回False: {from_str} → {to_node_name}.{actual_to_pin}")
    except Exception as e:
        _log(f"  连线失败: {from_str} → {to_node_name}.{actual_to_pin}: {e}")


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
        "WorldPositionOffset": unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET,
        "Refraction": unreal.MaterialProperty.MP_REFRACTION,
    }
    return prop_map.get(pin_name, unreal.MaterialProperty.MP_BASE_COLOR)


# ===================================================================
# 材质实例生成（原有功能）
# ===================================================================

def _generate_material_instance(template):
    """生成材质实例"""
    name = template.get("Name", "MI_Generated")
    parent_path = template.get("ParentMaterial", template.get("Parent", "/Engine/EngineMaterials/DefaultMaterial"))
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

    asset_path = output_path + name

    # 检查是否已存在，存在则更新
    mi = unreal.load_asset(asset_path)
    if mi and isinstance(mi, unreal.MaterialInstanceConstant):
        _log(f"  材质实例已存在，更新模式: {asset_path}")
    else:
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
