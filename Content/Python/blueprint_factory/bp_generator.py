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
    preserve_existing_components = template.get("PreserveExistingComponents", False)
    reset_existing_asset = bool(template.get("ResetExistingAsset", not preserve_existing_components))
    recreate_existing_asset = bool(template.get("RecreateExistingAsset", reset_existing_asset and not preserve_existing_components))
    reset_existing_graphs = bool(template.get("ResetExistingGraphs", reset_existing_asset))
    reset_existing_variables = bool(template.get("ResetExistingVariables", reset_existing_asset))
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

    # 检查蓝图是否已存在，存在则更新
    asset_path = output_path + name
    bp = unreal.load_asset(asset_path)
    if bp and isinstance(bp, unreal.Blueprint):
        if recreate_existing_asset and not preserve_existing_components:
            deleted = False
            try:
                deleted = bool(unreal.EditorAssetLibrary.delete_asset(asset_path))
            except Exception:
                deleted = False

            if not deleted:
                _log_error(f"删除已有蓝图失败，无法安全重建: {asset_path}")
                return False

            _log(f"  已删除旧蓝图，准备重建: {asset_path}")
            bp = None
        else:
            reset_succeeded = False
            if reset_existing_asset:
                lib = getattr(unreal, "BPFactoryBlueprintLibrary", None)
                reset_func = None
                if lib:
                    reset_func = getattr(lib, "reset_blueprint_for_regeneration", None)
                    if not callable(reset_func):
                        reset_func = getattr(lib, "ResetBlueprintForRegeneration", None)
                if callable(reset_func):
                    reset_succeeded = bool(reset_func(bp, reset_existing_graphs, reset_existing_variables))
                    if reset_succeeded:
                        _log("  Cleared existing blueprint content before regeneration")
                    else:
                        _log("  Failed to clear existing blueprint content, continuing with overwrite attempt")
                else:
                    _log("  ResetBlueprintForRegeneration not available, continuing with overwrite attempt")
            _log(f"  蓝图已存在，更新模式: {asset_path}")
            if preserve_existing_components:
                _log("  保留现有组件，仅更新属性/绑定")
            elif not reset_succeeded:
                # 清除旧组件（保留 DefaultSceneRoot）
                subsystem = unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)
                bfl = unreal.SubobjectDataBlueprintFunctionLibrary
                old_handles = subsystem.k2_gather_subobject_data_for_blueprint(bp)
                if old_handles and len(old_handles) > 1:
                    # 跳过第一个（root），删除其余
                    for h in old_handles[1:]:
                        # UE 5.7: get_object_for_handle 移到 SubobjectDataSubsystem
                        obj = None
                        try:
                            obj = bfl.get_object_for_handle(h)
                        except AttributeError:
                            try:
                                obj = subsystem.get_object_for_handle(h)
                            except Exception:
                                pass
                        if obj and obj.get_name() != "DefaultSceneRoot":
                            subsystem.delete_subobject_from_blueprint(h, bp)
                    _log(f"  已清除旧组件")

    if not (bp and isinstance(bp, unreal.Blueprint)):
        # 创建新蓝图资产
        factory = unreal.BlueprintFactory()
        factory.set_editor_property("ParentClass", parent_class)

        asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
        bp = asset_tools.create_asset(name, output_path, unreal.Blueprint, factory)
        if not bp:
            _log_error(f"创建蓝图失败: {asset_path}")
            return False

    _log(f"蓝图资产: {asset_path}")

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
    component_handles = {}

    for comp_data in components:
        comp_type = comp_data.get("Type", "SceneComponent")
        comp_name = comp_data.get("Name", "NewComponent")
        parent_name = str(comp_data.get("Parent", "") or "")
        wants_root = bool(comp_data.get("IsRoot", False)) or (comp_type == "SceneComponent" and comp_name == "Root")

        parent_handle = None
        if parent_name:
            parent_handle = component_handles.get(parent_name)
            if parent_handle is None:
                parent_handle = _find_subobject_handle_by_name(bp, parent_name)
        if parent_handle is None:
            parent_handle = scene_root_handle

        if preserve_existing_components:
            existing_comp = _find_existing_component_template(bp, comp_name)
            if existing_comp:
                _set_component_properties(existing_comp, comp_type, comp_data)
                _log(f"  Updated existing component: {comp_name} ({comp_type})")
                existing_handle = _find_subobject_handle_by_name(bp, comp_name)
                if existing_handle is not None:
                    component_handles[comp_name] = existing_handle
                    if wants_root:
                        scene_root_handle = existing_handle
                continue

        result = _add_component_v2(bp, subsystem, bfl, parent_handle, comp_data)
        if result is None:
            _log(f"  组件添加失败: {comp_name}")
            continue
        component_handles[comp_name] = result
        if wants_root:
            scene_root_handle = result

    # UnLua 绑定
    if unlua_binding:
        _set_unlua_binding(bp, unlua_binding)

    # 先编译蓝图（生成 GeneratedClass）
    try:
        unreal.KismetSystemLibrary.compile_blueprint(bp)
    except Exception:
        try:
            unreal.BlueprintEditorLibrary.compile_blueprint(bp)
        except Exception as e:
            _log(f"蓝图首次编译警告: {e}")

    # 设置 CDO（Class Default Object）属性
    cdo_properties = template.get("Properties", {})
    if cdo_properties and IN_UE:
        try:
            bp_gc = unreal.load_object(None, f"{asset_path}.{name}_C")
            if bp_gc:
                bp_cdo = unreal.get_default_object(bp_gc)
                if bp_cdo:
                    for prop_name, prop_value in cdo_properties.items():
                        try:
                            if isinstance(prop_value, str):
                                assigned = False
                                if prop_value.startswith("/Game/"):
                                    asset = unreal.load_asset(prop_value)
                                    if asset:
                                        bp_cdo.set_editor_property(prop_name, asset)
                                        _log(f"  CDO 属性: {prop_name} = {prop_value}")
                                        assigned = True
                                    else:
                                        cls = unreal.load_class(None, prop_value)
                                        if cls:
                                            bp_cdo.set_editor_property(prop_name, cls)
                                            _log(f"  CDO 类属性: {prop_name} = {prop_value}")
                                            assigned = True
                                elif prop_value.startswith("/Script/"):
                                    cls = unreal.load_class(None, prop_value)
                                    if cls:
                                        bp_cdo.set_editor_property(prop_name, cls)
                                        _log(f"  CDO 类属性: {prop_name} = {prop_value}")
                                        assigned = True

                                if not assigned:
                                    bp_cdo.set_editor_property(prop_name, prop_value)
                                    _log(f"  CDO 属性: {prop_name} = {prop_value}")
                            elif isinstance(prop_value, (int, float)):
                                bp_cdo.set_editor_property(prop_name, float(prop_value))
                                _log(f"  CDO 属性: {prop_name} = {prop_value}")
                            else:
                                bp_cdo.set_editor_property(prop_name, prop_value)
                                _log(f"  CDO 属性: {prop_name} = {prop_value}")
                        except Exception as e:
                            _log(f"  CDO 属性失败: {prop_name}: {e}")
                else:
                    _log(f"  get_default_object 返回 None")
            else:
                _log(f"  load_object BPGC 失败: {asset_path}.{name}_C")
        except Exception as e:
            _log(f"  CDO 异常: {e}")

    # 保存
    unreal.EditorAssetLibrary.save_asset(asset_path)
    _log(f"蓝图生成完成: {asset_path}")
    return True


