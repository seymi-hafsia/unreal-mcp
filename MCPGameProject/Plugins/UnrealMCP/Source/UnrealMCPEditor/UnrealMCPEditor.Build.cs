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
            "Slate",
            "SlateCore",
            "UMG",
            "UnrealMCP" // dépend du module runtime du plugin
        });

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "UnrealEd",            // APIs Editor (EditorViewportLibrary, AssetImportTask, etc.)
            "UMGEditor",           // WidgetBlueprint
            "Sequencer",
            "LevelSequenceEditor", // LevelSequenceFactoryNew
            "EditorScriptingUtilities",
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
            "MovieSceneTracks",
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
