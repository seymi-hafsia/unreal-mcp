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
            "MovieScene",
            "LevelSequence",
            "AssetRegistry"
        });

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "UnrealEd",
            "UMGEditor",
            "LevelEditor",
            "Sequencer",
            "LevelSequenceEditor",
            "EditorScriptingUtilities",
            "Blutility",
            "Projects",
            "MessageLog",
            "PropertyEditor",
            "BlueprintGraph",
            "KismetCompiler",
            "UnrealMCP"
        });

        PrivateDependencyModuleNames.AddRange(new[]
        {
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

        if (Target.bBuildEditor)
        {
            Definitions.Add("WITH_EDITOR=1");
        }
    }
}
