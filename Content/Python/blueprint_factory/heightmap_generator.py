# -*- coding: utf-8 -*-
"""BlueprintFactory - Heightmap Generator (Data-Driven)

JSON Regions format - each region has a "type" field:

"Regions": [
    {"type": "plateau", "center": [0.5, 0.25], "radius": 0.15, "height": 200},
    {"type": "bumps", "bounds": [0, 0.2, 0.35, 0.6], "height": 100, "amplitude": 150},
    {"type": "river", "centerX": 0.78, "width": 0.07, "depth": -1500, "waveFreq": 4, "waveAmp": 0.03},
    {"type": "hills", "bounds": [0, 0.5, 0.35, 0.8], "amplitude": 2000, "freq": 2.5},
    {"type": "peaks", "startY": 0.65, "peaks": [{"x":0.5,"height":5000,"width":3}], "ridgeHeight": 1500},
    {"type": "depression", "bounds": [0.6, 0.7, 0.75, 1.0], "depth": -1000},
    {"type": "lake", "center": [0.65, 0.45], "radius": 0.1, "depth": -1000},
    {"type": "slope", "bounds": [0, 0, 1, 0.2], "depth": -500},
    {"type": "noise", "freqX": 5, "freqY": 3, "amplitude": 150, "func": "sin"}
]
"""
import json
import math
import os
import struct
import time

try:
    import unreal
    IN_UE = True
except ImportError:
    IN_UE = False


def _log(msg):
    if IN_UE:
        unreal.log("[HeightmapGen] " + msg)
    else:
        print("[HeightmapGen] " + msg)


MID = 32768


def generate_heightmap(json_path):
    if not os.path.isfile(json_path):
        _log("JSON not found: " + json_path)
        return None

    with open(json_path, "r", encoding="utf-8") as f:
        cfg = json.load(f)

    size = cfg.get("Size", 127)
    regions_raw = cfg.get("Regions", {})

    # Support both dict (old format) and list (new format)
    if isinstance(regions_raw, dict):
        regions = _convert_dict_to_list(regions_raw)
    else:
        regions = regions_raw

    # GlobalNoise as extra regions
    for n in cfg.get("GlobalNoise", []):
        n["type"] = "noise"
        regions.append(n)

    _log("Generating: %dx%d, %d regions" % (size, size, len(regions)))

    data = bytearray()
    for y in range(size):
        for x in range(size):
            cx = x / max(size - 1.0, 1)
            cy = y / max(size - 1.0, 1)
            h = 0.0
            for region in regions:
                h += _apply_region(cx, cy, region)
            val = max(0, min(65535, int(MID + h)))
            data += struct.pack('<H', val)

    timestamp = time.strftime("%Y%m%d_%H%M%S")
    out_dir = os.path.dirname(json_path)
    out_name = cfg.get("OutputName", "heightmap") + "_" + timestamp + ".r16"
    out_path = os.path.join(out_dir, out_name)

    with open(out_path, 'wb') as f:
        f.write(data)

    _log("Done: %s (%d bytes)" % (out_path, len(data)))
    return out_path


def _convert_dict_to_list(regions_dict):
    """Convert old dict format to new list format."""
    result = []
    type_map = {
        "restaurant": "plateau",
        "farm": "bumps",
        "river": "river",
        "bamboo": "hills",
        "mountain": "peaks",
        "valley": "depression",
        "lake": "lake",
        "ocean": "slope",
    }
    for name, data in regions_dict.items():
        region = dict(data)
        if "type" not in region:
            region["type"] = type_map.get(name, name)
        region["_name"] = name
        result.append(region)
    return result


# ===================================================================
# Region handlers registry
# Each handler: (cx, cy, region_data) -> height_offset
# ===================================================================

_HANDLERS = {}


def register_handler(type_name, func):
    """Register a region type handler."""
    _HANDLERS[type_name] = func


def _apply_region(cx, cy, region):
    """Apply a single region's height contribution."""
    rtype = region.get("type", "")
    handler = _HANDLERS.get(rtype)
    if handler:
        return handler(cx, cy, region)
    return 0.0


