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
        _log(f"  蓝图已存在，更新模式: {asset_path}")
        if preserve_existing_components:
            _log("  保留现有组件，仅更新属性/绑定")
        else:
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
    else:
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

    for comp_data in components:
        comp_type = comp_data.get("Type", "SceneComponent")
        comp_name = comp_data.get("Name", "NewComponent")

        if comp_type == "SceneComponent" and comp_name == "Root":
            # 跳过 Root SceneComponent，使用默认的 DefaultSceneRoot
            continue

        if preserve_existing_components:
            existing_comp = _find_existing_component_template(bp, comp_name)
            if existing_comp:
                _set_component_properties(existing_comp, comp_type, comp_data)
                _log(f"  Updated existing component: {comp_name} ({comp_type})")
                continue

        result = _add_component_v2(bp, subsystem, bfl, scene_root_handle, comp_data)
        if result is None:
            _log(f"  组件添加失败: {comp_name}")

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

# 需要特殊属性设置的组件类型
_COMP_RESERVED_FIELDS = {"Type", "Name", "Mesh", "Material", "Location", "Rotation", "Scale",
                          "Points", "Extent", "Radius", "HalfHeight", "AnimationMode", "AnimClass",
                          "AnimToPlay", "AnimationAsset"}


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
