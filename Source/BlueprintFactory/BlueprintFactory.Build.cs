using UnrealBuildTool;

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
	}
}