# --- Built-in handlers ---

def _h_plateau(cx, cy, r):
    pcx, pcy = r.get("center", [0.5, 0.5])
    pr = r.get("radius", 0.15)
    ph = r.get("height", 0)
    dx = (cx - pcx) / max(pr, 0.01)
    dy = (cy - pcy) / max(pr, 0.01)
    dist = math.sqrt(dx*dx + dy*dy)
    if dist < 1.0:
        fade = max(0, 1.0 - dist * dist)  # smooth quadratic falloff
        return ph * fade
    return 0.0

register_handler("plateau", _h_plateau)


def _h_bumps(cx, cy, r):
    b = r.get("bounds", [0, 0, 1, 1])
    if not (b[0] <= cx <= b[2] and b[1] <= cy <= b[3]):
        return 0.0
    amp = r.get("amplitude", r.get("bumps", 200))
    base = r.get("height", 0)
    freq = r.get("freq", 8)
    return base + math.sin(cx * freq) * math.cos(cy * (freq * 0.75)) * amp

register_handler("bumps", _h_bumps)
register_handler("farm", _h_bumps)


def _h_river(cx, cy, r):
    rcx = r.get("centerX", 0.78)
    rw = r.get("width", 0.07)
    depth = r.get("depth", -1500)
    wf = r.get("waveFreq", 4)
    wa = r.get("waveAmp", 0.03)
    actual_center = rcx + math.sin(cy * wf) * wa
    rdist = abs(cx - actual_center)
    factor = max(0, 1.0 - rdist / max(rw, 0.01))
    return factor * factor * depth  # quadratic for smoother banks

register_handler("river", _h_river)


def _h_hills(cx, cy, r):
    b = r.get("bounds", [0, 0, 1, 1])
    if not (b[0] <= cx <= b[2] and b[1] <= cy <= b[3]):
        return 0.0
    amp = r.get("amplitude", r.get("hillHeight", 2000))
    freq = r.get("freq", r.get("hillFreq", 2.5))
    bx = (cx - (b[0]+b[2])/2) * 10
    by = (cy - (b[1]+b[3])/2) * 10
    bump = math.sin(bx * freq) * math.cos(by * freq * 0.8) * amp
    bump += math.sin(bx * freq * 0.5 + by * freq * 0.3) * amp * 0.6
    fx = min((cx - b[0]) * 8, (b[2] - cx) * 8, 1)
    fy = min((cy - b[1]) * 5, (b[3] - cy) * 5, 1)
    fade = max(0, fx) * max(0, fy)
    return bump * fade

register_handler("hills", _h_hills)
register_handler("bamboo", _h_hills)


def _h_peaks(cx, cy, r):
    start_y = r.get("startY", 0.65)
    if cy <= start_y:
        return 0.0
    mt_fade = min(1, (cy - start_y) * 4)
    h = 0.0
    for pk in r.get("peaks", []):
        px = pk.get("x", 0.5)
        pw = pk.get("width", 4)
        ph = pk.get("height", 5000)
        h += max(0, 1.0 - abs(cx - px) * pw) * ph * mt_fade
    ridge = r.get("ridgeHeight", 2000)
    h += ridge * mt_fade
    return h

register_handler("peaks", _h_peaks)
register_handler("mountain", _h_peaks)


def _h_depression(cx, cy, r):
    b = r.get("bounds", [0, 0, 1, 1])
    depth = r.get("depth", -1000)
    if not (b[0] <= cx <= b[2] and b[1] <= cy <= b[3]):
        return 0.0
    fx = min((cx - b[0]) * 10, (b[2] - cx) * 10, 1)
    fy = min((cy - b[1]) * 5, (b[3] - cy) * 5, 1)
    fade = max(0, fx) * max(0, fy)
    return depth * fade

register_handler("depression", _h_depression)
register_handler("valley", _h_depression)


