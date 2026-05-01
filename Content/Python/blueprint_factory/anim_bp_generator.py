"""BlueprintFactory - AnimBlueprint generator from JSON templates."""
import json
import os

try:
    import unreal
    IN_UE = True
except ImportError:
    IN_UE = False


def _log(msg):
    if IN_UE:
        unreal.log(f"[AnimBPFactory] {msg}")
    else:
        print(f"[AnimBPFactory] {msg}")


def _log_error(msg):
    if IN_UE:
        unreal.log_error(f"[AnimBPFactory] {msg}")
    else:
        print(f"[AnimBPFactory ERROR] {msg}")


def _load_existing_template(json_path):
    if not json_path or not os.path.isfile(json_path):
        return {}
    try:
        with open(json_path, "r", encoding="utf-8") as handle:
            loaded = json.load(handle)
        return loaded if isinstance(loaded, dict) else {}
    except Exception as exc:
        _log(f"  Failed to read existing template, falling back to full export: {exc}")
        return {}


def _resolve_unlua_binding(unlua_binding):
    if isinstance(unlua_binding, dict):
        if not unlua_binding.get("Enabled", True):
            return ""
        return str(unlua_binding.get("ModuleName", "") or "")
    if isinstance(unlua_binding, str):
        return unlua_binding
    return ""


def _set_unlua_binding(bp, unlua_binding):
    try:
        module_name = _resolve_unlua_binding(unlua_binding)
        if not module_name:
            return

        lib = getattr(unreal, "BPFactoryBlueprintLibrary", None)
        if not lib:
            _log("  UnLua binding skipped: missing BPFactoryBlueprintLibrary")
            return

        bind_func = None
        for func_name in ("setup_unlua_binding", "setup_un_lua_binding", "SetupUnLuaBinding"):
            candidate = getattr(lib, func_name, None)
            if callable(candidate):
                bind_func = candidate
                break

        if not bind_func:
            _log("  UnLua binding skipped: SetupUnLuaBinding not found")
            return

        ok = bool(bind_func(bp, module_name))
        if ok:
            _log(f"  UnLua binding applied: {module_name}")
        else:
            _log(f"  UnLua binding failed: {module_name}")
    except Exception as exc:
        _log(f"  UnLua binding error: {exc}")


def _load_class(path):
    if not path:
        return None

    try:
        cls = unreal.load_class(None, path)
        if cls:
            return cls
    except Exception:
        pass

    try:
        cls = unreal.load_object(None, path)
        if cls:
            return cls
    except Exception:
        pass

    return None


def _load_asset(path):
    if not path:
        return None

    try:
        asset = unreal.load_asset(path)
        if asset:
            return asset
    except Exception:
        pass

    try:
        asset = unreal.load_object(None, path)
        if asset:
            return asset
    except Exception:
        pass

    return None


def _asset_package_path(asset):
    if not asset:
        return ""
    try:
        path_name = asset.get_path_name()
    except Exception:
        return ""
    if "." in path_name:
        return path_name.rsplit(".", 1)[0]
    return path_name


def _compile_blueprint(bp):
    try:
        unreal.KismetSystemLibrary.compile_blueprint(bp)
        return
    except Exception:
        pass

    try:
        unreal.BlueprintEditorLibrary.compile_blueprint(bp)
        return
    except Exception as exc:
        _log(f"  Compile warning: {exc}")


