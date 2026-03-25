"""BlueprintFactory - 地形生成器
从 JSON 模板 + 高度图导入 Landscape。

JSON 模板格式:
{
    "Heightmap": "Config/LandscapeTemplates/heightmap_main.png",
    "Size": {"X": 200, "Y": 200},
    "Scale": {"X": 200, "Y": 200, "Z": 100},
    "Location": {"X": 0, "Y": 0, "Z": 0},
    "Layers": [
        {
            "Name": "Farm",
            "Material": "/Game/Art/Materials/Landscape/MI_Ground_Farm",
            "Region": {"MinX": 50, "MaxX": 100, "MinY": 50, "MaxY": 100}
        }
    ]
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
        unreal.log(f"[LandFactory] {msg}")
    else:
        print(f"[LandFactory] {msg}")


def _log_error(msg):
    if IN_UE:
        unreal.log_error(f"[LandFactory] {msg}")
    else:
        print(f"[LandFactory ERROR] {msg}")


def generate_landscape(json_path: str):
    """从 JSON 模板导入地形"""
    if not os.path.isfile(json_path):
        _log_error(f"JSON 文件不存在: {json_path}")
        return False

    with open(json_path, "r", encoding="utf-8") as f:
        template = json.load(f)

    heightmap_path = template.get("Heightmap", "")
    size = template.get("Size", {"X": 127, "Y": 127})
    scale = template.get("Scale", {"X": 100, "Y": 100, "Z": 100})
    location = template.get("Location", {"X": 0, "Y": 0, "Z": 0})
    layers = template.get("Layers", [])

    # 高度图路径相对于 JSON 文件所在目录
    if heightmap_path and not os.path.isabs(heightmap_path):
        json_dir = os.path.dirname(json_path)
        heightmap_path = os.path.join(json_dir, heightmap_path)

    if not IN_UE:
        _log(f"非 UE 环境，跳过地形生成")
        _log(f"  高度图: {heightmap_path}")
        _log(f"  尺寸: {size['X']}x{size['Y']}")
        _log(f"  缩放: {scale}")
        _log(f"  层数: {len(layers)}")
        return False

    _log(f"导入地形: {size['X']}x{size['Y']}")

    # 检查高度图
    if heightmap_path and os.path.isfile(heightmap_path):
        _log(f"  高度图: {heightmap_path}")
        _import_with_heightmap(heightmap_path, size, scale, location, layers)
    else:
        _log(f"  无高度图，创建平坦地形")
        _create_flat_landscape(size, scale, location, layers)

    return True


def _import_with_heightmap(heightmap_path, size, scale, location, layers):
    """从高度图导入地形"""
    # UE 的 Landscape 导入需要通过编辑器工具
    # 使用 LandscapeEditorUtils 或 EditorLevelLibrary
    _log("  高度图导入功能待完善（需要 LandscapeEditorUtils API）")
    _log("  临时方案：请在编辑器中手动导入高度图，然后用本工具刷材质层")

    # 记录层信息供后续使用
    for layer in layers:
        _log(f"  层: {layer['Name']} → {layer.get('Material', 'default')}")


def _create_flat_landscape(size, scale, location, layers):
    """创建平坦地形（无高度图时的回退方案）"""
    _log("  创建平坦地形...")

    # 使用 EditorLevelLibrary 在场景中生成 Landscape
    # 注意：UE Python API 对 Landscape 的支持有限
    # 最可靠的方式是通过 EditorLevelLibrary.spawn_actor_from_class
    try:
        loc = unreal.Vector(location["X"], location["Y"], location["Z"])
        rot = unreal.Rotator(0, 0, 0)
        sc = unreal.Vector(scale["X"], scale["Y"], scale["Z"])

        _log(f"  位置: {loc}")
        _log(f"  缩放: {sc}")
        _log(f"  地形创建需要在编辑器中完成（Python API 限制）")
        _log(f"  建议：编辑器 → Landscape 工具 → 新建 → 设置尺寸 {size['X']}x{size['Y']}")

        # 输出层配置供手动参考
        for layer in layers:
            region = layer.get("Region", {})
            _log(f"  层 '{layer['Name']}': 材质={layer.get('Material', 'default')}, "
                 f"区域=({region.get('MinX', 0)},{region.get('MinY', 0)})-"
                 f"({region.get('MaxX', 0)},{region.get('MaxY', 0)})")

    except Exception as e:
        _log_error(f"  地形创建失败: {e}")