# 短名别名 → UE 组件类名（仅用于无法直接拼接的情况）
COMPONENT_TYPE_ALIASES = {
    "StaticMesh": "StaticMeshComponent",
    "SkeletalMesh": "SkeletalMeshComponent",
    "Widget": "WidgetComponent",
    "BoxCollision": "BoxComponent",
    "SphereCollision": "SphereComponent",
    "CapsuleCollision": "CapsuleComponent",
    "PointLight": "PointLightComponent",
    "SpotLight": "SpotLightComponent",
    "ParticleSystem": "ParticleSystemComponent",
    "Audio": "AudioComponent",
    "Arrow": "ArrowComponent",
    "Billboard": "BillboardComponent",
    "Spline": "SplineComponent",
    "SplineMesh": "SplineMeshComponent",
}

EXPORT_COMPONENT_TYPE_MAP = {
    "StaticMeshComponent": "StaticMesh",
    "SkeletalMeshComponent": "SkeletalMesh",
    "BoxComponent": "BoxCollision",
    "SphereComponent": "SphereCollision",
    "CapsuleComponent": "CapsuleCollision",
    "PointLightComponent": "PointLight",
    "SpotLightComponent": "SpotLight",
    "ParticleSystemComponent": "ParticleSystem",
    "AudioComponent": "Audio",
    "ArrowComponent": "Arrow",
    "BillboardComponent": "Billboard",
    "SplineComponent": "Spline",
    "SplineMeshComponent": "SplineMesh",
    "SceneComponent": "SceneComponent",
    "WidgetComponent": "WidgetComponent",
}

# 需要特殊属性设置的组件类型
_COMP_RESERVED_FIELDS = {"Type", "Name", "Parent", "IsRoot", "Mesh", "Material", "Location", "Rotation", "Scale",
                          "Points", "Extent", "Radius", "HalfHeight", "AnimationMode", "AnimClass",
                          "AnimToPlay", "AnimationAsset"}