def _build_legacy_quadruped_definition(setup):
    machine_name = str(setup.get("StateMachineName", "QuadrupedLocomotion") or "QuadrupedLocomotion")
    return {
        "StateMachineName": machine_name,
        "EntryState": "Idle",
        "DefaultCrossfade": 0.18,
        "Variables": [
            {"Name": "Speed", "Type": "float"},
            {"Name": "Direction", "Type": "float"},
            {"Name": "MoveAlpha", "Type": "float"},
            {"Name": "IdleAlpha", "Type": "float"},
            {"Name": "bIsMoving", "Type": "bool"},
            {"Name": "bIsGrounded", "Type": "bool"},
            {"Name": "bIsAirborne", "Type": "bool"},
            {"Name": "bIsSitting", "Type": "bool"},
            {"Name": "bIsSleeping", "Type": "bool"},
            {"Name": "bIsSniffing", "Type": "bool"},
            {"Name": "bIsAlert", "Type": "bool"},
        ],
        "States": [
            {"Name": "Idle", "AnimationAsset": str(setup.get("IdleAnimationAsset", "") or ""), "NodePosition": {"X": 80, "Y": -80}},
            {"Name": "Walk", "AnimationAsset": str(setup.get("WalkAnimationAsset", "") or ""), "NodePosition": {"X": 420, "Y": -80}},
            {"Name": "Sit", "AnimationAsset": str(setup.get("SitAnimationAsset", "") or ""), "NodePosition": {"X": 80, "Y": 220}},
            {"Name": "Sleep", "AnimationAsset": str(setup.get("SleepAnimationAsset", "") or ""), "NodePosition": {"X": -240, "Y": 220}},
            {"Name": "Sniff", "AnimationAsset": str(setup.get("SniffAnimationAsset", "") or ""), "NodePosition": {"X": 420, "Y": 220}},
        ],
        "Transitions": [
            {"From": "Idle", "To": "Walk", "Priority": 3, "Condition": {"Variable": "bIsMoving", "Value": True}},
            {"From": "Walk", "To": "Idle", "Priority": 0, "Condition": {"Variable": "bIsMoving", "Value": False}},
            {"From": "Idle", "To": "Sit", "Priority": 2, "Condition": {"Variable": "bIsSitting", "Value": True}},
            {"From": "Sit", "To": "Idle", "Priority": 0, "Condition": {"Variable": "bIsSitting", "Value": False}},
            {"From": "Idle", "To": "Sleep", "Priority": 0, "Condition": {"Variable": "bIsSleeping", "Value": True}},
            {"From": "Sleep", "To": "Idle", "Priority": 0, "Condition": {"Variable": "bIsSleeping", "Value": False}},
            {"From": "Idle", "To": "Sniff", "Priority": 1, "Condition": {"Variable": "bIsSniffing", "Value": True}},
            {"From": "Sniff", "To": "Idle", "Priority": 0, "Condition": {"Variable": "bIsSniffing", "Value": False}},
        ],
    }


def _resolve_state_machine_definition(template):
    setup = template.get("StateMachineDefinition")
    if isinstance(setup, dict):
        return setup

    legacy_setup = template.get("QuadrupedStateMachine")
    if isinstance(legacy_setup, dict):
        return _build_legacy_quadruped_definition(legacy_setup)

    return None


def _apply_state_machine_definition(bp, template):
    setup = _resolve_state_machine_definition(template)
    if not isinstance(setup, dict):
        return

    lib = getattr(unreal, "BPFactoryBlueprintLibrary", None)
    if not lib:
        _log("  State machine skipped: missing BPFactoryBlueprintLibrary")
        return

    func = getattr(lib, "setup_anim_state_machine_from_json", None)
    if not callable(func):
        func = getattr(lib, "SetupAnimStateMachineFromJson", None)
    if not callable(func):
        _log("  State machine skipped: SetupAnimStateMachineFromJson not found")
        return

    ok = bool(func(bp, json.dumps(setup, ensure_ascii=False)))
    if ok:
        _log("  State machine definition applied")
    else:
        _log("  State machine definition apply failed")


def _apply_animation_overrides(bp, template):
    overrides = template.get("AnimationOverrides")
    if not isinstance(overrides, list):
        return

    lib = getattr(unreal, "BPFactoryBlueprintLibrary", None)
    if not lib:
        _log("  Animation overrides skipped: missing BPFactoryBlueprintLibrary")
        return

    func = getattr(lib, "setup_anim_asset_overrides_from_json", None)
    if not callable(func):
        func = getattr(lib, "SetupAnimAssetOverridesFromJson", None)
    if not callable(func):
        _log("  Animation overrides skipped: SetupAnimAssetOverridesFromJson not found")
        return

    ok = bool(func(bp, json.dumps(overrides, ensure_ascii=False)))
    if ok:
        _log("  Animation overrides applied")
    else:
        _log("  Animation overrides apply failed")


