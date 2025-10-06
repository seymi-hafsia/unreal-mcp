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
                "Networking",
                "RenderCore",
                "RHI",
                "Sockets"
            }
        );

        PrivateDependencyModuleNames.AddRange(
            new string[]
            {
                "AssetRegistry",
                "AssetTools",
                "AudioMixer",
                "BlueprintGraph",
                "Blutility",
                "CinematicCamera",
                "EditorScriptingUtilities",
                "EditorSubsystem",
                "Kismet",
                "KismetCompiler",
                "LevelEditor",
                "LevelSequence",
                "LevelSequenceEditor",
                "MovieScene",
                "MovieSceneTools",
                "MovieSceneTracks",
                "Niagara",
                "NiagaraCore",
                "Sequencer",
                "Projects",
                "SequencerScripting",
                "Settings",
                "SignalProcessing",
                "SkeletalMeshUtilitiesCommon",
                "Slate",
                "SlateCore",
                "SourceControl",
                "UMG",
                "UnrealEd"
            }
        );

        if (Target.bBuildEditor == true)
        {
            PrivateDependencyModuleNames.AddRange(
                new string[]
                {
                    "BlueprintEditorLibrary", // For Blueprint utilities
                    "PropertyEditor",         // For widget property editing
                    "ToolMenus",             // For editor UI
                    "UMGEditor"               // For WidgetBlueprint.h and other UMG editor functionality
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