_EXPORTED_COMPONENT_FIELDS = {
    "Type", "Name", "Parent", "IsRoot", "Mesh", "Material",
    "Location", "Rotation", "Scale",
    "Extent", "Radius", "HalfHeight",
    "CollisionProfileName", "bCanEverAffectNavigation", "bDynamicObstacle",
    "AnimationMode", "AnimClass", "AnimToPlay", "AnimationAsset",
}


def _safe_get_editor_property(obj, prop_name, default=None):
    if obj is None or not prop_name:
        return default
    try:
        return obj.get_editor_property(prop_name)
    except Exception:
        return default


def _load_existing_template(json_path):
    if not json_path or not os.path.isfile(json_path):
        return {}
    try:
        with open(json_path, "r", encoding="utf-8") as f:
            loaded = json.load(f)
        return loaded if isinstance(loaded, dict) else {}
    except Exception as exc:
        _log(f"读取已有模板失败，将改为全量导出: {exc}")
        return {}


def _normalize_component_type_for_export(comp):
    if comp is None:
        return "SceneComponent"
    class_name = comp.get_class().get_name()
    mapped = EXPORT_COMPONENT_TYPE_MAP.get(class_name)
    if mapped:
        return mapped
    if class_name.endswith("Component"):
        return class_name[:-9]
    return class_name


def _normalize_animation_mode_for_export(anim_mode):
    if anim_mode is None:
        return ""

    text = str(anim_mode)
    if "ANIMATION_BLUEPRINT" in text:
        return "UseAnimationBlueprint"
    if "ANIMATION_SINGLE_NODE" in text:
        return "UseAnimationAsset"
    if "ANIMATION_CUSTOM_MODE" in text:
        return "AnimationCustomMode"
    return text.split(".")[-1]


def _vector_to_list(vec):
    if vec is None:
        return None
    return [float(vec.x), float(vec.y), float(vec.z)]


def _rotator_to_list(rot):
    if rot is None:
        return None
    return [float(rot.pitch), float(rot.yaw), float(rot.roll)]


def _is_close(a, b, epsilon=0.001):
    return abs(float(a) - float(b)) <= epsilon


def _is_vector_close(values, expected, epsilon=0.001):
    if values is None or len(values) != len(expected):
        return False
    for idx, expected_value in enumerate(expected):
        if not _is_close(values[idx], expected_value, epsilon):
            return False
    return True


def _merge_component_export(existing_comp, exported_comp):
    merged = {}
    if isinstance(existing_comp, dict):
        merged.update(existing_comp)
        for field_name in _EXPORTED_COMPONENT_FIELDS:
            merged.pop(field_name, None)
    merged.update(exported_comp)
    return merged


def _extract_parent_name(node):
    parent_name = _safe_get_editor_property(node, "ParentComponentOrVariableName", "")
    if parent_name is None:
        return ""
    parent_name = str(parent_name)
    if parent_name in ("None", "None.None", ""):
        return ""
    return parent_name.split(".")[-1]


