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
        # 尝试直接作为路径使用
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

    # 添加组件
    subsystem = unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)
    for comp_data in components:
        _add_component(bp, subsystem, comp_data)

    # UnLua 绑定
    if unlua_binding:
        _set_unlua_binding(bp, unlua_binding)

    # 编译蓝图
    unreal.KismetSystemLibrary.compile_blueprint(bp)

    # 保存
    unreal.EditorAssetLibrary.save_asset(asset_path)
    _log(f"蓝图生成完成: {asset_path}")
    return True


def _add_component(bp, subsystem, comp_data):
    """向蓝图添加组件"""
    comp_type = comp_data.get("Type", "SceneComponent")
    comp_name = comp_data.get("Name", "NewComponent")

    # 组件类型映射
    type_map = {
        "SceneComponent": unreal.SceneComponent,
        "StaticMesh": unreal.StaticMeshComponent,
        "SkeletalMesh": unreal.SkeletalMeshComponent,
        "BoxCollision": unreal.BoxComponent,
        "SphereCollision": unreal.SphereComponent,
        "CapsuleCollision": unreal.CapsuleComponent,
        "PointLight": unreal.PointLightComponent,
        "SpotLight": unreal.SpotLightComponent,
        "ParticleSystem": unreal.ParticleSystemComponent,
        "Audio": unreal.AudioComponent,
        "Arrow": unreal.ArrowComponent,
        "Billboard": unreal.BillboardComponent,
    }

    comp_class = type_map.get(comp_type)
    if not comp_class:
        _log(f"  未知组件类型: {comp_type}，跳过")
        return

    # 通过 SCS 添加组件
    scs = bp.get_editor_property("SimpleConstructionScript")
    if not scs:
        _log(f"  无法获取 SCS，跳过组件: {comp_name}")
        return

    node = scs.create_node(comp_class, comp_name)
    if not node:
        _log(f"  创建组件节点失败: {comp_name}")
        return

    scs.add_node(node)
    comp_template = node.get_editor_property("ComponentTemplate")

    # 设置组件属性
    if comp_type == "StaticMesh" and comp_data.get("Mesh"):
        mesh = unreal.load_asset(comp_data["Mesh"])
        if mesh:
            comp_template.set_editor_property("StaticMesh", mesh)

    if comp_type == "BoxCollision" and comp_data.get("Extent"):
        ext = comp_data["Extent"]
        comp_template.set_editor_property("BoxExtent",
            unreal.Vector(ext[0], ext[1], ext[2]))

    if comp_type == "SphereCollision" and comp_data.get("Radius"):
        comp_template.set_editor_property("SphereRadius", comp_data["Radius"])

    _log(f"  组件已添加: {comp_name} ({comp_type})")


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
