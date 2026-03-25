"""BlueprintFactory - 关卡生成器
从 JSON 模板创建完整关卡：Level + 地形 + Actor 摆放 + WorldSettings。
支持反向导出：当前关卡 → JSON 模板。

JSON 模板格式:
{
    "LevelName": "cooker_world",
    "OutputPath": "/Game/Maps/",
    "Landscape": {
        "Heightmap": "heightmap_main.png",
        "Size": {"X": 200, "Y": 200},
        "Scale": {"X": 200, "Y": 200, "Z": 100}
    },
    "Actors": [
        {"Class": "BP路径", "Location": [x,y,z], "Rotation": [p,y,r], "Scale": [x,y,z]},
        {"Class": "DirectionalLight", "Rotation": [-45, 30, 0]},
        {"Class": "SkyLight"}
    ],
    "WorldSettings": {
        "GameMode": "/Game/Blueprints/BP_CookerGameMode.BP_CookerGameMode_C"
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
        unreal.log(f"[LevelFactory] {msg}")
    else:
        print(f"[LevelFactory] {msg}")


def _log_error(msg):
    if IN_UE:
        unreal.log_error(f"[LevelFactory] {msg}")
    else:
        print(f"[LevelFactory ERROR] {msg}")


# 内置 Actor 类型映射（不需要完整路径的简写）
BUILTIN_CLASSES = {
    "DirectionalLight": "/Script/Engine.DirectionalLight",
    "PointLight": "/Script/Engine.PointLight",
    "SpotLight": "/Script/Engine.SpotLight",
    "SkyLight": "/Script/Engine.SkyLight",
    "SkyAtmosphere": "/Script/Engine.SkyAtmosphere",
    "ExponentialHeightFog": "/Script/Engine.ExponentialHeightFog",
    "PostProcessVolume": "/Script/Engine.PostProcessVolume",
    "PlayerStart": "/Script/Engine.PlayerStart",
    "CameraActor": "/Script/Engine.CameraActor",
    "StaticMeshActor": "/Script/Engine.StaticMeshActor",
}


def generate_level(json_path: str):
    """从 JSON 模板生成完整关卡"""
    if not os.path.isfile(json_path):
        _log_error(f"JSON 文件不存在: {json_path}")
        return False

    with open(json_path, "r", encoding="utf-8") as f:
        template = json.load(f)

    level_name = template.get("LevelName", "NewLevel")
    output_path = template.get("OutputPath", "/Game/Maps/")
    actors = template.get("Actors", [])
    world_settings = template.get("WorldSettings", {})
    landscape = template.get("Landscape")

    if not IN_UE:
        _log(f"非 UE 环境，跳过关卡生成: {level_name}")
        return False

    _log(f"生成关卡: {level_name}")

    # 1. 创建新关卡
    asset_path = output_path + level_name
    level_created = _create_level(asset_path)
    if not level_created:
        _log(f"使用当前已打开的关卡")

    # 2. 设置 WorldSettings
    if world_settings:
        _apply_world_settings(world_settings)

    # 3. 地形（如果有）
    if landscape:
        from blueprint_factory.land_generator import generate_landscape
        # 将 landscape 配置写入临时 JSON
        land_json = os.path.join(os.path.dirname(json_path), "_temp_landscape.json")
        with open(land_json, "w", encoding="utf-8") as f:
            json.dump(landscape, f)
        generate_landscape(land_json)
        try:
            os.remove(land_json)
        except Exception:
            pass

    # 4. 摆放 Actor
    spawned = 0
    for actor_data in actors:
        if _spawn_actor(actor_data):
            spawned += 1

    # 5. 保存关卡
    unreal.EditorLevelLibrary.save_current_level()

    _log(f"关卡生成完成: {level_name}, 摆放了 {spawned} 个 Actor")
    return True


def _create_level(asset_path):
    """创建新关卡"""
    try:
        # 检查是否已存在
        if unreal.EditorAssetLibrary.does_asset_exist(asset_path):
            _log(f"  关卡已存在，打开: {asset_path}")
            unreal.EditorLevelLibrary.load_level(asset_path)
            return True

        # 创建新关卡
        success = unreal.EditorLevelLibrary.new_level(asset_path)
        if success:
            _log(f"  新关卡已创建: {asset_path}")
        return success
    except Exception as e:
        _log_error(f"  创建关卡失败: {e}")
        return False


def _apply_world_settings(settings):
    """设置 WorldSettings"""
    try:
        world = unreal.EditorLevelLibrary.get_editor_world()
        if not world:
            return

        ws = world.get_world_settings()
        if not ws:
            return

        # GameMode
        gm_path = settings.get("GameMode")
        if gm_path:
            gm_class = unreal.load_class(None, gm_path)
            if gm_class:
                ws.set_editor_property("DefaultGameMode", gm_class)
                _log(f"  GameMode: {gm_path}")
    except Exception as e:
        _log(f"  WorldSettings 设置失败: {e}")


def _spawn_actor(actor_data):
    """在当前关卡中摆放一个 Actor"""
    class_path = actor_data.get("Class", "")
    if not class_path:
        return False

    # 特殊类型处理（GameMode/PlayerController 不需要 Spawn）
    actor_type = actor_data.get("Type", "")
    if actor_type in ("GameMode", "PlayerController"):
        _log(f"  跳过 {actor_type}（通过 WorldSettings 设置）")
        return True

    # 解析类路径
    resolved = BUILTIN_CLASSES.get(class_path, class_path)
    actor_class = unreal.load_class(None, resolved)
    if not actor_class:
        # 尝试作为蓝图路径加载
        actor_class = unreal.load_class(None, class_path)
    if not actor_class:
        _log_error(f"  无法加载类: {class_path}")
        return False

    # 位置/旋转/缩放
    loc_data = actor_data.get("Location", [0, 0, 0])
    rot_data = actor_data.get("Rotation", [0, 0, 0])
    scale_data = actor_data.get("Scale", [1, 1, 1])

    loc = unreal.Vector(loc_data[0], loc_data[1], loc_data[2])
    rot = unreal.Rotator(rot_data[0], rot_data[1], rot_data[2])

    # Spawn
    actor = unreal.EditorLevelLibrary.spawn_actor_from_class(actor_class, loc, rot)
    if not actor:
        _log_error(f"  Spawn 失败: {class_path}")
        return False

    # 缩放
    if scale_data != [1, 1, 1]:
        actor.set_actor_scale3d(unreal.Vector(scale_data[0], scale_data[1], scale_data[2]))

    # 标签
    label = actor_data.get("Label", "")
    if label:
        actor.set_actor_label(label)

    # 属性
    props = actor_data.get("Properties", {})
    for key, value in props.items():
        try:
            actor.set_editor_property(key, value)
        except Exception:
            pass

    name = label or class_path.split(".")[-1].split("/")[-1]
    _log(f"  Actor: {name} @ ({loc_data[0]}, {loc_data[1]}, {loc_data[2]})")
    return True


# ===================================================================
# 反向导出：当前关卡 → JSON 模板
# ===================================================================

def export_level(json_path: str):
    """将当前关卡导出为 JSON 模板"""
    if not IN_UE:
        _log("非 UE 环境，无法导出")
        return False

    _log("导出当前关卡...")

    world = unreal.EditorLevelLibrary.get_editor_world()
    if not world:
        _log_error("无法获取当前世界")
        return False

    template = {
        "LevelName": world.get_name(),
        "OutputPath": "/Game/Maps/",
        "Actors": [],
        "WorldSettings": {},
    }

    # WorldSettings
    ws = world.get_world_settings()
    if ws:
        gm = ws.get_editor_property("DefaultGameMode")
        if gm:
            template["WorldSettings"]["GameMode"] = gm.get_path_name()

    # 遍历所有 Actor
    actors = unreal.EditorLevelLibrary.get_all_level_actors()
    for actor in actors:
        actor_data = _export_actor(actor)
        if actor_data:
            template["Actors"].append(actor_data)

    # 写入 JSON
    os.makedirs(os.path.dirname(json_path), exist_ok=True)
    with open(json_path, "w", encoding="utf-8") as f:
        json.dump(template, f, indent=2, ensure_ascii=False)

    _log(f"关卡导出完成: {json_path} ({len(template['Actors'])} 个 Actor)")
    return True


def _export_actor(actor):
    """导出单个 Actor 为字典"""
    # 跳过内置不可见 Actor
    class_name = actor.get_class().get_name()
    skip_classes = {"WorldSettings", "Brush", "DefaultPhysicsVolume",
                    "GameplayDebuggerCategoryReplicator", "AbstractNavData"}
    if class_name in skip_classes:
        return None

    loc = actor.get_actor_location()
    rot = actor.get_actor_rotation()
    scale = actor.get_actor_scale3d()

    # 获取蓝图路径或类路径
    bp = actor.get_class().get_outer()
    if bp and hasattr(bp, "get_path_name"):
        class_path = actor.get_class().get_path_name()
    else:
        class_path = actor.get_class().get_path_name()

    # 简化内置类名
    for short_name, full_path in BUILTIN_CLASSES.items():
        if full_path in class_path:
            class_path = short_name
            break

    data = {
        "Class": class_path,
        "Location": [round(loc.x, 1), round(loc.y, 1), round(loc.z, 1)],
        "Rotation": [round(rot.pitch, 1), round(rot.yaw, 1), round(rot.roll, 1)],
    }

    # 只在非默认缩放时记录
    if abs(scale.x - 1) > 0.01 or abs(scale.y - 1) > 0.01 or abs(scale.z - 1) > 0.01:
        data["Scale"] = [round(scale.x, 2), round(scale.y, 2), round(scale.z, 2)]

    # 标签
    label = actor.get_actor_label()
    if label:
        data["Label"] = label

    return data