def _export_component_data(node, component_name_to_root):
    comp = _safe_get_editor_property(node, "ComponentTemplate")
    if comp is None:
        return None

    comp_name = comp.get_name()
    comp_type = _normalize_component_type_for_export(comp)
    comp_data = {
        "Type": comp_type,
        "Name": comp_name,
    }

    parent_name = _extract_parent_name(node)
    if parent_name:
        comp_data["Parent"] = parent_name

    if component_name_to_root.get(comp_name, False):
        comp_data["IsRoot"] = True

    rel_loc = _vector_to_list(_safe_get_editor_property(comp, "RelativeLocation"))
    if rel_loc and not _is_vector_close(rel_loc, [0.0, 0.0, 0.0]):
        comp_data["Location"] = rel_loc

    rel_rot = _rotator_to_list(_safe_get_editor_property(comp, "RelativeRotation"))
    if rel_rot and not _is_vector_close(rel_rot, [0.0, 0.0, 0.0]):
        comp_data["Rotation"] = rel_rot

    rel_scale = _vector_to_list(_safe_get_editor_property(comp, "RelativeScale3D"))
    if rel_scale and not _is_vector_close(rel_scale, [1.0, 1.0, 1.0]):
        comp_data["Scale"] = rel_scale

    collision_profile = _safe_get_editor_property(comp, "CollisionProfileName")
    if comp_type != "SceneComponent" and collision_profile not in (None, "", "None"):
        comp_data["CollisionProfileName"] = str(collision_profile)

    can_affect_nav = _safe_get_editor_property(comp, "bCanEverAffectNavigation", None)
    if can_affect_nav is not None and (bool(can_affect_nav) or comp_type != "SceneComponent"):
        comp_data["bCanEverAffectNavigation"] = bool(can_affect_nav)

    dynamic_obstacle = _safe_get_editor_property(comp, "bDynamicObstacle", None)
    if dynamic_obstacle is not None and bool(dynamic_obstacle):
        comp_data["bDynamicObstacle"] = bool(dynamic_obstacle)

    if isinstance(comp, unreal.StaticMeshComponent):
        mesh = _safe_get_editor_property(comp, "StaticMesh")
        if mesh:
            comp_data["Mesh"] = mesh.get_path_name()
        material = None
        try:
            material = comp.get_material(0)
        except Exception:
            material = None
        if material:
            comp_data["Material"] = material.get_path_name()

    if isinstance(comp, unreal.SkeletalMeshComponent):
        mesh = _safe_get_editor_property(comp, "SkeletalMeshAsset")
        if mesh:
            comp_data["Mesh"] = mesh.get_path_name()

        anim_mode = _safe_get_editor_property(comp, "AnimationMode")
        if anim_mode is not None:
            comp_data["AnimationMode"] = _normalize_animation_mode_for_export(anim_mode)

        anim_class = _safe_get_editor_property(comp, "AnimClass")
        if anim_class:
            comp_data["AnimClass"] = anim_class.get_path_name()

        anim_to_play = _safe_get_editor_property(comp, "AnimToPlay")
        if anim_to_play:
            comp_data["AnimToPlay"] = anim_to_play.get_path_name()

    if isinstance(comp, unreal.BoxComponent):
        ext = _safe_get_editor_property(comp, "BoxExtent")
        if ext:
            comp_data["Extent"] = _vector_to_list(ext)

    if isinstance(comp, unreal.SphereComponent):
        radius = _safe_get_editor_property(comp, "SphereRadius")
        if radius is not None:
            comp_data["Radius"] = float(radius)

    if isinstance(comp, unreal.CapsuleComponent):
        radius = _safe_get_editor_property(comp, "CapsuleRadius")
        half_height = _safe_get_editor_property(comp, "CapsuleHalfHeight")
        if radius is not None:
            comp_data["Radius"] = float(radius)
        if half_height is not None:
            comp_data["HalfHeight"] = float(half_height)

    return comp_data


def _get_component_object_from_data(bp, bfl, data):
    comp = None
    try:
        comp = bfl.get_object_for_blueprint(data, bp)
    except Exception:
        comp = None
    if comp is None:
        try:
            comp = bfl.get_associated_object(data)
        except Exception:
            comp = None
    return comp


def _get_component_name_from_data(bp, bfl, data):
    comp_name = ""
    try:
        comp_name = str(bfl.get_variable_name(data))
    except Exception:
        comp_name = ""
    if comp_name and comp_name not in ("None", "None.None"):
        return comp_name

    comp = _get_component_object_from_data(bp, bfl, data)
    if comp is not None:
        return comp.get_name()
    return ""


def _extract_parent_name_from_data(bp, bfl, data):
    parent_handle = None
    try:
        parent_handle = bfl.get_parent_handle(data)
    except Exception:
        parent_handle = None

    if not parent_handle:
        try:
            parent_handle = data.get_parent_handle()
        except Exception:
            parent_handle = None

    if not parent_handle:
        return ""

    try:
        parent_data = bfl.get_data(parent_handle)
    except Exception:
        parent_data = None

    if not parent_data:
        return ""

    try:
        if not bfl.is_component(parent_data):
            return ""
    except Exception:
        pass

    return _get_component_name_from_data(bp, bfl, parent_data)


def _is_root_component_data(data, bfl):
    try:
        return bool(bfl.is_root_component(data))
    except Exception:
        try:
            return bool(data.is_root_component())
        except Exception:
            return False


