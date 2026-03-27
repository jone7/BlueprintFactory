"""BlueprintFactory - 蓝图生成器
从 JSON 模板生成 Actor Blueprint，包含组件树、属性、UnLua 绑定。

JSON 模板格式:
{
    "Name": "BP_FarmPlot",
    "ParentClass": "Actor",
    "OutputPath": "/Game/Blueprints/Farm/",
    "Components": [
        {"Type": "StaticMesh", "Name": "PlotMesh", "Mesh": "/Game/Art/Meshes/Farm/SM_FarmPlot"},
        {"Type": "BoxCollision", "Name": "Interaction", "Extent": [100, 100, 50]},
        {"Type": "SceneComponent", "Name": "CropSlot"}
    ],
    "UnLuaBinding": "BP_FarmPlot_C"
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
        unreal.log(f"[BPFactory] {msg}")
    else:
        print(f"[BPFactory] {msg}")


def _log_error(msg):
    if IN_UE:
        unreal.log_error(f"[BPFactory] {msg}")
    else:
        print(f"[BPFactory ERROR] {msg}")


# 父类映射
PARENT_CLASS_MAP = {
    "Actor": "/Script/Engine.Actor",
    "Character": "/Script/Engine.Character",
    "Pawn": "/Script/Engine.Pawn",
    "CookerCharacter": "/Script/cooker.CookerCharacter",
}


def generate_blueprint(json_path: str):
    """从 JSON 模板生成蓝图"""
    if not os.path.isfile(json_path):
        _log_error(f"JSON 文件不存在: {json_path}")
        return False

    with open(json_path, "r", encoding="utf-8") as f:
        template = json.load(f)

    name = template.get("Name", "BP_Generated")
    parent = template.get("ParentClass", "Actor")
    output_path = template.get("OutputPath", "/Game/Blueprints/Generated/")
    components = template.get("Components", [])
    unlua_binding = template.get("UnLuaBinding", "")

    if not IN_UE:
        _log(f"非 UE 环境，跳过生成: {name}")
        return False

    _log(f"生成蓝图: {name} (父类={parent})")

    # 获取父类
    parent_path = PARENT_CLASS_MAP.get(parent)
    if not parent_path:
        parent_path = parent

    parent_class = unreal.load_class(None, parent_path)
    if not parent_class:
        _log_error(f"无法加载父类: {parent_path}")
        return False

    # 创建蓝图资产
    asset_path = output_path + name
    factory = unreal.BlueprintFactory()
    factory.set_editor_property("ParentClass", parent_class)

    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    bp = asset_tools.create_asset(name, output_path, unreal.Blueprint, factory)
    if not bp:
        _log_error(f"创建蓝图失败: {asset_path}")
        return False

    _log(f"蓝图资产已创建: {asset_path}")

    # UE 5.1+ 使用 SubobjectDataSubsystem 添加组件
    subsystem = unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)
    bfl = unreal.SubobjectDataBlueprintFunctionLibrary

    # 获取根 handle
    handles = subsystem.k2_gather_subobject_data_for_blueprint(bp)
    if not handles or len(handles) == 0:
        _log_error("无法获取蓝图 subobject handles")
        return False

    root_handle = handles[0]

    # 找到或创建 Scene root handle（用于挂载子组件）
    scene_root_handle = root_handle

    for comp_data in components:
        comp_type = comp_data.get("Type", "SceneComponent")
        comp_name = comp_data.get("Name", "NewComponent")

        if comp_type == "SceneComponent" and comp_name == "Root":
            # 跳过 Root SceneComponent，使用默认的 DefaultSceneRoot
            continue

        result = _add_component_v2(bp, subsystem, bfl, scene_root_handle, comp_data)
        if result is None:
            _log(f"  组件添加失败: {comp_name}")

    # UnLua 绑定
    if unlua_binding:
        _set_unlua_binding(bp, unlua_binding)

    # 编译蓝图
    try:
        unreal.KismetSystemLibrary.compile_blueprint(bp)
    except Exception:
        try:
            unreal.BlueprintEditorLibrary.compile_blueprint(bp)
        except Exception as e:
            _log(f"蓝图编译警告（可能仍然有效）: {e}")

    # 保存
    unreal.EditorAssetLibrary.save_asset(asset_path)
    _log(f"蓝图生成完成: {asset_path}")
    return True


# 组件类型映射
COMPONENT_TYPE_MAP = {
    "SceneComponent": "SceneComponent",
    "StaticMesh": "StaticMeshComponent",
    "SkeletalMesh": "SkeletalMeshComponent",
    "BoxCollision": "BoxComponent",
    "SphereCollision": "SphereComponent",
    "CapsuleCollision": "CapsuleComponent",
    "PointLight": "PointLightComponent",
    "SpotLight": "SpotLightComponent",
    "ParticleSystem": "ParticleSystemComponent",
    "Audio": "AudioComponent",
    "Arrow": "ArrowComponent",
    "Billboard": "BillboardComponent",
}

COMPONENT_CLASS_MAP = {
    "SceneComponent": unreal.SceneComponent if IN_UE else None,
    "StaticMesh": unreal.StaticMeshComponent if IN_UE else None,
    "SkeletalMesh": unreal.SkeletalMeshComponent if IN_UE else None,
    "BoxCollision": unreal.BoxComponent if IN_UE else None,
    "SphereCollision": unreal.SphereComponent if IN_UE else None,
    "CapsuleCollision": unreal.CapsuleComponent if IN_UE else None,
    "PointLight": unreal.PointLightComponent if IN_UE else None,
    "SpotLight": unreal.SpotLightComponent if IN_UE else None,
    "ParticleSystem": unreal.ParticleSystemComponent if IN_UE else None,
    "Audio": unreal.AudioComponent if IN_UE else None,
    "Arrow": unreal.ArrowComponent if IN_UE else None,
    "Billboard": unreal.BillboardComponent if IN_UE else None,
}


def _add_component_v2(bp, subsystem, bfl, parent_handle, comp_data):
    """UE 5.1+ 方式添加组件到蓝图"""
    comp_type = comp_data.get("Type", "SceneComponent")
    comp_name = comp_data.get("Name", "NewComponent")

    comp_class = COMPONENT_CLASS_MAP.get(comp_type)
    if not comp_class:
        _log(f"  未知组件类型: {comp_type}，跳过")
        return None

    # 通过 SubobjectDataSubsystem 添加组件
    sub_handle, fail_reason = subsystem.add_new_subobject(
        params=unreal.AddNewSubobjectParams(
            parent_handle=parent_handle,
            new_class=comp_class,
            blueprint_context=bp
        )
    )

    if not fail_reason.is_empty():
        _log_error(f"  添加组件失败 [{comp_name}]: {fail_reason}")
        return None

    # 重命名
    subsystem.rename_subobject(handle=sub_handle, new_name=unreal.Text(comp_name))

    # 挂载到父节点
    subsystem.attach_subobject(owner_handle=parent_handle, child_to_add_handle=sub_handle)

    # 获取组件对象以设置属性
    sub_data = bfl.get_data(sub_handle)
    comp_obj = bfl.get_object(sub_data)

    if comp_obj:
        _set_component_properties(comp_obj, comp_type, comp_data)

    _log(f"  组件已添加: {comp_name} ({comp_type})")
    return sub_handle


def _set_component_properties(comp_obj, comp_type, comp_data):
    """设置组件属性"""
    if comp_type == "StaticMesh" and comp_data.get("Mesh"):
        mesh_path = comp_data["Mesh"]
        mesh = unreal.load_asset(mesh_path)
        if mesh:
            comp_obj.set_static_mesh(mesh)
            _log(f"    设置 Mesh: {mesh_path}")
        else:
            _log(f"    Mesh 未找到（渐进式生成，跳过）: {mesh_path}")

    if comp_type == "SkeletalMesh" and comp_data.get("Mesh"):
        mesh_path = comp_data["Mesh"]
        mesh = unreal.load_asset(mesh_path)
        if mesh:
            comp_obj.set_editor_property("SkeletalMeshAsset", mesh)
            _log(f"    设置 SkeletalMesh: {mesh_path}")
        else:
            _log(f"    SkeletalMesh 未找到（渐进式生成，跳过）: {mesh_path}")

    if comp_type == "BoxCollision" and comp_data.get("Extent"):
        ext = comp_data["Extent"]
        comp_obj.set_editor_property("BoxExtent", unreal.Vector(ext[0], ext[1], ext[2]))

    if comp_type == "SphereCollision" and comp_data.get("Radius"):
        comp_obj.set_editor_property("SphereRadius", comp_data["Radius"])

    if comp_type == "CapsuleCollision":
        if comp_data.get("Radius"):
            comp_obj.set_editor_property("CapsuleRadius", comp_data["Radius"])
        if comp_data.get("HalfHeight"):
            comp_obj.set_editor_property("CapsuleHalfHeight", comp_data["HalfHeight"])

    # 通用属性：相对位置/旋转
    if comp_data.get("Location"):
        loc = comp_data["Location"]
        comp_obj.set_editor_property("RelativeLocation", unreal.Vector(loc[0], loc[1], loc[2]))

    if comp_data.get("Rotation"):
        rot = comp_data["Rotation"]
        comp_obj.set_editor_property("RelativeRotation", unreal.Rotator(rot[0], rot[1], rot[2]))

    if comp_data.get("Scale"):
        sc = comp_data["Scale"]
        comp_obj.set_editor_property("RelativeScale3D", unreal.Vector(sc[0], sc[1], sc[2]))


def _set_unlua_binding(bp, module_name):
    """设置 UnLua 绑定（通过接口实现）"""
    try:
        # UnLua 绑定通过 GetModuleName 接口实现
        # 需要蓝图实现 UnLuaInterface
        _log(f"  UnLua 绑定: {module_name}（需手动在蓝图中设置 UnLuaInterface）")
    except Exception as e:
        _log(f"  UnLua 绑定设置失败: {e}")


# ===================================================================
# 反向导出：Blueprint → JSON 模板
# ===================================================================

def export_blueprint(asset_path: str, json_path: str):
    """将已有蓝图导出为 JSON 模板"""
    if not IN_UE:
        _log("非 UE 环境，无法导出")
        return False

    bp = unreal.load_asset(asset_path)
    if not bp:
        _log_error(f"无法加载蓝图: {asset_path}")
        return False

    _log(f"导出蓝图: {asset_path}")

    template = {
        "Name": bp.get_name(),
        "ParentClass": "",
        "OutputPath": str(asset_path).rsplit("/", 1)[0] + "/",
        "Components": [],
        "UnLuaBinding": "",
    }

    # 父类
    parent = bp.get_editor_property("ParentClass")
    if parent:
        parent_path = parent.get_path_name()
        # 反查简写
        for short, full in PARENT_CLASS_MAP.items():
            if full in parent_path:
                parent_path = short
                break
        template["ParentClass"] = parent_path

    # 组件
    scs = bp.get_editor_property("SimpleConstructionScript")
    if scs:
        nodes = scs.get_all_nodes()
        for node in nodes:
            comp = node.get_editor_property("ComponentTemplate")
            if comp:
                comp_data = {
                    "Type": comp.get_class().get_name().replace("Component", ""),
                    "Name": comp.get_name(),
                }

                # StaticMesh
                if isinstance(comp, unreal.StaticMeshComponent):
                    mesh = comp.get_editor_property("StaticMesh")
                    if mesh:
                        comp_data["Mesh"] = mesh.get_path_name()

                # BoxComponent
                if isinstance(comp, unreal.BoxComponent):
                    ext = comp.get_editor_property("BoxExtent")
                    comp_data["Extent"] = [ext.x, ext.y, ext.z]

                # SphereComponent
                if isinstance(comp, unreal.SphereComponent):
                    comp_data["Radius"] = comp.get_editor_property("SphereRadius")

                template["Components"].append(comp_data)

    # 写入 JSON
    os.makedirs(os.path.dirname(json_path), exist_ok=True)
    with open(json_path, "w", encoding="utf-8") as f:
        json.dump(template, f, indent=2, ensure_ascii=False)

    _log(f"蓝图导出完成: {json_path} ({len(template['Components'])} 个组件)")
    return True
