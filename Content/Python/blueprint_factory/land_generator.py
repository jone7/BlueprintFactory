"""BlueprintFactory - 地形生成器
从 JSON 模板创建真正的 UE Landscape。
通过 C++ BPFactoryBlueprintLibrary.CreateLandscape 创建。

JSON 模板格式:
{
    "Size": {"X": 127, "Y": 127},
    "Scale": {"X": 100, "Y": 100, "Z": 100},
    "Location": {"X": 0, "Y": 0, "Z": 0},
    "HeightData": "flat",
    "Layers": [
        {"Name": "Grass", "Material": "/Game/Art/Materials/MI_Ground_Farm"}
    ]
}

HeightData 可以是:
- "flat" — 平坦地形
- "file:path/to/heightmap.raw" — 从 RAW 文件读取（uint16 数组）
- [32768, 32768, ...] — 直接内嵌数组
"""
import json
import os
import struct

try:
    import unreal
    IN_UE = True
except ImportError:
    IN_UE = False


def _log(msg):
    if IN_UE:
        unreal.log(f"[LandFactory] {msg}")
    else:
        print(f"[LandFactory] {msg}")


def _log_error(msg):
    if IN_UE:
        unreal.log_error(f"[LandFactory] {msg}")
    else:
        print(f"[LandFactory ERROR] {msg}")


def generate_landscape(json_path: str):
    """从 JSON 模板生成地形"""
    if not os.path.isfile(json_path):
        _log_error(f"JSON 文件不存在: {json_path}")
        return False

    with open(json_path, "r", encoding="utf-8") as f:
        template = json.load(f)

    size = template.get("Size", {"X": 127, "Y": 127})
    scale = template.get("Scale", {"X": 100, "Y": 100, "Z": 100})
    location = template.get("Location", {"X": 0, "Y": 0, "Z": 0})
    height_data_spec = template.get("HeightData", "flat")
    layers = template.get("Layers", [])

    size_x = int(size["X"])
    size_y = int(size["Y"])

    if not IN_UE:
        _log(f"非 UE 环境，跳过地形生成: {size_x}x{size_y}")
        return False

    _log(f"生成地形: {size_x}x{size_y}, 缩放=({scale['X']},{scale['Y']},{scale['Z']})")

    # 准备高度数据
    height_array = _prepare_height_data(height_data_spec, size_x, size_y, json_path)

    # 调用 C++ 创建 Landscape
    loc = unreal.Vector(location["X"], location["Y"], location["Z"])

    bplib = unreal.BPFactoryBlueprintLibrary
    landscape = bplib.create_landscape(
        unreal.EditorLevelLibrary.get_editor_world(),
        loc,
        size_x, size_y,
        float(scale["X"]), float(scale["Y"]), float(scale["Z"]),
        height_array
    )

    if not landscape:
        _log_error("Landscape 创建失败")
        return False

    _log(f"Landscape 创建成功")

    # 设置材质层（如果有）
    for layer in layers:
        mat_path = layer.get("Material", "")
        if mat_path:
            _log(f"  层: {layer.get('Name', '?')} → {mat_path}")
            # 材质层设置需要 LandscapeMaterialInterface，后续完善

    return True


def _prepare_height_data(spec, size_x, size_y, json_path):
    """准备高度数据数组"""
    total = size_x * size_y

    if spec == "flat":
        # 平坦地形，中间高度
        return [32768] * total

    if isinstance(spec, str) and spec.startswith("file:"):
        # 从 RAW 文件读取
        file_path = spec[5:]
        if not os.path.isabs(file_path):
            file_path = os.path.join(os.path.dirname(json_path), file_path)

        if os.path.isfile(file_path):
            with open(file_path, "rb") as f:
                raw = f.read()
            count = len(raw) // 2
            heights = list(struct.unpack(f"<{count}H", raw[:count * 2]))
            if len(heights) >= total:
                return heights[:total]
            else:
                _log(f"  高度图数据不足: {len(heights)} < {total}，补齐为平坦")
                heights.extend([32768] * (total - len(heights)))
                return heights
        else:
            _log(f"  高度图文件不存在: {file_path}，使用平坦")
            return [32768] * total

    if isinstance(spec, list):
        # 直接内嵌数组
        if len(spec) >= total:
            return [int(v) for v in spec[:total]]
        else:
            data = [int(v) for v in spec]
            data.extend([32768] * (total - len(data)))
            return data

    # 默认平坦
    return [32768] * total


def export_landscape(json_path: str):
    """导出当前关卡中的 Landscape 为 JSON（基本信息）"""
    if not IN_UE:
        _log("非 UE 环境，无法导出")
        return False

    actors = unreal.EditorLevelLibrary.get_all_level_actors()
    landscape = None
    for actor in actors:
        if isinstance(actor, unreal.LandscapeProxy):
            landscape = actor
            break

    if not landscape:
        _log_error("当前关卡中没有 Landscape")
        return False

    loc = landscape.get_actor_location()
    scale = landscape.get_actor_scale3d()

    template = {
        "Size": {"X": 127, "Y": 127},
        "Scale": {"X": round(scale.x, 2), "Y": round(scale.y, 2), "Z": round(scale.z, 2)},
        "Location": {"X": round(loc.x, 1), "Y": round(loc.y, 1), "Z": round(loc.z, 1)},
        "HeightData": "flat",
        "Layers": [],
    }

    os.makedirs(os.path.dirname(json_path), exist_ok=True)
    with open(json_path, "w", encoding="utf-8") as f:
        json.dump(template, f, indent=2, ensure_ascii=False)

    _log(f"Landscape 导出完成: {json_path}")
    return True
