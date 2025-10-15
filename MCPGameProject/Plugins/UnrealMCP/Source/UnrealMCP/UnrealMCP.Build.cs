using UnrealBuildTool;

public class UnrealMCP : ModuleRules
{
    public UnrealMCP(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "Slate",
            "SlateCore",
            "UMG",
            "AssetRegistry",
            "Projects"
        });

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "InputCore",
            "DeveloperSettings"
        });

        PublicDefinitions.Add("UNREALMCP_RUNTIME=1");
    }
}
