// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class UnrealMCP : ModuleRules
{
    public UnrealMCP(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
        // Use IWYUSupport instead of the deprecated bEnforceIWYU in UE5.5
        IWYUSupport = IWYUSupport.Full;

        PublicIncludePaths.AddRange(
            new string[]
            {
                // ... add public include paths required here ...
            }
        );

        PrivateIncludePaths.AddRange(
            new string[]
            {
                // ... add other private include paths required here ...
            }
        );

        PublicDependencyModuleNames.AddRange(
            new string[]
            {
                "Core",
                "CoreUObject",
                "DeveloperSettings",
                "Engine",
                "HTTP",
                "InputCore",
                "Json",
                "JsonUtilities",
                "AssetRegistry",
                "Sockets",
                "Networking",
                "RenderCore",
                "RHI",
                "LevelSequence",
                "UMG"
            }
        );

        PrivateDependencyModuleNames.AddRange(
            new string[]
            {
                "UnrealEd",
                "LevelEditor",
                "PropertyEditor",
                "AssetTools",
                "EditorScriptingUtilities",
                "Blutility",
                "Slate",
                "SlateCore",
                "Sequencer",
                "MovieScene",
                "SequencerScripting",
                "LevelSequenceEditor",
                "UMGEditor",
                "Niagara",
                "NiagaraCore",
                "AudioMixer",
                "BlueprintGraph",
                "CinematicCamera",
                "EditorSubsystem",
                "Kismet",
                "KismetCompiler",
                "MovieSceneTools",
                "MovieSceneTracks",
                "Projects",
                "Settings",
                "SignalProcessing",
                "SkeletalMeshUtilitiesCommon",
                "SourceControl"
            }
        );

        if (Target.bBuildEditor == true)
        {
            PrivateDependencyModuleNames.AddRange(
                new string[]
                {
                    "BlueprintEditorLibrary", // For Blueprint utilities
                    "ToolMenus"              // For editor UI
                }
            );
        }

        DynamicallyLoadedModuleNames.AddRange(
            new string[]
            {
                // ... add any modules that your module loads dynamically here ...
            }
        );
    }
}