def _apply_anim_blueprint_properties(bp, is_template, target_skeleton=None, preview_mesh=None):
    if not bp:
        return

    try:
        bp.set_editor_property("bTemplate", bool(is_template))
    except Exception:
        pass

    if target_skeleton:
        try:
            bp.set_editor_property("TargetSkeleton", target_skeleton)
        except Exception as exc:
            _log(f"  TargetSkeleton update skipped: {exc}")

    if preview_mesh:
        lib = getattr(unreal, "BPFactoryBlueprintLibrary", None)
        set_preview_func = None
        if lib:
            set_preview_func = getattr(lib, "set_anim_blueprint_preview_mesh", None)
            if not callable(set_preview_func):
                set_preview_func = getattr(lib, "SetAnimBlueprintPreviewMesh", None)
        if callable(set_preview_func):
            try:
                ok = bool(set_preview_func(bp, preview_mesh, False))
                if not ok:
                    _log("  PreviewSkeletalMesh update skipped: helper returned false")
            except Exception as exc:
                _log(f"  PreviewSkeletalMesh helper failed: {exc}")
        else:
            try:
                bp.set_editor_property("PreviewSkeletalMesh", preview_mesh)
            except Exception as exc:
                _log(f"  PreviewSkeletalMesh update skipped: {exc}")


def _export_anim_blueprint_metadata(bp):
    if not bp:
        return {}

    lib = getattr(unreal, "BPFactoryBlueprintLibrary", None)
    if not lib:
        return {}

    func = getattr(lib, "export_anim_blueprint_metadata_to_json", None)
    if not callable(func):
        func = getattr(lib, "ExportAnimBlueprintMetadataToJson", None)
    if not callable(func):
        return {}

    try:
        payload = func(bp)
    except Exception as exc:
        _log(f"  AnimBlueprint metadata export failed: {exc}")
        return {}

    if not payload:
        return {}

    try:
        parsed = json.loads(payload)
        return parsed if isinstance(parsed, dict) else {}
    except Exception as exc:
        _log(f"  AnimBlueprint metadata parse failed: {exc}")
        return {}


def generate_anim_blueprint(json_path: str):
    """Generate an AnimBlueprint from a JSON template."""
    if not os.path.isfile(json_path):
        _log_error(f"JSON file not found: {json_path}")
        return False

    with open(json_path, "r", encoding="utf-8") as handle:
        template = json.load(handle)

    name = template.get("Name", "ABP_Generated")
    parent_class_path = template.get("ParentClass", "/Script/Engine.AnimInstance")
    output_path = template.get("OutputPath", "/Game/Blueprints/Characters/AnimInstance/")
    target_skeleton_path = template.get("TargetSkeleton", "")
    preview_mesh_path = template.get("PreviewSkeletalMesh", "")
    unlua_binding = template.get("UnLuaBinding", "")
    is_template = bool(template.get("bTemplate", False))
    reset_existing_asset = bool(template.get("ResetExistingAsset", False))
    reset_existing_variables = bool(template.get("ResetExistingVariables", reset_existing_asset))

    if not IN_UE:
        _log(f"Non-UE environment, skipping generation: {name}")
        return False

    target_skeleton = None
    if target_skeleton_path:
        target_skeleton = _load_asset(target_skeleton_path)
        if not target_skeleton:
            _log_error(f"Failed to load target skeleton: {target_skeleton_path}")
            return False
    elif not is_template:
        _log_error("Child AnimBlueprint templates require TargetSkeleton")
        return False

    preview_mesh = None
    if preview_mesh_path:
        preview_mesh = _load_asset(preview_mesh_path)
        if not preview_mesh:
            _log(f"Preview mesh not found, continuing without it: {preview_mesh_path}")

    asset_path = output_path + name
    existing = unreal.load_asset(asset_path)
    if existing:
        if not isinstance(existing, unreal.AnimBlueprint):
            _log_error(f"Existing asset is not an AnimBlueprint: {asset_path}")
            return False
        _log(f"AnimBlueprint already exists, reusing: {asset_path}")
        if reset_existing_asset:
            lib = getattr(unreal, "BPFactoryBlueprintLibrary", None)
            reset_func = None
            if lib:
                reset_func = getattr(lib, "reset_anim_blueprint_for_regeneration", None)
                if not callable(reset_func):
                    reset_func = getattr(lib, "ResetAnimBlueprintForRegeneration", None)
            if callable(reset_func):
                ok = bool(reset_func(existing, reset_existing_variables))
                if ok:
                    _log("  Cleared existing AnimBlueprint content before regeneration")
                else:
                    _log("  Failed to clear existing AnimBlueprint content, continuing with overwrite attempt")
            else:
                _log("  ResetAnimBlueprintForRegeneration not available, continuing with overwrite attempt")
        _apply_anim_blueprint_properties(existing, is_template, target_skeleton, preview_mesh)
        _apply_state_machine_definition(existing, template)
        _apply_animation_overrides(existing, template)
        if unlua_binding:
            _set_unlua_binding(existing, unlua_binding)
        _compile_blueprint(existing)
        unreal.EditorAssetLibrary.save_asset(asset_path)
        return True

    parent_class = _load_class(parent_class_path)
    if not parent_class:
        _log_error(f"Failed to load parent class: {parent_class_path}")
        return False

    factory = unreal.AnimBlueprintFactory()
    factory.set_editor_property("ParentClass", parent_class)
    factory.set_editor_property("bTemplate", is_template)
    if target_skeleton:
        factory.set_editor_property("TargetSkeleton", target_skeleton)
    if preview_mesh:
        factory.set_editor_property("PreviewSkeletalMesh", preview_mesh)

    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    anim_bp = asset_tools.create_asset(name, output_path, unreal.AnimBlueprint, factory)
    if not anim_bp:
        _log_error(f"Failed to create AnimBlueprint: {asset_path}")
        return False

    _apply_anim_blueprint_properties(anim_bp, is_template, target_skeleton, preview_mesh)
    _apply_state_machine_definition(anim_bp, template)
    _apply_animation_overrides(anim_bp, template)
    if unlua_binding:
        _set_unlua_binding(anim_bp, unlua_binding)

    _compile_blueprint(anim_bp)
    unreal.EditorAssetLibrary.save_asset(asset_path)
    _log(f"AnimBlueprint generated: {asset_path}")
    return True