def _h_lake(cx, cy, r):
    lcx, lcy = r.get("center", [0.5, 0.5])
    lr = r.get("radius", 0.1)
    depth = r.get("depth", -1000)
    dx = (cx - lcx) / max(lr, 0.01)
    dy = (cy - lcy) / max(lr, 0.01)
    dist = math.sqrt(dx*dx + dy*dy)
    if dist < 1.0:
        fade = max(0, 1.0 - dist * dist)
        return depth * fade
    return 0.0

register_handler("lake", _h_lake)


def _h_slope(cx, cy, r):
    b = r.get("bounds", [0, 0, 1, 0.1])
    depth = r.get("depth", -500)
    height = r.get("height", 0)
    if not (b[0] <= cx <= b[2] and b[1] <= cy <= b[3]):
        return 0.0
    fy = min((cy - b[1]) * 10, (b[3] - cy) * 10, 1)
    fade = max(0, fy)
    return (depth + height) * fade

register_handler("slope", _h_slope)
register_handler("ocean", _h_slope)


def _h_noise(cx, cy, r):
    fx = r.get("freqX", 5)
    fy = r.get("freqY", 3)
    amp = r.get("amplitude", 100)
    func = r.get("func", "sin")
    if func == "cos":
        return math.cos(cx * fx - cy * fy) * amp
    return math.sin(cx * fx + cy * fy) * amp

register_handler("noise", _h_noise)


def _h_fractal(cx, cy, r):
    """多层分形噪声，打破规则性，模拟自然地形起伏"""
    octaves = r.get("octaves", 5)
    base_freq = r.get("frequency", 4.0)
    amp = r.get("amplitude", 150)
    persistence = r.get("persistence", 0.45)
    lacunarity = r.get("lacunarity", 2.17)
    ox = r.get("offsetX", 0.0)
    oy = r.get("offsetY", 0.0)

    h = 0.0
    freq = base_freq
    a = amp
    for i in range(octaves):
        px = (cx + ox) * freq * math.pi * 2
        py = (cy + oy) * freq * math.pi * 2
        v = (math.sin(px + math.cos(py * 0.7)) * 0.5
             + math.cos(py + math.sin(px * 1.3)) * 0.3
             + math.sin(px * 0.6 - py * 0.8) * 0.2)
        h += v * a
        freq *= lacunarity
        a *= persistence
    return h

register_handler("fractal", _h_fractal)


def _h_cliff(cx, cy, r):
    """Steep cliff edge."""
    b = r.get("bounds", [0, 0, 1, 1])
    height = r.get("height", 3000)
    direction = r.get("direction", "north")  # north/south/east/west
    if not (b[0] <= cx <= b[2] and b[1] <= cy <= b[3]):
        return 0.0
    if direction == "north":
        t = (cy - b[1]) / max(b[3] - b[1], 0.01)
    elif direction == "south":
        t = 1.0 - (cy - b[1]) / max(b[3] - b[1], 0.01)
    elif direction == "east":
        t = (cx - b[0]) / max(b[2] - b[0], 0.01)
    else:
        t = 1.0 - (cx - b[0]) / max(b[2] - b[0], 0.01)
    t = max(0, min(1, t))
    return height * t * t  # quadratic for cliff feel

register_handler("cliff", _h_cliff)


def _h_terrace(cx, cy, r):
    """Stepped terraces."""
    b = r.get("bounds", [0, 0, 1, 1])
    height = r.get("height", 1000)
    steps = r.get("steps", 4)
    if not (b[0] <= cx <= b[2] and b[1] <= cy <= b[3]):
        return 0.0
    t = (cy - b[1]) / max(b[3] - b[1], 0.01)
    step = math.floor(t * steps) / steps
    return height * step

register_handler("terrace", _h_terrace)