def _export_component_data_from_subobject(bp, bfl, data):
    comp = _get_component_object_from_data(bp, bfl, data)
    if comp is None:
        return None

    comp_name = _get_component_name_from_data(bp, bfl, data)
    if not comp_name:
        return None

    comp_type = _normalize_component_type_for_export(comp)
    comp_data = {
        "Type": comp_type,
        "Name": comp_name,
    }

    parent_name = _extract_parent_name_from_data(bp, bfl, data)
    if parent_name:
        comp_data["Parent"] = parent_name

    if _is_root_component_data(data, bfl):
        comp_data["IsRoot"] = True

    rel_loc = _vector_to_list(_safe_get_editor_property(comp, "RelativeLocation"))
    if rel_loc and not _is_vector_close(rel_loc, [0.0, 0.0, 0.0]):
        comp_data["Location"] = rel_loc

    rel_rot = _rotator_to_list(_safe_get_editor_property(comp, "RelativeRotation"))
    if rel_rot and not _is_vector_close(rel_rot, [0.0, 0.0, 0.0]):
        comp_data["Rotation"] = rel_rot

    rel_scale = _vector_to_list(_safe_get_editor_property(comp, "RelativeScale3D"))
    if rel_scale and not _is_vector_close(rel_scale, [1.0, 1.0, 1.0]):
        comp_data["Scale"] = rel_scale

    collision_profile = _safe_get_editor_property(comp, "CollisionProfileName")
    if comp_type != "SceneComponent" and collision_profile not in (None, "", "None"):
        comp_data["CollisionProfileName"] = str(collision_profile)

    can_affect_nav = _safe_get_editor_property(comp, "bCanEverAffectNavigation", None)
    if can_affect_nav is not None and (bool(can_affect_nav) or comp_type != "SceneComponent"):
        comp_data["bCanEverAffectNavigation"] = bool(can_affect_nav)

    dynamic_obstacle = _safe_get_editor_property(comp, "bDynamicObstacle", None)
    if dynamic_obstacle is not None and bool(dynamic_obstacle):
        comp_data["bDynamicObstacle"] = bool(dynamic_obstacle)

    if isinstance(comp, unreal.StaticMeshComponent):
        mesh = _safe_get_editor_property(comp, "StaticMesh")
        if mesh:
            comp_data["Mesh"] = mesh.get_path_name()

        material = None
        try:
            material = comp.get_material(0)
        except Exception:
            material = None
        if material:
            comp_data["Material"] = material.get_path_name()

    if isinstance(comp, unreal.SkeletalMeshComponent):
        mesh = _safe_get_editor_property(comp, "SkeletalMeshAsset")
        if mesh:
            comp_data["Mesh"] = mesh.get_path_name()

        anim_mode = _safe_get_editor_property(comp, "AnimationMode")
        if anim_mode is not None:
            comp_data["AnimationMode"] = _normalize_animation_mode_for_export(anim_mode)

        anim_class = _safe_get_editor_property(comp, "AnimClass")
        if anim_class:
            comp_data["AnimClass"] = anim_class.get_path_name()

        anim_to_play = _safe_get_editor_property(comp, "AnimToPlay")
        if anim_to_play:
            comp_data["AnimToPlay"] = anim_to_play.get_path_name()

    if isinstance(comp, unreal.BoxComponent):
        ext = _safe_get_editor_property(comp, "BoxExtent")
        if ext:
            comp_data["Extent"] = _vector_to_list(ext)

    if isinstance(comp, unreal.SphereComponent):
        radius = _safe_get_editor_property(comp, "SphereRadius")
        if radius is not None:
            comp_data["Radius"] = float(radius)

    if isinstance(comp, unreal.CapsuleComponent):
        radius = _safe_get_editor_property(comp, "CapsuleRadius")
        half_height = _safe_get_editor_property(comp, "CapsuleHalfHeight")
        if radius is not None:
            comp_data["Radius"] = float(radius)
        if half_height is not None:
            comp_data["HalfHeight"] = float(half_height)

    return comp_data


def _find_existing_component_template(bp, comp_name):
    """Find an existing component template by component name.

    This covers both components defined directly on the blueprint and
    inherited components that need child-blueprint overrides.
    """
    try:
        scs = bp.get_editor_property("SimpleConstructionScript")
        if not scs:
            scs = None
        if scs:
            for node in scs.get_all_nodes():
                comp = node.get_editor_property("ComponentTemplate")
                if comp and comp.get_name() == comp_name:
                    return comp
    except Exception:
        pass

    # Fallback: inspect the reflected subobject tree so we can override
    # inherited components such as Character's built-in Mesh component.
    try:
        subsystem = unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)
        bfl = unreal.SubobjectDataBlueprintFunctionLibrary
        if subsystem and bfl:
            handles = subsystem.k2_gather_subobject_data_for_blueprint(bp)
            for handle in handles:
                try:
                    data = bfl.get_data(handle)
                except Exception:
                    data = None

                if not data or not bfl.is_component(data):
                    continue

                variable_name = ""
                try:
                    variable_name = str(bfl.get_variable_name(data))
                except Exception:
                    variable_name = ""

                comp = None
                try:
                    comp = bfl.get_object_for_blueprint(data, bp)
                except Exception:
                    try:
                        comp = bfl.get_associated_object(data)
                    except Exception:
                        comp = None

                if not comp:
                    continue

                if variable_name == comp_name or comp.get_name() == comp_name:
                    return comp
    except Exception:
        pass

    return None


