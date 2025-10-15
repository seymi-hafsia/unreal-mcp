using UnrealBuildTool;

public class UnrealMCPEditor : ModuleRules
{
    public UnrealMCPEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        Type = ModuleRules.ModuleType.Cpp;

        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "Projects",
            "Json",
            "JsonUtilities",
            "AssetRegistry",
            "LevelSequence",
            "MovieScene",
            "Slate",
            "SlateCore",
            "UMG",
            "UnrealMCP"
        });

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "UnrealEd",
            "UMGEditor",
            "Sequencer",
            "LevelSequenceEditor",
            "EditorScriptingUtilities",
            "Blutility",
            "LevelEditor",
            "MessageLog",
            "PropertyEditor",
            "ContentBrowser",
            "BlueprintGraph",
            "KismetCompiler",
            "Json",
            "JsonUtilities",
            "Sockets",
            "Networking",
            "AssetTools",
            "Niagara",
            "NiagaraCore",
            "MovieSceneTracks",
            "CinematicCamera"
        });

        PublicIncludePaths.AddRange(new[]
        {
            "UnrealMCP/Public"
        });

        PrivateIncludePaths.AddRange(new[]
        {
            "UnrealMCP/Private"
        });

    }
}
