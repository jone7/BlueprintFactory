# -*- coding: utf-8 -*-
"""BlueprintFactory - Landscape Generator
Read JSON config -> generate .r16 heightmap -> prompt user to import.
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
        unreal.log("[LandFactory] " + msg)
    else:
        print("[LandFactory] " + msg)


def _log_error(msg):
    if IN_UE:
        unreal.log_error("[LandFactory] " + msg)
    else:
        print("[LandFactory ERROR] " + msg)


def generate_landscape(json_path):
    """Read JSON, generate heightmap .r16, log import instructions.
    支持两种模式：
    1. Regions 模式：从 JSON regions 生成高度图
    2. Image 模式：从灰度图片转换高度图（JSON 里有 sourceImage 字段）
    """
    if not os.path.isfile(json_path):
        _log_error("JSON not found: " + json_path)
        return False

    with open(json_path, "r", encoding="utf-8") as f:
        cfg = json.load(f)

    size = cfg.get("Size", 505)
    scale = cfg.get("Scale", {"X": 100, "Y": 100, "Z": 100})
    location = cfg.get("Location", {"X": 0, "Y": 0, "Z": 0})
    source_image = cfg.get("sourceImage", "")

    _log("Config: %dx%d, Scale=(%s,%s,%s)" % (
        size, size, scale["X"], scale["Y"], scale["Z"]))

    r16_path = None

    if source_image:
        # Image 模式：从灰度图片转换
        # sourceImage 路径相对于项目根目录
        img_path = source_image
        if not os.path.isabs(img_path):
            project_dir = os.path.dirname(os.path.dirname(os.path.dirname(
                os.path.dirname(os.path.dirname(os.path.abspath(json_path))))))
            # 尝试相对于 JSON 所在目录
            img_path_rel = os.path.join(os.path.dirname(json_path), source_image)
            if os.path.isfile(img_path_rel):
                img_path = img_path_rel
            elif os.path.isfile(source_image):
                img_path = source_image

        if not os.path.isfile(img_path):
            _log_error("Source image not found: " + source_image)
            return False

        height_scale = cfg.get("heightScale", 0.15)
        blur_radius = cfg.get("blurRadius", 3)
        _log("Image mode: %s, scale=%.2f, blur=%d" % (img_path, height_scale, blur_radius))

        from blueprint_factory.heightmap_generator import convert_image_to_r16
        r16_path = convert_image_to_r16(img_path, size, None, height_scale, blur_radius=blur_radius)
    else:
        # Regions 模式：从 JSON regions 生成
        from blueprint_factory.heightmap_generator import generate_heightmap
        r16_path = generate_heightmap(json_path)

    if not r16_path:
        _log_error("Heightmap generation failed")
        return False

    _log("========================================")
    _log("Heightmap generated: " + r16_path)
    _log("Please import in Landscape tool:")
    _log("  1. Landscape Mode -> Import tab")
    _log("  2. Select file: " + r16_path)
    _log("  3. Scale: %s, %s, %s" % (scale["X"], scale["Y"], scale["Z"]))
    _log("  4. Location: %s, %s, %s" % (location["X"], location["Y"], location["Z"]))
    _log("========================================")

    return True


def export_landscape(json_path):
    """Export current landscape info to JSON."""
    if not IN_UE:
        _log("Not in UE, cannot export")
        return False

    actors = unreal.EditorLevelLibrary.get_all_level_actors()
    landscape = None
    for actor in actors:
        if isinstance(actor, unreal.LandscapeProxy):
            landscape = actor
            break

    if not landscape:
        _log_error("No Landscape in current level")
        return False

    loc = landscape.get_actor_location()
    scale = landscape.get_actor_scale3d()

    template = {
        "Size": 505,
        "Scale": {"X": round(scale.x, 2), "Y": round(scale.y, 2), "Z": round(scale.z, 2)},
        "Location": {"X": round(loc.x, 1), "Y": round(loc.y, 1), "Z": round(loc.z, 1)},
        "OutputName": "exported_landscape",
        "Regions": {},
        "GlobalNoise": [],
    }

    os.makedirs(os.path.dirname(json_path), exist_ok=True)
    with open(json_path, "w", encoding="utf-8") as f:
        json.dump(template, f, indent=2, ensure_ascii=False)

    _log("Landscape exported: " + json_path)
    return True