def export_anim_blueprint(asset_path: str, json_path: str):
    """Export an existing AnimBlueprint into a JSON template."""
    if not IN_UE:
        _log("Non-UE environment, cannot export AnimBlueprint")
        return False

    bp = unreal.load_asset(asset_path)
    if not bp or not isinstance(bp, unreal.AnimBlueprint):
        _log_error(f"Failed to load AnimBlueprint: {asset_path}")
        return False

    _log(f"Export AnimBlueprint: {asset_path}")
    existing_template = _load_existing_template(json_path)
    template = dict(existing_template) if isinstance(existing_template, dict) else {}
    template["Name"] = bp.get_name()
    template["OutputPath"] = str(asset_path).rsplit("/", 1)[0] + "/"

    parent_class_path = str(template.get("ParentClass", "") or "")
    try:
        parent_class = bp.get_editor_property("ParentClass")
    except Exception:
        parent_class = None
    if parent_class:
        try:
            parent_class_path = parent_class.get_path_name()
        except Exception:
            parent_class_path = str(parent_class)
    if parent_class_path:
        template["ParentClass"] = parent_class_path

    try:
        template["bTemplate"] = bool(bp.get_editor_property("bTemplate"))
    except Exception:
        pass

    try:
        target_skeleton = bp.get_editor_property("TargetSkeleton")
    except Exception:
        target_skeleton = None
    if target_skeleton:
        try:
            template["TargetSkeleton"] = _asset_package_path(target_skeleton)
        except Exception:
            pass
    else:
        template.pop("TargetSkeleton", None)

    try:
        preview_mesh = bp.get_editor_property("PreviewSkeletalMesh")
    except Exception:
        preview_mesh = None
    if preview_mesh:
        try:
            template["PreviewSkeletalMesh"] = _asset_package_path(preview_mesh)
        except Exception:
            pass
    else:
        template.pop("PreviewSkeletalMesh", None)

    metadata = _export_anim_blueprint_metadata(bp)
    if metadata.get("StateMachineDefinition"):
        template["StateMachineDefinition"] = metadata["StateMachineDefinition"]
    else:
        template.pop("StateMachineDefinition", None)

    if metadata.get("AnimationOverrides"):
        template["AnimationOverrides"] = metadata["AnimationOverrides"]
    else:
        template.pop("AnimationOverrides", None)

    if metadata.get("UnLuaBinding"):
        template["UnLuaBinding"] = metadata["UnLuaBinding"]

    os.makedirs(os.path.dirname(json_path), exist_ok=True)
    with open(json_path, "w", encoding="utf-8") as handle:
        json.dump(template, handle, indent=2, ensure_ascii=False)

    _log(f"AnimBlueprint export complete: {json_path}")
    return True
