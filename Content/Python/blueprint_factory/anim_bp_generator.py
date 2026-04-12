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
        _apply_state_machine_definition(existing, template)
        if unlua_binding:
            _set_unlua_binding(existing, unlua_binding)
        _compile_blueprint(existing)
        unreal.EditorAssetLibrary.save_asset(asset_path)
        return True

    parent_class = _load_class(parent_class_path)
    if not parent_class:
        _log_error(f"Failed to load parent class: {parent_class_path}")
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

    _apply_state_machine_definition(anim_bp, template)
    if unlua_binding:
        _set_unlua_binding(anim_bp, unlua_binding)

    _compile_blueprint(anim_bp)
    unreal.EditorAssetLibrary.save_asset(asset_path)
    _log(f"AnimBlueprint generated: {asset_path}")
    return True