def _find_subobject_handle_by_name(bp, comp_name):
    if not IN_UE or not bp or not comp_name:
        return None

    try:
        subsystem = unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)
        bfl = unreal.SubobjectDataBlueprintFunctionLibrary
        if subsystem and bfl:
            handles = subsystem.k2_gather_subobject_data_for_blueprint(bp)
            for handle in handles:
                try:
                    data = bfl.get_data(handle)
                except Exception:
                    data = None

                if not data or not bfl.is_component(data):
                    continue

                variable_name = ""
                try:
                    variable_name = str(bfl.get_variable_name(data))
                except Exception:
                    variable_name = ""

                comp = None
                try:
                    comp = bfl.get_object_for_blueprint(data, bp)
                except Exception:
                    try:
                        comp = bfl.get_associated_object(data)
                    except Exception:
                        comp = None

                comp_object_name = comp.get_name() if comp else ""
                if variable_name == comp_name or comp_object_name == comp_name:
                    return handle
    except Exception:
        pass

    return None


def _resolve_component_class(comp_type):
    """动态解析组件类"""
    if not IN_UE:
        return None
    # 1. 先查别名
    ue_name = COMPONENT_TYPE_ALIASES.get(comp_type, comp_type)
    # 2. 尝试 getattr(unreal, ue_name)
    cls = getattr(unreal, ue_name, None)
    if cls:
        return cls
    # 3. 尝试加 Component 后缀
    if not ue_name.endswith("Component"):
        cls = getattr(unreal, ue_name + "Component", None)
        if cls:
            return cls
    # 4. load_class
    try:
        cls = unreal.load_class(None, f"/Script/Engine.{ue_name}")
        if cls:
            return cls
    except Exception:
        pass
    return None


def _add_component_v2(bp, subsystem, bfl, parent_handle, comp_data):
    """UE 5.1+ 方式添加组件到蓝图（动态反射）"""
    comp_type = comp_data.get("Type", "SceneComponent")
    comp_name = comp_data.get("Name", "NewComponent")

    comp_class = _resolve_component_class(comp_type)
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


def _set_collision_profile(comp_obj, profile_name):
    if not profile_name:
        return
    try:
        if hasattr(comp_obj, "set_collision_profile_name"):
            comp_obj.set_collision_profile_name(profile_name)
            return
    except Exception:
        pass
    for prop_name in ("CollisionProfileName", "collision_profile_name"):
        try:
            comp_obj.set_editor_property(prop_name, profile_name)
            return
        except Exception:
            pass


def _set_can_affect_navigation(comp_obj, enabled):
    try:
        if hasattr(comp_obj, "set_can_ever_affect_navigation"):
            comp_obj.set_can_ever_affect_navigation(enabled)
            return
    except Exception:
        pass
    for prop_name in ("bCanEverAffectNavigation", "can_ever_affect_navigation"):
        try:
            comp_obj.set_editor_property(prop_name, enabled)
            return
        except Exception:
            pass


def _set_animation_mode(comp_obj, mode_name):
    if not mode_name:
        return

    mode_map = {
        "UseAnimationBlueprint": unreal.AnimationMode.ANIMATION_BLUEPRINT,
        "AnimationBlueprint": unreal.AnimationMode.ANIMATION_BLUEPRINT,
        "AnimBlueprint": unreal.AnimationMode.ANIMATION_BLUEPRINT,
        "UseAnimationAsset": unreal.AnimationMode.ANIMATION_SINGLE_NODE,
        "AnimationAsset": unreal.AnimationMode.ANIMATION_SINGLE_NODE,
        "SingleNode": unreal.AnimationMode.ANIMATION_SINGLE_NODE,
        "AnimationCustomMode": unreal.AnimationMode.ANIMATION_CUSTOM_MODE,
        "CustomMode": unreal.AnimationMode.ANIMATION_CUSTOM_MODE,
    }

    mode_value = mode_map.get(str(mode_name), None)
    if mode_value is None:
        _log(f"    Unknown AnimationMode, skipped: {mode_name}")
        return

    try:
        comp_obj.set_editor_property("AnimationMode", mode_value)
    except Exception as exc:
        _log(f"    Failed to set AnimationMode {mode_name}: {exc}")


def _set_anim_class(comp_obj, class_path):
    if not class_path:
        return

    anim_class = None
    try:
        anim_class = unreal.load_class(None, class_path)
    except Exception:
        anim_class = None

    if not anim_class:
        try:
            anim_class = unreal.load_object(None, class_path)
        except Exception:
            anim_class = None

    if not anim_class:
        _log(f"    AnimClass not found, skipped: {class_path}")
        return

    try:
        comp_obj.set_editor_property("AnimClass", anim_class)
        _log(f"    设置 AnimClass: {class_path}")
    except Exception as exc:
        _log(f"    Failed to set AnimClass {class_path}: {exc}")


