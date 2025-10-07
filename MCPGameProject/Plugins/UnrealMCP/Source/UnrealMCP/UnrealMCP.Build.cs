// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class UnrealMCP : ModuleRules
{
    public UnrealMCP(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
        // Use IWYUSupport instead of the deprecated bEnforceIWYU in UE5.5
        IWYUSupport = IWYUSupport.Full;

        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "UMG",
            "Json",
            "JsonUtilities",
            "AssetRegistry",
            "Sockets",
            "Networking",
            "RenderCore",
            "RHI"
        });

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "UnrealEd",
            "LevelEditor",
            "PropertyEditor",
            "AssetTools",
            "EditorScriptingUtilities",
            "Blutility",
            "UMGEditor",
            "Slate",
            "SlateCore",
            "Sequencer",
            "MovieScene",
            "SequencerScripting",
            "LevelSequenceEditor",
            "Niagara",
            "NiagaraCore"
        });
    }
}
