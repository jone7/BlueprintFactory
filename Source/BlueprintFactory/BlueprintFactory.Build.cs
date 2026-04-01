using UnrealBuildTool;
using System.IO;

public class BlueprintFactory : ModuleRules
{
	public BlueprintFactory(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Slate",
			"SlateCore",
			"InputCore",
			"UnrealEd",
			"ToolMenus",
			"EditorStyle",
			"PythonScriptPlugin",
			"DesktopPlatform",
			"Json",
			"JsonUtilities",
			"Landscape",
			"LandscapeEditor",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Kismet",
			"BlueprintGraph",
		});

		bool bHasUnLua = false;
		string[] PluginSearchPaths = new string[]
		{
			Path.Combine(ModuleDirectory, "..", "..", "..", "..", "Plugins"),
			Path.Combine(ModuleDirectory, "..", "..", "..", "..", "_deps"),
		};
		foreach (string SearchPath in PluginSearchPaths)
		{
			string UnLuaPath = Path.Combine(SearchPath, "UnLua");
			if (Directory.Exists(UnLuaPath))
			{
				bHasUnLua = true;
				break;
			}

			string NestedPath = Path.Combine(SearchPath, "Tencent-UnLua", "Plugins", "UnLua");
			if (Directory.Exists(NestedPath))
			{
				bHasUnLua = true;
				break;
			}
		}

		if (bHasUnLua)
		{
			PrivateDependencyModuleNames.Add("UnLua");
			PublicDefinitions.Add("WITH_UNLUA=1");
		}
		else
		{
			PublicDefinitions.Add("WITH_UNLUA=0");
		}
	}
}