def _h_crater(cx, cy, r):
    """Circular crater with raised rim."""
    ccx, ccy = r.get("center", [0.5, 0.5])
    cr = r.get("radius", 0.1)
    depth = r.get("depth", -800)
    rim = r.get("rimHeight", 500)
    dx = (cx - ccx) / max(cr, 0.01)
    dy = (cy - ccy) / max(cr, 0.01)
    dist = math.sqrt(dx*dx + dy*dy)
    if dist > 1.5:
        return 0.0
    if dist < 0.7:
        return depth * (1.0 - dist / 0.7)
    elif dist < 1.0:
        t = (dist - 0.7) / 0.3
        return rim * math.sin(t * math.pi)
    else:
        t = (dist - 1.0) / 0.5
        return rim * max(0, 1.0 - t) * 0.3
    return 0.0

register_handler("crater", _h_crater)


def convert_image_to_r16(image_path, target_size=505, output_path=None, height_scale=0.15, invert=False, blur_radius=3):
    """将灰度图片转换为 .r16 高度图"""
    pixels = None
    src_w = src_h = 0

    # 方法1: 用 Pillow（系统 Python 或已安装）
    try:
        from PIL import Image, ImageFilter
        img = Image.open(image_path).convert('L')
        img = img.resize((target_size, target_size), Image.LANCZOS)
        if blur_radius > 0:
            img = img.filter(ImageFilter.GaussianBlur(radius=blur_radius))
        pixels = list(img.getdata())
        _log("Read image with PIL: %dx%d" % (target_size, target_size))
    except ImportError:
        pass

    # 方法2: 用 subprocess 调系统 Python（UE Python 没有 Pillow 时）
    if pixels is None:
        import subprocess, sys, tempfile, json as _json
        _log("PIL not in UE Python, trying system Python...")
        # 写一个临时脚本让系统 Python 执行
        script = '''# -*- coding: utf-8 -*-
import json, struct
from PIL import Image, ImageFilter
img = Image.open(r"%s").convert("L")
img = img.resize((%d, %d), Image.LANCZOS)
blur = %d
if blur > 0:
    img = img.filter(ImageFilter.GaussianBlur(radius=blur))
pixels = list(img.getdata())
with open(r"%s", "wb") as f:
    f.write(bytes(bytearray(pixels)))
print("OK")
'''
        tmp_raw = os.path.join(tempfile.gettempdir(), "heightmap_raw.bin")
        script_filled = script % (image_path, target_size, target_size, blur_radius, tmp_raw)
        tmp_script = os.path.join(tempfile.gettempdir(), "hm_convert.py")
        with open(tmp_script, "w") as f:
            f.write(script_filled)

        try:
            result = subprocess.run(["python3", tmp_script], capture_output=True, text=True, timeout=30)
            if result.returncode == 0 and os.path.isfile(tmp_raw):
                with open(tmp_raw, "rb") as f:
                    raw = f.read()
                pixels = list(raw)
                os.remove(tmp_raw)
                os.remove(tmp_script)
                _log("Read image via system Python: %d pixels" % len(pixels))
            else:
                _log("System Python failed: " + result.stderr[:200])
                return None
        except Exception as e:
            _log("System Python error: " + str(e))
            return None

    if pixels is None or len(pixels) != target_size * target_size:
        _log("ERROR: pixel count mismatch")
        return None

    if invert:
        pixels = [255 - p for p in pixels]

    # 以中位数为 Z=0 基准
    sorted_px = sorted(pixels)
    median_val = sorted_px[len(sorted_px) // 2]
    _log("Median gray: %d, scale: %.2f" % (median_val, height_scale))

    data = bytearray()
    for p in pixels:
        normalized = (p - median_val) / 255.0
        val = int(MID + normalized * 32767 * height_scale * 4)
        val = max(0, min(65535, val))
        data += struct.pack('<H', val)

    if not output_path:
        out_dir = os.path.dirname(image_path)
        base_name = os.path.splitext(os.path.basename(image_path))[0]
        ts = time.strftime('%Y%m%d_%H%M%S')
        output_path = os.path.join(out_dir, base_name + "_" + ts + ".r16")

    with open(output_path, 'wb') as f:
        f.write(data)

    _log("Image -> r16: %s (%dx%d)" % (output_path, target_size, target_size))
    return output_path
