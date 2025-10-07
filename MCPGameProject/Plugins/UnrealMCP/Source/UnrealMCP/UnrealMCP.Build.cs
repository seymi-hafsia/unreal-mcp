// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class UnrealMCP : ModuleRules
{
    public UnrealMCP(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
        // Use IWYUSupport instead of the deprecated bEnforceIWYU in UE5.5
        IWYUSupport = IWYUSupport.Full;

        PublicDependencyModuleNames.AddRange(
            new string[]
            {
                "Core",
                "CoreUObject",
                "Engine",
                "Json",
                "JsonUtilities",
                "AssetRegistry",          // AssetData/ARFilter
                "Sockets",                // FIPv4Address, ISocketSubsystem, etc.
                "Networking",
                "RenderCore",
                "RHI"
            }
        );

        PrivateDependencyModuleNames.AddRange(
            new string[]
            {
                "AssetTools",
                "AudioMixer",
                "BlueprintGraph",
                "EditorFramework",
                "EditorScriptingUtilities",
                "EditorSubsystem",
                "Kismet",
                "KismetCompiler",
                "LevelEditor",
                "LevelSequenceEditor",
                "LevelSequence",
                "MovieScene",
                "MovieSceneTools",
                "MovieSceneTracks",
                "Niagara",
                "NiagaraCore",
                "PropertyEditor",
                "SequencerScripting",
                "Sequencer",
                "Slate",
                "SlateCore",
                "SourceControl",
                "ToolMenus",
                "UMG",
                "UMGEditor",
                "UnrealEd"
            }
        );
    }
}