def _set_anim_asset(comp_obj, asset_path):
    if not asset_path:
        return

    anim_asset = unreal.load_asset(asset_path)
    if not anim_asset:
        _log(f"    AnimationAsset not found, skipped: {asset_path}")
        return

    for prop_name in ("AnimToPlay", "AnimationData"):
        try:
            comp_obj.set_editor_property(prop_name, anim_asset)
            _log(f"    设置 AnimationAsset: {asset_path}")
            return
        except Exception:
            pass


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

    if comp_type == "StaticMesh" and comp_data.get("Material"):
        mat_path = comp_data["Material"]
        mat = unreal.load_asset(mat_path)
        if mat:
            comp_obj.set_material(0, mat)
            _log(f"    设置材质: {mat_path}")
        else:
            _log(f"    材质未找到（渐进式生成，跳过）: {mat_path}")

    if comp_type == "SkeletalMesh" and comp_data.get("Mesh"):
        mesh_path = comp_data["Mesh"]
        mesh = unreal.load_asset(mesh_path)
        if mesh:
            comp_obj.set_editor_property("SkeletalMeshAsset", mesh)
            _log(f"    设置 SkeletalMesh: {mesh_path}")
        else:
            _log(f"    SkeletalMesh 未找到（渐进式生成，跳过）: {mesh_path}")

    if comp_type == "SkeletalMesh" and comp_data.get("AnimationMode"):
        _set_animation_mode(comp_obj, comp_data.get("AnimationMode"))

    if comp_type == "SkeletalMesh" and comp_data.get("AnimClass"):
        _set_anim_class(comp_obj, comp_data.get("AnimClass"))

    if comp_type == "SkeletalMesh":
        anim_asset_path = comp_data.get("AnimToPlay") or comp_data.get("AnimationAsset")
        if anim_asset_path:
            _set_anim_asset(comp_obj, anim_asset_path)

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

    if "CollisionProfileName" in comp_data:
        _set_collision_profile(comp_obj, comp_data.get("CollisionProfileName"))

    if "bCanEverAffectNavigation" in comp_data:
        _set_can_affect_navigation(comp_obj, bool(comp_data.get("bCanEverAffectNavigation")))

    # Spline 组件：设置默认点
    if comp_type == "Spline" and comp_data.get("Points"):
        points = comp_data["Points"]
        # 清除默认点，重新设置
        num_existing = comp_obj.get_number_of_spline_points()
        for i in range(len(points)):
            pt = points[i]
            pos = unreal.Vector(pt[0], pt[1], pt[2])
            if i < num_existing:
                comp_obj.set_location_at_spline_point(i, pos, unreal.SplineCoordinateSpace.LOCAL)
            else:
                comp_obj.add_spline_point(pos, unreal.SplineCoordinateSpace.LOCAL, True)
        # 删除多余的默认点
        while comp_obj.get_number_of_spline_points() > len(points):
            comp_obj.remove_spline_point(comp_obj.get_number_of_spline_points() - 1, True)
        comp_obj.update_spline()
        _log(f"    Spline 设置 {len(points)} 个点")

    # SplineMesh 组件：设置 Mesh 和材质
    if comp_type == "SplineMesh":
        if comp_data.get("Mesh"):
            mesh = unreal.load_asset(comp_data["Mesh"])
            if mesh:
                comp_obj.set_static_mesh(mesh)
        if comp_data.get("Material"):
            mat = unreal.load_asset(comp_data["Material"])
            if mat:
                comp_obj.set_material(0, mat)

    # === 通用属性：JSON 里非保留字段自动 set_editor_property ===
    for key, val in comp_data.items():
        if key in _COMP_RESERVED_FIELDS:
            continue
        try:
            if isinstance(val, bool):
                comp_obj.set_editor_property(key, val)
            elif isinstance(val, (int, float)):
                comp_obj.set_editor_property(key, val)
            elif isinstance(val, str):
                assigned = False
                if val.startswith("/Game/"):
                    asset = unreal.load_asset(val)
                    if asset:
                        comp_obj.set_editor_property(key, asset)
                        assigned = True
                    else:
                        cls = unreal.load_class(None, val)
                        if cls:
                            comp_obj.set_editor_property(key, cls)
                            assigned = True
                elif val.startswith("/Script/"):
                    cls = unreal.load_class(None, val)
                    if cls:
                        comp_obj.set_editor_property(key, cls)
                        assigned = True

                if not assigned:
                    comp_obj.set_editor_property(key, val)
        except Exception:
            pass  # 静默跳过不存在或 protected 的属性


