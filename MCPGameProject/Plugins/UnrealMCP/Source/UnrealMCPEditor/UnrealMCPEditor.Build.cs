// Source/UnrealMCPEditor/UnrealMCPEditor.Build.cs
using UnrealBuildTool;
using System.IO;

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
            "Projects",
            "Json",
            "JsonUtilities",
            "AssetRegistry",
            "LevelSequence",
            "MovieScene",
            "MovieSceneTracks",
            "Slate",
            "SlateCore",
            "UMG",
            "UnrealMCP", // dépend du module runtime du plugin
            "UnrealEd",  // APIs Editor - Needed in public for Rocket mode
            "EditorScriptingUtilities", // EditorViewportLibrary
            "LevelSequenceEditor" // LevelSequenceFactoryNew
        });

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "UMGEditor",           // WidgetBlueprint
            "Sequencer",
            "Blutility",
            "LevelEditor",
            "MessageLog",
            "PropertyEditor",
            "ContentBrowser",
            "BlueprintGraph",
            "KismetCompiler",
            "Sockets",
            "Networking",
            "AssetTools",
            "Niagara",
            "NiagaraCore",
            "CinematicCamera"
        });

        // Inclut les headers publics/privés du module runtime "UnrealMCP" via des chemins robustes.
        // (Évite les chemins relatifs "UnrealMCP/Public" qui cassent selon l’emplacement du module.)
        PublicIncludePaths.AddRange(new[]
        {
            Path.Combine(ModuleDirectory, "..", "UnrealMCP", "Public")
        });

        PrivateIncludePaths.AddRange(new[]
        {
            Path.Combine(ModuleDirectory, "..", "UnrealMCP", "Private"),
            Path.Combine(ModuleDirectory, "Private")
        });
    }
}
