"""Editor-state guards shared by BlueprintFactory generators."""

try:
    import unreal
    IN_UE = True
except ImportError:
    IN_UE = False


def _log(msg):
    if IN_UE:
        unreal.log(f"[BPFactoryGuard] {msg}")
    else:
        print(f"[BPFactoryGuard] {msg}")


def _log_error(msg):
    if IN_UE:
        unreal.log_error(f"[BPFactoryGuard] {msg}")
    else:
        print(f"[BPFactoryGuard ERROR] {msg}")


def _asset_exists(asset_path: str) -> bool:
    if not asset_path or not IN_UE:
        return False

    try:
        if unreal.EditorAssetLibrary.does_asset_exist(asset_path):
            return True
    except Exception:
        pass

    try:
        return unreal.load_asset(asset_path) is not None
    except Exception:
        return False


def ensure_editor_not_playing_for_existing_asset(asset_path: str) -> bool:
    """Stop PIE before a generator overwrites an existing asset."""
    if not IN_UE or not _asset_exists(asset_path):
        return True

    lib = getattr(unreal, "BPFactoryBlueprintLibrary", None)
    if lib:
        for func_name in (
            "ensure_editor_not_playing_for_existing_asset",
            "EnsureEditorNotPlayingForExistingAsset",
        ):
            func = getattr(lib, func_name, None)
            if callable(func):
                return bool(func(asset_path))

    editor_level_library = getattr(unreal, "EditorLevelLibrary", None)
    end_play = getattr(editor_level_library, "editor_end_play", None)
    if callable(end_play):
        try:
            _log(f"Existing asset detected; requesting PIE stop before overwrite: {asset_path}")
            end_play()
            return True
        except Exception as exc:
            _log_error(f"Failed to request PIE stop before overwrite: {asset_path}: {exc}")
            return False

    _log("PIE guard helper is unavailable; continuing without direct play-state control.")
    return True
