"""BlueprintFactory - 材质生成器
支持材质、材质实例、公共材质函数及参数集合：
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

try:
    from .editor_guard import ensure_editor_not_playing_for_existing_asset
except ImportError:
    from editor_guard import ensure_editor_not_playing_for_existing_asset


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
    """按 JSON 声明生成材质资源；未知类型直接拒绝，避免误生成实例。"""
    if not os.path.isfile(json_path):
        _log_error(f"JSON 文件不存在: {json_path}")
        return False

    with open(json_path, "r", encoding="utf-8") as f:
        template = json.load(f)

    mat_type = template.get("Type", "MaterialInstance")

    if mat_type == "Material":
        if "SurfaceExtension" in template:
            return _generate_material_surface_extension(template)
        return _generate_master_material(template)
    if mat_type == "MaterialFunction":
        return _generate_material_function(template)
    if mat_type == "MaterialParameterCollection":
        return _generate_parameter_collection(template)
    if mat_type == "MaterialInstance":
        return _generate_material_instance(template)
    _log_error(f"不支持的材质类型: {mat_type}")
    return False


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
                      "ScalarParameter", "VectorParameter", "Custom",
                      "MaterialFunctionCall", "CollectionParameter", "FunctionInput", "FunctionOutput",
                      "StaticSwitchParameter"}

# set_editor_property 时跳过的保留字段
_RESERVED_FIELDS = {"Type", "Name", "Texture", "Layers", "Value", "Position"}

def _validate_graph(template):
    """在清理已有图之前校验节点、连线和外部资源，缺失依赖直接失败。"""
    names = set()
    for node in template["Nodes"]:
        name = node["Name"]
        if not name or "." in name or name in names or name == "Material":
            raise ValueError(f"节点名无效或重复: {name}")
        names.add(name)
        node_type = NODE_TYPE_ALIASES.get(node["Type"], node["Type"])
        if not getattr(unreal, "MaterialExpression" + node_type, None):
            raise ValueError(f"节点类型不存在: {node_type}")
        if node_type == "MaterialFunctionCall":
            dependency = unreal.load_asset(node["MaterialFunction"])
            if not isinstance(dependency, unreal.MaterialFunctionInterface):
                raise ValueError(f"节点 {name} 的公共函数不存在或类型不符")
        if node_type == "CollectionParameter":
            dependency = unreal.load_asset(node["Collection"])
            if not isinstance(dependency, unreal.MaterialParameterCollection):
                raise ValueError(f"节点 {name} 的参数集合不存在或类型不符")
            parameters = list(dependency.get_editor_property("ScalarParameters")) + list(dependency.get_editor_property("VectorParameters"))
            if node["ParameterName"] not in {str(p.get_editor_property("parameter_name")) for p in parameters}:
                raise ValueError(f"节点 {name} 引用不存在的集合参数")
    for connection in template["Connections"]:
        source = connection["From"].split(".", 1)[0]
        target = connection["To"].split(".", 1)[0]
        if source not in names or (target not in names and target != "Material"):
            raise ValueError(f"连线引用不存在的节点: {connection}")


def _generate_material_function(template):
    """原位生成公共函数，保留函数资源身份及同名接口的 GUID。"""
    if not IN_UE:
        return False
    _validate_graph(template)
    name, folder = template["Name"], template["OutputPath"].rstrip("/")
    path = folder + "/" + name
    if not ensure_editor_not_playing_for_existing_asset(path):
        return False
    function = unreal.load_asset(path)
    ports = {}
    wanted_ports = {(data["Type"], data["Name"]) for data in template["Nodes"]
                    if data["Type"] in ("FunctionInput", "FunctionOutput")}
    if function:
        if not isinstance(function, unreal.MaterialFunction):
            raise TypeError(f"目标不是材质函数: {path}")
        for expression in unreal.MaterialEditingLibrary.get_material_function_expressions(function):
            key = None
            if isinstance(expression, unreal.MaterialExpressionFunctionInput):
                key = ("FunctionInput", str(expression.get_editor_property("InputName")))
            elif isinstance(expression, unreal.MaterialExpressionFunctionOutput):
                key = ("FunctionOutput", str(expression.get_editor_property("OutputName")))
            if key in wanted_ports:
                if key in ports:
                    raise ValueError(f"函数接口重复: {key}")
                # 接口 GUID 是受保护字段，保留节点对象即可保留 GUID；旧连线按配置重建。
                ports[key] = expression
                for pin in unreal.MaterialEditingLibrary.get_material_expression_input_names(expression):
                    unreal.MaterialEditingLibrary.disconnect_material_expressions(expression, pin)
            else:
                unreal.MaterialEditingLibrary.delete_material_expression_in_function(function, expression)
    else:
        function = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            name, folder, unreal.MaterialFunction, unreal.MaterialFunctionFactoryNew())
    if not function:
        raise RuntimeError(f"创建材质函数失败: {path}")
    function.set_editor_property("Description", template.get("Description", ""))
    nodes = {}
    for index, data in enumerate(template["Nodes"]):
        key = (data["Type"], data["Name"])
        expression = _create_node(function, data, index, ports.get(key))
        nodes[data["Name"]] = expression
    for connection in template["Connections"]:
        _connect_nodes(function, unreal.MaterialEditingLibrary, nodes, connection)
    unreal.MaterialEditingLibrary.update_material_function(function)
    if not unreal.EditorAssetLibrary.save_loaded_asset(function):
        raise RuntimeError(f"材质函数保存失败: {path}")
    _log(f"材质函数生成完成: {path}")
    return True


def _generate_parameter_collection(template):
    """原位更新集合默认值，保留同名参数 GUID，防止已有材质引用失效。"""
    if not IN_UE:
        return False
    name, folder = template["Name"], template["OutputPath"].rstrip("/")
    path = folder + "/" + name
    if not ensure_editor_not_playing_for_existing_asset(path):
        return False
    collection = unreal.load_asset(path)
    if collection and not isinstance(collection, unreal.MaterialParameterCollection):
        raise TypeError(f"目标不是参数集合: {path}")
    if not collection:
        collection = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            name, folder, unreal.MaterialParameterCollection, unreal.MaterialParameterCollectionFactoryNew())
    if not collection:
        raise RuntimeError(f"创建参数集合失败: {path}")
    names = set()
    for field, struct_type in (("ScalarParameters", unreal.CollectionScalarParameter),
                               ("VectorParameters", unreal.CollectionVectorParameter)):
        previous = {str(parameter.get_editor_property("parameter_name")): parameter for parameter in collection.get_editor_property(field)}
        parameters = []
        for data in template.get(field, []):
            parameter_name = data["Name"]
            if parameter_name in names:
                raise ValueError(f"集合参数重复: {parameter_name}")
            names.add(parameter_name)
            parameter = previous.get(parameter_name)
            if parameter is None:
                parameter = struct_type()
            parameter.set_editor_property("parameter_name", parameter_name)
            value = data["Value"]
            parameter.set_editor_property("default_value", float(value) if field == "ScalarParameters"
                                          else unreal.LinearColor(*value))
            parameters.append(parameter)
        collection.set_editor_property(field, parameters)
    if not unreal.EditorAssetLibrary.save_loaded_asset(collection):
        raise RuntimeError(f"参数集合保存失败: {path}")
    _log(f"参数集合生成完成: {path}")
    return True


def _generate_material_surface_extension(template):
    """从正式配置扩展现有游戏材质，保留原图、资源身份和材质实例引用。"""
    path = template["OutputPath"].rstrip("/") + "/" + template["Name"]
    if not IN_UE or not ensure_editor_not_playing_for_existing_asset(path):
        return False
    material = unreal.load_asset(path)
    if not isinstance(material, unreal.Material):
        raise TypeError(f"被扩展的原材质不存在: {path}")
    extension = template["SurfaceExtension"]
    function = unreal.load_asset(extension["SnowFunction"])
    collection = unreal.load_asset(extension["Collection"])
    if not isinstance(function, unreal.MaterialFunctionInterface) or not isinstance(collection, unreal.MaterialParameterCollection):
        raise TypeError(f"公共覆盖依赖缺失: {path}")
    if not unreal.SurfaceMaterialAuthoringLibrary.apply_snow_cover(material, function, collection, extension["EnableSnowCover"]):
        raise RuntimeError(f"原位接入积雪失败: {path}")
    if not unreal.EditorAssetLibrary.save_loaded_asset(material):
        raise RuntimeError(f"扩展材质保存失败: {path}")
    _log(f"正式游戏材质扩展完成: {path}")
    return True


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

    _validate_graph(template)
    output_path = output_path.rstrip("/") + "/"
    asset_path = output_path + name
    if not ensure_editor_not_playing_for_existing_asset(asset_path):
        return False

    mel = unreal.MaterialEditingLibrary

    # 检查材质是否已存在，存在则更新而不是重建（保留引用）
    mat = unreal.load_asset(asset_path)
    if mat:
        if not isinstance(mat, unreal.Material):
            raise TypeError(f"目标不是母材质: {asset_path}")
        _log(f"  原位更新母材质，保留实例引用: {asset_path}")
        # 引擎批量删除会边遍历边移除表达式，使用节点快照逐个删除避免残留旧图。
        for expression in list(mel.get_material_expressions(mat)):
            mel.delete_material_expression(mat, expression)

    if not mat:
        asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
        factory = unreal.MaterialFactoryNew()
        mat = asset_tools.create_asset(name, output_path, unreal.Material, factory)
        if not mat:
            _log_error(f"创建材质失败: {name}")
            return False

    # 设置材质属性
    _apply_material_properties(mat, properties)

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
    try:
        mat.post_edit_change()
    except AttributeError:
        # UE 5.7: post_edit_change 不再暴露给 Python，recompile_material 已足够
        pass
    asset_path = output_path + name
    if not unreal.EditorAssetLibrary.save_loaded_asset(mat):
        raise RuntimeError(f"母材质保存失败: {asset_path}")

    _log(f"母材质生成完成: {asset_path} ({len(nodes)} 个节点, {len(connections)} 条连线)")
    return True


def _apply_material_properties(mat, properties):
    """设置材质属性"""
    if not properties:
        return

    material_domain = properties.get("MaterialDomain", "")
    material_domain_map = {
        "Surface": unreal.MaterialDomain.MD_SURFACE,
        "DeferredDecal": unreal.MaterialDomain.MD_DEFERRED_DECAL,
        "LightFunction": unreal.MaterialDomain.MD_LIGHT_FUNCTION,
        "Volume": unreal.MaterialDomain.MD_VOLUME,
        "PostProcess": unreal.MaterialDomain.MD_POST_PROCESS,
        "UserInterface": unreal.MaterialDomain.MD_UI,
        "UI": unreal.MaterialDomain.MD_UI,
    }
    if material_domain in material_domain_map:
        mat.set_editor_property("MaterialDomain", material_domain_map[material_domain])

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

    # 通用 bool 属性透传（bUsedWithSplineMeshes, bUsedWithLandscape 等）
    for key, val in properties.items():
        if key.startswith("bUsed") and isinstance(val, bool):
            try:
                mat.set_editor_property(key, val)
                _log(f"  {key}: {val}")
            except Exception as e:
                _log(f"  警告: 设置 {key} 失败: {e}")


def _create_node(mat, node_data, index=0, expression=None):
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

    # 同名函数接口复用原节点以保留受保护的 GUID，其余节点通过同一配置路径创建。
    expr = expression
    if expr is None:
        expr = (mel.create_material_expression_in_function(mat, expr_class, pos_x, pos_y)
                if isinstance(mat, unreal.MaterialFunction)
                else mel.create_material_expression(mat, expr_class, pos_x, pos_y))
    else:
        expr.set_editor_property("MaterialExpressionEditorX", pos_x)
        expr.set_editor_property("MaterialExpressionEditorY", pos_y)
    if not expr:
        raise RuntimeError(f"创建节点失败: {node_name} ({node_type})")
    expr.set_editor_property("Desc", "CookerJsonNode:" + node_name)

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

    elif node_type == "MaterialFunctionCall":
        function = unreal.load_asset(node_data["MaterialFunction"])
        if not expr.set_material_function(function):
            raise RuntimeError(f"函数绑定失败: {node_name}")

    elif node_type == "CollectionParameter":
        collection = unreal.load_asset(node_data["Collection"])
        parameter_name = node_data["ParameterName"]
        parameters = list(collection.get_editor_property("ScalarParameters")) + list(collection.get_editor_property("VectorParameters"))
        if parameter_name not in {str(parameter.get_editor_property("parameter_name")) for parameter in parameters}:
            raise ValueError(f"集合参数不存在: {node_name}.{parameter_name}")
        expr.set_editor_property("Collection", collection)
        expr.set_editor_property("ParameterName", parameter_name)

    elif node_type == "FunctionInput":
        expr.set_editor_property("InputName", node_name)
        expr.set_editor_property("InputType", getattr(unreal.FunctionInputType, "FUNCTION_INPUT_" + node_data["InputType"].upper()))
        expr.set_editor_property("SortPriority", node_data.get("SortPriority", index))
        expr.set_editor_property("bUsePreviewValueAsDefault", "PreviewValue" in node_data)
        if "PreviewValue" in node_data:
            # UE 5.8 的 float4 反射结构只提供无参构造，按声明逐分量填写预览值。
            preview = unreal.Vector4f()
            for component, value in zip(("X", "Y", "Z", "W"), node_data["PreviewValue"]):
                preview.set_editor_property(component, float(value))
            expr.set_editor_property("PreviewValue", preview)

    elif node_type == "FunctionOutput":
        expr.set_editor_property("OutputName", node_name)
        expr.set_editor_property("SortPriority", node_data.get("SortPriority", index))

    elif node_type == "StaticSwitchParameter":
        expr.set_editor_property("ParameterName", node_data.get("ParameterName", node_name))
        expr.set_editor_property("DefaultValue", node_data["Value"])

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
    """按声明的引脚连接；拒绝失败和隐式改接，防止不完整材质被保存。"""
    from_str = conn.get("From", "")
    to_str = conn.get("To", "")

    if not from_str or not to_str:
        raise ValueError(f"连线缺少端点: {conn}")

    # 解析 "NodeName.OutputPin" 格式
    from_parts = from_str.split(".")
    to_parts = to_str.split(".")

    from_node_name = from_parts[0]
    from_pin = from_parts[1] if len(from_parts) > 1 else ""

    to_node_name = to_parts[0]
    to_pin = to_parts[1] if len(to_parts) > 1 else ""

    from_expr = node_map[from_node_name]
    # 函数图只能通过 FunctionOutput 返回结果，不能连接材质表面输出。
    if to_node_name == "Material":
        if isinstance(mat, unreal.MaterialFunction):
            raise ValueError("函数图不能连接 Material 输出")
        mat_prop = _get_material_property(to_pin)
        result = mel.connect_material_property(from_expr, from_pin, mat_prop)
    else:
        to_expr = node_map[to_node_name]
        actual_to_pin = to_pin
        if isinstance(to_expr, unreal.MaterialExpressionLandscapeLayerBlend) and to_pin:
            actual_to_pin = "Layer " + to_pin
        result = mel.connect_material_expressions(from_expr, from_pin, to_expr, actual_to_pin)
    if not result:
        raise RuntimeError(f"材质连线失败: {from_str} → {to_str}")
    _log(f"  连线OK: {from_str} → {to_str}")


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
    return prop_map[pin_name]


# ===================================================================
# 材质实例生成（原有功能）
# ===================================================================

def _generate_material_instance(template):
    """生成或原位更新材质实例，目录是否带末尾斜杠不改变资源身份。"""
    name = template.get("Name", "MI_Generated")
    parent_path = template.get("ParentMaterial", template.get("Parent", "/Engine/EngineMaterials/DefaultMaterial"))
    output_path = template.get("OutputPath", "/Game/Art/Materials/Generated/").rstrip("/")
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

    asset_path = output_path + "/" + name

    # 检查是否已存在，存在则更新
    if not ensure_editor_not_playing_for_existing_asset(asset_path):
        return False

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

    for param_name, value in template.get("StaticSwitchParameters", {}).items():
        if not isinstance(value, bool):
            raise TypeError(f"静态开关必须是布尔值: {param_name}")
        if param_name not in {str(n) for n in mel.get_static_switch_parameter_names(mi)}:
            raise RuntimeError(f"静态开关不存在: {param_name}")
        # UE 5.8 此 setter 成功后也固定返回 false，必须回读实际参数值验收。
        mel.set_material_instance_static_switch_parameter_value(mi, param_name, value)
        if mel.get_material_instance_static_switch_parameter_value(mi, param_name) != value:
            raise RuntimeError(f"静态开关回读不一致: {param_name}")
    mel.update_material_instance(mi)
    if not unreal.EditorAssetLibrary.save_loaded_asset(mi):
        raise RuntimeError(f"材质实例保存失败: {asset_path}")
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
        "ParentMaterial": "",
        "OutputPath": str(asset_path).rsplit("/", 1)[0] + "/",
        "Textures": {},
        "Parameters": {},
    }

    parent = mi.get_editor_property("Parent")
    if parent:
        template["ParentMaterial"] = parent.get_path_name()

    mel = unreal.MaterialEditingLibrary
    for parameter in mel.get_texture_parameter_names(mi):
        texture = mel.get_material_instance_texture_parameter_value(mi, parameter)
        if texture:
            template["Textures"][str(parameter)] = texture.get_path_name()
    for parameter in mel.get_scalar_parameter_names(mi):
        template["Parameters"][str(parameter)] = mel.get_material_instance_scalar_parameter_value(mi, parameter)
    for parameter in mel.get_vector_parameter_names(mi):
        color = mel.get_material_instance_vector_parameter_value(mi, parameter)
        template["Parameters"][str(parameter)] = [color.r, color.g, color.b, color.a]
    template["StaticSwitchParameters"] = {
        str(parameter): mel.get_material_instance_static_switch_parameter_value(mi, parameter)
        for parameter in mel.get_static_switch_parameter_names(mi)
    }

    os.makedirs(os.path.dirname(json_path), exist_ok=True)
    with open(json_path, "w", encoding="utf-8") as f:
        json.dump(template, f, indent=2, ensure_ascii=False)

    _log(f"材质实例导出完成: {json_path}")
    return True


def _export_master_material(mat, asset_path, json_path):
    """导出母材质（节点图）"""
    mel = unreal.MaterialEditingLibrary
    expressions = mel.get_material_expressions(mat)

    # 旧导出器不能完整表达函数调用及集合连线，禁止覆盖公共层的正式源模板。
    # 本轮只增加生成和独立资源回读；完整图反向编辑需另行实现。
    if any(isinstance(expr, (unreal.MaterialExpressionMaterialFunctionCall,
                             unreal.MaterialExpressionCollectionParameter)) for expr in expressions):
        _log("公共函数材质的完整反向导出尚未支持；保留正式 JSON: " + json_path)
        return False

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