def _resolve_unlua_binding(unlua_binding):
    if isinstance(unlua_binding, dict):
        if not unlua_binding.get("Enabled", True):
            return ""
        return str(unlua_binding.get("ModuleName", "") or "")
    if isinstance(unlua_binding, str):
        return unlua_binding
    return ""


def _set_unlua_binding(bp, unlua_binding):
    """设置 UnLua 绑定（自动实现 UnLuaInterface + GetModuleName）"""
    try:
        module_name = _resolve_unlua_binding(unlua_binding)
        if not module_name:
            return

        lib = getattr(unreal, "BPFactoryBlueprintLibrary", None)
        if not lib:
            _log("  UnLua 绑定跳过：未找到 BPFactoryBlueprintLibrary")
            return

        bind_func = None
        for func_name in ("setup_unlua_binding", "setup_un_lua_binding", "SetupUnLuaBinding"):
            candidate = getattr(lib, func_name, None)
            if callable(candidate):
                bind_func = candidate
                break

        if not bind_func:
            _log("  UnLua 绑定跳过：未找到 SetupUnLuaBinding 接口")
            return

        ok = bool(bind_func(bp, module_name))
        if ok:
            _log(f"  UnLua 绑定完成: {module_name}")
        else:
            _log(f"  UnLua 绑定失败: {module_name}")
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
    existing_template = _load_existing_template(json_path)
    template = dict(existing_template) if isinstance(existing_template, dict) else {}
    template["Name"] = bp.get_name()
    template["ParentClass"] = ""
    template["OutputPath"] = str(asset_path).rsplit("/", 1)[0] + "/"
    if "Components" not in template:
        template["Components"] = []
    if "UnLuaBinding" not in template:
        template["UnLuaBinding"] = ""

    # 父类
    parent_path = str(template.get("ParentClass", "") or "")
    parent_obj = None
    try:
        parent_obj = bp.get_editor_property("ParentClass")
    except Exception:
        parent_obj = None

    if parent_obj is None:
        generated_class = None
        try:
            generated_class = bp.generated_class()
        except Exception:
            try:
                generated_class = bp.generated_class
            except Exception:
                generated_class = None

        if generated_class is not None:
            try:
                parent_obj = generated_class.get_super_class()
            except Exception:
                try:
                    parent_obj = generated_class.get_super_struct()
                except Exception:
                    parent_obj = None

    if parent_obj:
        try:
            parent_path = parent_obj.get_path_name()
        except Exception:
            parent_path = str(parent_obj)

    if parent_path:
        for short, full in PARENT_CLASS_MAP.items():
            if full in parent_path:
                parent_path = short
                break
        template["ParentClass"] = parent_path
    elif not template.get("ParentClass"):
        template["ParentClass"] = "Actor"

    # 组件
    existing_components = template.get("Components", [])
    existing_by_name = {}
    if isinstance(existing_components, list):
        for existing_comp in existing_components:
            if isinstance(existing_comp, dict) and existing_comp.get("Name"):
                existing_by_name[existing_comp.get("Name")] = existing_comp

    exported_components = []
    exported_names = set()
    subsystem = unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)
    bfl = unreal.SubobjectDataBlueprintFunctionLibrary
    handles = subsystem.k2_gather_subobject_data_for_blueprint(bp) if subsystem and bfl else []
    for handle in handles or []:
        try:
            data = bfl.get_data(handle)
        except Exception:
            data = None
        if not data:
            continue

        try:
            if not bfl.is_component(data):
                continue
        except Exception:
            continue

        comp_data = _export_component_data_from_subobject(bp, bfl, data)
        if not comp_data:
            continue
        comp_name = comp_data.get("Name")
        if comp_name in exported_names:
            continue
        exported_names.add(comp_name)

        exported_components.append(
            _merge_component_export(existing_by_name.get(comp_name), comp_data)
        )

    ordered_components = []
    emitted_names = set()
    remaining_components = list(exported_components)

    while remaining_components:
        next_remaining = []
        progress_made = False
        for comp_data in remaining_components:
            parent_name = str(comp_data.get("Parent", "") or "")
            if not parent_name or parent_name in emitted_names or bool(comp_data.get("IsRoot", False)):
                ordered_components.append(comp_data)
                emitted_names.add(comp_data.get("Name"))
                progress_made = True
            else:
                next_remaining.append(comp_data)

        if not progress_made:
            ordered_components.extend(next_remaining)
            break

        remaining_components = next_remaining

    template["Components"] = ordered_components

    # 写入 JSON
    os.makedirs(os.path.dirname(json_path), exist_ok=True)
    with open(json_path, "w", encoding="utf-8") as f:
        json.dump(template, f, indent=2, ensure_ascii=False)

    _log(f"蓝图导出完成: {json_path} ({len(template['Components'])} 个组件)")
    return True
