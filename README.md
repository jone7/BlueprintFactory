# BlueprintFactory

JSON 模板驱动的 UE5 蓝图/材质/地形批量生成工具。

## 功能

- 蓝图生成：JSON → Actor Blueprint（组件树、属性、UnLua 绑定）
- 材质生成：JSON → Material Instance（纹理参数、标量参数）
- 地形导入：JSON + 高度图 → Landscape

## 架构

C++ Slate 面板（轻量 UI）+ Python 执行引擎（实际生成逻辑）。
改 Python 不需要重新编译 C++。

## 依赖

- UE 5.7 + PythonScriptPlugin
- EditorScriptingUtilities

## 安装

```bash
git submodule add https://github.com/jone7/BlueprintFactory.git _deps/BlueprintFactory
mklink /J Plugins\BlueprintFactory _deps\BlueprintFactory
```

## 使用

UE 编辑器菜单 → 工具 → Blueprint Factory

### 蓝图模板 JSON

```json
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
```

### 材质模板 JSON

```json
{
    "Name": "MI_Ground_Farm",
    "Parent": "/Game/Art/Materials/M_Landscape_Master",
    "OutputPath": "/Game/Art/Materials/Landscape/",
    "Textures": {
        "BaseColor": "/Game/Art/Textures/Landscape/T_Ground_Farm_D",
        "Normal": "/Game/Art/Textures/Landscape/T_Ground_Farm_N"
    },
    "Parameters": {
        "Roughness": 0.8,
        "Tiling": 4.0
    }
}
```

### 地形模板 JSON

```json
{
    "Heightmap": "heightmap_main.png",
    "Size": {"X": 200, "Y": 200},
    "Scale": {"X": 200, "Y": 200, "Z": 100},
    "Layers": [
        {"Name": "Farm", "Material": "MI_Ground_Farm", "Region": {"MinX": 50, "MaxX": 100, "MinY": 50, "MaxY": 100}}
    ]
}
```

## 中文说明

JSON 模板驱动的蓝图/材质/地形批量生成工具。

- 蓝图生成：从 JSON 模板自动创建 Actor Blueprint，含组件树和属性
- 材质生成：从 JSON 模板自动创建 Material Instance，含纹理和参数
- 地形导入：从 JSON + 高度图自动创建 Landscape
