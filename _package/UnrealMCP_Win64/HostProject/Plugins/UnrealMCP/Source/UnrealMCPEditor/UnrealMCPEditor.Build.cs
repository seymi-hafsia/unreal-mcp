using UnrealBuildTool;

public class UnrealMCPEditor : ModuleRules
{
    public UnrealMCPEditor(ReadOnlyTargetRules Target) : base(Target)
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
            "MovieScene",
            "LevelSequence",
            "Projects",
            "UnrealMCP"
        });

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "UnrealEd",
            "UMGEditor",
            "EditorScriptingUtilities",
            "Blutility",
            "LevelEditor",
            "Sequencer",
            "LevelSequenceEditor",
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
