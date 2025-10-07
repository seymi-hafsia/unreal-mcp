#include "Assets/AssetImport.h"
#include "CoreMinimal.h"

#include "AssetToolsModule.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetImportTask.h"
#include "EditorFramework/AssetImportData.h"
#include "Factories/Factory.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EditorAssetLibrary.h"
#include "Factories/FbxFactory.h"
#include "Factories/FbxImportUI.h"
#include "Factories/FbxSkeletalMeshImportData.h"
#include "Factories/FbxStaticMeshImportData.h"
#include "Factories/SoundFactory.h"
#include "Factories/TextureFactory.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Permissions/WriteGate.h"
#include "ScopedTransaction.h"
#include "Sound/SoundWave.h"
#include "SourceControlService.h"
#include "Engine/Texture.h"
#include "Engine/TextureDefines.h"
#include "Engine/Texture2D.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "Animation/Skeleton.h"
#include "UnrealEdGlobals.h"
#include "Editor/UnrealEdEngine.h"

namespace
{
    constexpr const TCHAR* ErrorCodeInvalidParameters = TEXT("INVALID_PARAMETERS");
    constexpr const TCHAR* ErrorCodePathNotAllowed = TEXT("PATH_NOT_ALLOWED");
    constexpr const TCHAR* ErrorCodeDestPathInvalid = TEXT("DEST_PATH_INVALID");
    constexpr const TCHAR* ErrorCodeFileNotFound = TEXT("FILE_NOT_FOUND");
    constexpr const TCHAR* ErrorCodeUnsupportedExtension = TEXT("UNSUPPORTED_EXTENSION");
    constexpr const TCHAR* ErrorCodeImportFailed = TEXT("IMPORT_FAILED");
    constexpr const TCHAR* ErrorCodeSourceControlRequired = TEXT("SOURCE_CONTROL_REQUIRED");

    enum class EImportKind
    {
        Unknown,
        Fbx,
        Texture,
        Audio
    };

    enum class EConflictPolicy
    {
        Skip,
        Overwrite,
        CreateUnique
    };

    struct FConflictOptions
    {
        EConflictPolicy Policy = EConflictPolicy::Overwrite;
    };

    struct FFbxOptions
    {
        bool bImportAsSkeletal = false;
        bool bImportAnimations = false;
        bool bImportMaterials = true;
        bool bImportTextures = true;
        bool bCombineMeshes = false;
        FString SkeletonPath;
        FString NormalImportMethod;
        FString LodGroup;
    };

    struct FTextureOptions
    {
        bool bCreateMaterial = false;
        bool bSRGB = true;
        FString CompressionSettings = TEXT("Default");
        FString MipGenSettings = TEXT("FromTextureGroup");
        bool bFlipGreenChannel = false;
    };

    struct FAudioOptions
    {
        FString SoundGroup = TEXT("SFX");
    };

    struct FImportPlanEntry
    {
        FString SourceFile;
        FString NormalizedSourceFile;
        FString DestPath;
        FString PackageBaseName;
        EImportKind Kind = EImportKind::Unknown;
        bool bShouldImport = true;
        bool bWillOverwrite = false;
        FString SkipReason;
        TArray<FString> PreExistingPackages;
        TArray<FString> ImportedObjectPaths;
        TArray<FString> Warnings;
        bool bImportFailed = false;
    };

    FString NormalizeContentPath(const FString& InPath)
    {
        FString Trimmed = InPath;
        Trimmed.TrimStartAndEndInline();
        if (Trimmed.IsEmpty())
        {
            return FString();
        }

        if (!Trimmed.StartsWith(TEXT("/")))
        {
            return FString::Printf(TEXT("/Game/%s"), *Trimmed);
        }

        return Trimmed;
    }

    TSharedPtr<FJsonObject> MakeErrorResponse(const FString& Code, const FString& Message)
    {
        TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
        Error->SetBoolField(TEXT("success"), false);
        Error->SetStringField(TEXT("errorCode"), Code);
        Error->SetStringField(TEXT("error"), Message);
        return Error;
    }

    TSharedPtr<FJsonObject> MakeSuccessResponse(const TSharedPtr<FJsonObject>& Payload)
    {
        TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
        Response->SetBoolField(TEXT("success"), true);
        if (Payload.IsValid())
        {
            Response->SetObjectField(TEXT("data"), Payload);
        }
        return Response;
    }

    bool EnsureDirectory(const FString& DestPath)
    {
        if (UEditorAssetLibrary::DoesDirectoryExist(DestPath))
        {
            return true;
        }

        return UEditorAssetLibrary::MakeDirectory(DestPath);
    }

    EImportKind DetectKindByExtension(const FString& FilePath)
    {
        const FString Extension = FPaths::GetExtension(FilePath, true).ToLower();
        if (Extension == TEXT(".fbx"))
        {
            return EImportKind::Fbx;
        }
        if (Extension == TEXT(".png") || Extension == TEXT(".tga") || Extension == TEXT(".jpg") ||
            Extension == TEXT(".jpeg") || Extension == TEXT(".exr") || Extension == TEXT(".hdr") ||
            Extension == TEXT(".bmp"))
        {
            return EImportKind::Texture;
        }
        if (Extension == TEXT(".wav") || Extension == TEXT(".ogg") || Extension == TEXT(".flac"))
        {
            return EImportKind::Audio;
        }
        return EImportKind::Unknown;
    }

    FConflictOptions ParseConflictOptions(const TSharedPtr<FJsonObject>& OptionsObject)
    {
        FConflictOptions Result;

        if (!OptionsObject.IsValid())
        {
            return Result;
        }

        const TSharedPtr<FJsonObject>* ConflictObject = nullptr;
        if (!OptionsObject->TryGetObjectField(TEXT("conflict"), ConflictObject) || !ConflictObject || !ConflictObject->IsValid())
        {
            return Result;
        }

        FString OnExisting;
        if ((*ConflictObject)->TryGetStringField(TEXT("onExisting"), OnExisting))
        {
            OnExisting = OnExisting.ToLower();
            if (OnExisting == TEXT("skip"))
            {
                Result.Policy = EConflictPolicy::Skip;
            }
            else if (OnExisting == TEXT("overwrite"))
            {
                Result.Policy = EConflictPolicy::Overwrite;
            }
            else if (OnExisting == TEXT("create_unique"))
            {
                Result.Policy = EConflictPolicy::CreateUnique;
            }
        }

        return Result;
    }

    void ApplyFbxPreset(const FString& PresetName, FFbxOptions& OutOptions)
    {
        const FString Lower = PresetName.ToLower();
        if (Lower == TEXT("fbx_character"))
        {
            OutOptions.bImportAsSkeletal = true;
            OutOptions.bImportAnimations = true;
            OutOptions.bImportMaterials = false;
            OutOptions.bImportTextures = false;
            OutOptions.bCombineMeshes = false;
        }
        else if (Lower == TEXT("fbx_static"))
        {
            OutOptions.bImportAsSkeletal = false;
            OutOptions.bImportAnimations = false;
            OutOptions.bCombineMeshes = false;
        }
    }

    void ApplyTexturePreset(const FString& PresetName, FTextureOptions& OutOptions)
    {
        const FString Lower = PresetName.ToLower();
        if (Lower == TEXT("textures_default"))
        {
            OutOptions.bSRGB = true;
            OutOptions.CompressionSettings = TEXT("Default");
            OutOptions.MipGenSettings = TEXT("FromTextureGroup");
        }
    }

    void ApplyAudioPreset(const FString& PresetName, FAudioOptions& OutOptions)
    {
        const FString Lower = PresetName.ToLower();
        if (Lower == TEXT("audio_default"))
        {
            OutOptions.SoundGroup = TEXT("SFX");
        }
    }

    void OverrideFbxOptions(const TSharedPtr<FJsonObject>& OptionsObject, FFbxOptions& OutOptions)
    {
        if (!OptionsObject.IsValid())
        {
            return;
        }

        const TSharedPtr<FJsonObject>* FbxObject = nullptr;
        if (!OptionsObject->TryGetObjectField(TEXT("fbx"), FbxObject) || !FbxObject || !FbxObject->IsValid())
        {
            return;
        }

        if ((*FbxObject)->HasTypedField<EJson::Boolean>(TEXT("importAsSkeletal")))
        {
            OutOptions.bImportAsSkeletal = (*FbxObject)->GetBoolField(TEXT("importAsSkeletal"));
        }
        if ((*FbxObject)->HasTypedField<EJson::Boolean>(TEXT("importAnimations")))
        {
            OutOptions.bImportAnimations = (*FbxObject)->GetBoolField(TEXT("importAnimations"));
        }
        if ((*FbxObject)->HasTypedField<EJson::Boolean>(TEXT("importMaterials")))
        {
            OutOptions.bImportMaterials = (*FbxObject)->GetBoolField(TEXT("importMaterials"));
        }
        if ((*FbxObject)->HasTypedField<EJson::Boolean>(TEXT("importTextures")))
        {
            OutOptions.bImportTextures = (*FbxObject)->GetBoolField(TEXT("importTextures"));
        }
        if ((*FbxObject)->HasTypedField<EJson::Boolean>(TEXT("combineMeshes")))
        {
            OutOptions.bCombineMeshes = (*FbxObject)->GetBoolField(TEXT("combineMeshes"));
        }
        FString SkeletonPath;
        if ((*FbxObject)->TryGetStringField(TEXT("skeleton"), SkeletonPath))
        {
            OutOptions.SkeletonPath = SkeletonPath;
        }
        FString NormalMethod;
        if ((*FbxObject)->TryGetStringField(TEXT("normalImportMethod"), NormalMethod))
        {
            OutOptions.NormalImportMethod = NormalMethod;
        }
        FString LodGroup;
        if ((*FbxObject)->TryGetStringField(TEXT("lodGroup"), LodGroup))
        {
            OutOptions.LodGroup = LodGroup;
        }
    }

    void OverrideTextureOptions(const TSharedPtr<FJsonObject>& OptionsObject, FTextureOptions& OutOptions)
    {
        if (!OptionsObject.IsValid())
        {
            return;
        }

        const TSharedPtr<FJsonObject>* TextureObject = nullptr;
        if (!OptionsObject->TryGetObjectField(TEXT("textures"), TextureObject) || !TextureObject || !TextureObject->IsValid())
        {
            return;
        }

        if ((*TextureObject)->HasTypedField<EJson::Boolean>(TEXT("createMaterial")))
        {
            OutOptions.bCreateMaterial = (*TextureObject)->GetBoolField(TEXT("createMaterial"));
        }
        if ((*TextureObject)->HasTypedField<EJson::Boolean>(TEXT("sRGB")))
        {
            OutOptions.bSRGB = (*TextureObject)->GetBoolField(TEXT("sRGB"));
        }
        FString Compression;
        if ((*TextureObject)->TryGetStringField(TEXT("compressionSettings"), Compression))
        {
            OutOptions.CompressionSettings = Compression;
        }
        FString MipGen;
        if ((*TextureObject)->TryGetStringField(TEXT("mipGenSettings"), MipGen))
        {
            OutOptions.MipGenSettings = MipGen;
        }
        if ((*TextureObject)->HasTypedField<EJson::Boolean>(TEXT("flipGreenChannel")))
        {
            OutOptions.bFlipGreenChannel = (*TextureObject)->GetBoolField(TEXT("flipGreenChannel"));
        }
    }

    void OverrideAudioOptions(const TSharedPtr<FJsonObject>& OptionsObject, FAudioOptions& OutOptions)
    {
        if (!OptionsObject.IsValid())
        {
            return;
        }

        const TSharedPtr<FJsonObject>* AudioObject = nullptr;
        if (!OptionsObject->TryGetObjectField(TEXT("audio"), AudioObject) || !AudioObject || !AudioObject->IsValid())
        {
            return;
        }

        FString SoundGroup;
        if ((*AudioObject)->TryGetStringField(TEXT("soundGroup"), SoundGroup))
        {
            OutOptions.SoundGroup = SoundGroup;
        }
    }

    TextureCompressionSettings ParseCompressionSettings(const FString& Value)
    {
        const FString Lower = Value.ToLower();
        if (Lower == TEXT("masks"))
        {
            return TC_Masks;
        }
        if (Lower == TEXT("hdr"))
        {
            return TC_HDR;
        }
        if (Lower == TEXT("normalmap"))
        {
            return TC_Normalmap;
        }
        return TC_Default;
    }

    TextureMipGenSettings ParseMipGenSettings(const FString& Value)
    {
        const FString Lower = Value.ToLower();
        if (Lower == TEXT("nomipmaps"))
        {
            return TMGS_NoMipmaps;
        }
        return TMGS_FromTextureGroup;
    }

    ESoundGroup ParseSoundGroup(const FString& Value)
    {
        const FString Lower = Value.ToLower();
        if (Lower == TEXT("voice"))
        {
            return ESoundGroup::Voice;
        }
        if (Lower == TEXT("music"))
        {
            return ESoundGroup::Music;
        }
        if (Lower == TEXT("ui"))
        {
            return ESoundGroup::UI;
        }
        if (Lower == TEXT("ambient"))
        {
            return ESoundGroup::Ambient;
        }
        return ESoundGroup::Default;
    }

    bool ConvertPackagesToFiles(const TArray<FString>& PackagePaths, TArray<FString>& OutFiles, FString& OutError)
    {
        if (PackagePaths.Num() == 0)
        {
            return true;
        }

        if (!FSourceControlService::AssetPathsToFiles(PackagePaths, OutFiles, OutError))
        {
            return false;
        }

        return true;
    }

    bool MarkFilesForAdd(const TArray<FString>& Files, FString& OutError)
    {
        if (Files.Num() == 0 || !FSourceControlService::IsEnabled())
        {
            return true;
        }

        TMap<FString, bool> PerFileResult;
        FString OperationError;
        if (!FSourceControlService::MarkForAdd(Files, PerFileResult, OperationError))
        {
            OutError = OperationError;
            return false;
        }

        for (const TPair<FString, bool>& Pair : PerFileResult)
        {
            if (!Pair.Value)
            {
                OutError = OperationError;
                return false;
            }
        }

        return true;
    }

    bool EnsureCheckoutForPackages(const TArray<FString>& PackagePaths, TSharedPtr<FJsonObject>& OutError)
    {
        for (const FString& Package : PackagePaths)
        {
            if (!FWriteGate::EnsureCheckoutForContentPath(Package, OutError))
            {
                return false;
            }
        }
        return true;
    }

    UObject* LoadObjectIfAvailable(const FString& ObjectPath)
    {
        if (ObjectPath.IsEmpty())
        {
            return nullptr;
        }
        return StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath);
    }

    void ApplyTexturePostImport(UTexture* Texture, const FTextureOptions& Options, bool& bOutChanged)
    {
        if (!Texture)
        {
            return;
        }

        bOutChanged = false;

        const TextureCompressionSettings DesiredCompression = ParseCompressionSettings(Options.CompressionSettings);
        if (Texture->CompressionSettings != DesiredCompression)
        {
            Texture->CompressionSettings = DesiredCompression;
            bOutChanged = true;
        }

        if (Texture->SRGB != Options.bSRGB)
        {
            Texture->SRGB = Options.bSRGB;
            bOutChanged = true;
        }

        const TextureMipGenSettings DesiredMipGen = ParseMipGenSettings(Options.MipGenSettings);
        if (Texture->MipGenSettings != DesiredMipGen)
        {
            Texture->MipGenSettings = DesiredMipGen;
            bOutChanged = true;
        }

        if (Texture->bFlipGreenChannel != Options.bFlipGreenChannel)
        {
            Texture->bFlipGreenChannel = Options.bFlipGreenChannel;
            bOutChanged = true;
        }

        if (bOutChanged)
        {
            Texture->MarkPackageDirty();
        }
    }

    void ApplyAudioPostImport(USoundWave* SoundWave, const FAudioOptions& Options, bool& bOutChanged)
    {
        if (!SoundWave)
        {
            return;
        }

        bOutChanged = false;

        const ESoundGroup DesiredGroup = ParseSoundGroup(Options.SoundGroup);
        if (SoundWave->SoundGroup != DesiredGroup)
        {
            SoundWave->SoundGroup = DesiredGroup;
            bOutChanged = true;
        }

        if (bOutChanged)
        {
            SoundWave->MarkPackageDirty();
        }
    }

    void AppendArrayField(TSharedPtr<FJsonObject> Parent, const FString& FieldName, const TArray<TSharedPtr<FJsonValue>>& Values)
    {
        if (!Parent)
        {
            return;
        }
        Parent->SetArrayField(FieldName, Values);
    }

    FString BuildSkipReason(EConflictPolicy Policy, const FString& ExistingPackage)
    {
        switch (Policy)
        {
        case EConflictPolicy::Skip:
            return TEXT("exists_and_skip");
        default:
            break;
        }
        return ExistingPackage.IsEmpty() ? TEXT("exists") : ExistingPackage;
    }

    void AddResultEntry(TArray<TSharedPtr<FJsonValue>>& Array, const FString& FilePath, const TArray<FString>& Assets, const FString& Reason = FString())
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("file"), FilePath);
        if (Assets.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> AssetValues;
            for (const FString& AssetPath : Assets)
            {
                AssetValues.Add(MakeShared<FJsonValueString>(AssetPath));
            }
            Entry->SetArrayField(TEXT("assets"), AssetValues);
        }
        if (!Reason.IsEmpty())
        {
            Entry->SetStringField(TEXT("reason"), Reason);
        }
        Array.Add(MakeShared<FJsonValueObject>(Entry));
    }

    void AddAuditAction(TArray<TSharedPtr<FJsonValue>>& Actions, const FString& FilePath, const FString& DestPath)
    {
        TSharedPtr<FJsonObject> Action = MakeShared<FJsonObject>();
        Action->SetStringField(TEXT("op"), TEXT("import"));
        Action->SetStringField(TEXT("file"), FilePath);
        Action->SetStringField(TEXT("dest"), DestPath);
        Actions.Add(MakeShared<FJsonValueObject>(Action));
    }
}

TSharedPtr<FJsonObject> FAssetImport::BatchImport(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return MakeErrorResponse(ErrorCodeInvalidParameters, TEXT("Missing parameters"));
    }

    FString RawDestPath;
    if (!Params->TryGetStringField(TEXT("destPath"), RawDestPath))
    {
        return MakeErrorResponse(ErrorCodeInvalidParameters, TEXT("Missing destPath"));
    }

    const FString DestPath = NormalizeContentPath(RawDestPath);
    if (!DestPath.StartsWith(TEXT("/Game/")))
    {
        return MakeErrorResponse(ErrorCodeDestPathInvalid, TEXT("Destination must be under /Game"));
    }

    FString PathReason;
    if (!FWriteGate::IsPathAllowed(DestPath, PathReason))
    {
        return MakeErrorResponse(ErrorCodePathNotAllowed, PathReason);
    }

    const TArray<TSharedPtr<FJsonValue>>* FilesArray = nullptr;
    if (!Params->TryGetArrayField(TEXT("files"), FilesArray) || !FilesArray || FilesArray->Num() == 0)
    {
        return MakeErrorResponse(ErrorCodeInvalidParameters, TEXT("Missing files array"));
    }

    const TSharedPtr<FJsonObject>* OptionsObjectPtr = nullptr;
    if (!Params->TryGetObjectField(TEXT("options"), OptionsObjectPtr))
    {
        OptionsObjectPtr = nullptr;
    }

    const TSharedPtr<FJsonObject> OptionsObject = OptionsObjectPtr ? *OptionsObjectPtr : nullptr;

    FString Preset;
    Params->TryGetStringField(TEXT("preset"), Preset);

    FFbxOptions FbxOptions;
    FTextureOptions TextureOptions;
    FAudioOptions AudioOptions;

    ApplyFbxPreset(Preset, FbxOptions);
    ApplyTexturePreset(Preset, TextureOptions);
    ApplyAudioPreset(Preset, AudioOptions);

    OverrideFbxOptions(OptionsObject, FbxOptions);
    OverrideTextureOptions(OptionsObject, TextureOptions);
    OverrideAudioOptions(OptionsObject, AudioOptions);

    const FConflictOptions ConflictOptions = ParseConflictOptions(OptionsObject);

    if (!EnsureDirectory(DestPath))
    {
        return MakeErrorResponse(ErrorCodeDestPathInvalid, TEXT("Failed to create destination folder"));
    }

    TArray<FImportPlanEntry> PlanEntries;
    PlanEntries.Reserve(FilesArray->Num());

    TArray<TSharedPtr<FJsonValue>> AuditActions;

    for (const TSharedPtr<FJsonValue>& Value : *FilesArray)
    {
        if (!Value.IsValid() || Value->Type != EJson::String)
        {
            continue;
        }

        FImportPlanEntry Entry;
        Entry.SourceFile = Value->AsString();
        Entry.SourceFile.TrimStartAndEndInline();
        Entry.NormalizedSourceFile = Entry.SourceFile;

        if (Entry.SourceFile.IsEmpty())
        {
            Entry.bShouldImport = false;
            Entry.SkipReason = TEXT("empty_path");
            PlanEntries.Add(MoveTemp(Entry));
            continue;
        }

        if (!FPaths::FileExists(Entry.SourceFile))
        {
            Entry.bShouldImport = false;
            Entry.SkipReason = TEXT("file_not_found");
            PlanEntries.Add(MoveTemp(Entry));
            continue;
        }

        Entry.Kind = DetectKindByExtension(Entry.SourceFile);
        if (Entry.Kind == EImportKind::Unknown)
        {
            Entry.bShouldImport = false;
            Entry.SkipReason = TEXT("unsupported_extension");
            PlanEntries.Add(MoveTemp(Entry));
            continue;
        }

        Entry.DestPath = DestPath;
        Entry.PackageBaseName = FPaths::GetBaseFilename(Entry.SourceFile);

        const FString PackagePath = FString::Printf(TEXT("%s/%s"), *DestPath, *Entry.PackageBaseName);
        FString ExistingPackageFilename;
        if (FPackageName::DoesPackageExist(PackagePath, &ExistingPackageFilename))
        {
            Entry.bWillOverwrite = true;
            Entry.PreExistingPackages.Add(PackagePath);
        }

        if (Entry.bWillOverwrite && ConflictOptions.Policy == EConflictPolicy::Skip)
        {
            Entry.bShouldImport = false;
            Entry.SkipReason = BuildSkipReason(ConflictOptions.Policy, PackagePath);
        }
        else if (Entry.bWillOverwrite && ConflictOptions.Policy == EConflictPolicy::CreateUnique)
        {
            Entry.bWillOverwrite = false;
            Entry.PreExistingPackages.Reset();
        }

        PlanEntries.Add(MoveTemp(Entry));
    }

    if (PlanEntries.Num() == 0)
    {
        return MakeErrorResponse(ErrorCodeInvalidParameters, TEXT("No importable files"));
    }

    // Early exit if all entries are skipped or invalid
    bool bHasImportable = false;
    for (const FImportPlanEntry& Entry : PlanEntries)
    {
        if (Entry.bShouldImport)
        {
            bHasImportable = true;
            break;
        }
    }

    if (!bHasImportable)
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetBoolField(TEXT("ok"), true);

        TArray<TSharedPtr<FJsonValue>> SkippedArray;
        for (const FImportPlanEntry& Entry : PlanEntries)
        {
            if (!Entry.bShouldImport)
            {
                AddResultEntry(SkippedArray, Entry.SourceFile, {}, Entry.SkipReason);
            }
        }

        AppendArrayField(Data, TEXT("skipped"), SkippedArray);
        Data->SetBoolField(TEXT("dryRun"), false);

        TSharedPtr<FJsonObject> Audit = MakeShared<FJsonObject>();
        Audit->SetBoolField(TEXT("dryRun"), false);
        Audit->SetArrayField(TEXT("actions"), AuditActions);
        Data->SetObjectField(TEXT("audit"), Audit);

        return MakeSuccessResponse(Data);
    }

    // Ensure checkout for overwrite cases when required by settings
    for (const FImportPlanEntry& Entry : PlanEntries)
    {
        if (!Entry.bShouldImport || !Entry.bWillOverwrite)
        {
            continue;
        }

        TSharedPtr<FJsonObject> CheckoutError;
        if (!EnsureCheckoutForPackages(Entry.PreExistingPackages, CheckoutError))
        {
            FString FailureMessage = TEXT("Source control checkout failed");
            if (CheckoutError.IsValid() && CheckoutError->HasField(TEXT("message")))
            {
                FailureMessage = CheckoutError->GetStringField(TEXT("message"));
            }
            return MakeErrorResponse(ErrorCodeSourceControlRequired, FailureMessage);
        }
    }

    FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));

    TArray<FAssetImportTask*> ImportTasks;
    ImportTasks.Reserve(PlanEntries.Num());

    TArray<TStrongObjectPtr<UAssetImportTask>> OwnedTasks;
    TArray<TStrongObjectPtr<UObject>> OwnedObjects;

    for (FImportPlanEntry& Entry : PlanEntries)
    {
        if (!Entry.bShouldImport)
        {
            continue;
        }

        TStrongObjectPtr<UAssetImportTask> Task = TStrongObjectPtr<UAssetImportTask>(NewObject<UAssetImportTask>());
        Task->Filename = Entry.SourceFile;
        Task->DestinationPath = Entry.DestPath;
        Task->bAutomated = true;
        Task->bSave = false;
        Task->bReplaceExisting = (ConflictOptions.Policy == EConflictPolicy::Overwrite);

        if (ConflictOptions.Policy == EConflictPolicy::CreateUnique)
        {
            FString TargetPackage = FString::Printf(TEXT("%s/%s"), *Entry.DestPath, *Entry.PackageBaseName);
            FString UniquePackage;
            FString UniqueName;
            AssetToolsModule.Get().CreateUniqueAssetName(TargetPackage, TEXT(""), UniquePackage, UniqueName);
            Task->DestinationName = UniqueName;
        }

        UObject* OverrideFactory = nullptr;
        UObject* OptionsObject = nullptr;

        switch (Entry.Kind)
        {
        case EImportKind::Fbx:
        {
            UFbxImportUI* ImportUI = NewObject<UFbxImportUI>();
            ImportUI->bImportAsSkeletalMesh = FbxOptions.bImportAsSkeletal;
            ImportUI->bImportMesh = true;
            ImportUI->bImportAnimations = FbxOptions.bImportAnimations;
            ImportUI->bImportMaterials = FbxOptions.bImportMaterials;
            ImportUI->bImportTextures = FbxOptions.bImportTextures;
            ImportUI->MeshTypeToImport = FbxOptions.bImportAsSkeletal ? FBXIT_SkeletalMesh : FBXIT_StaticMesh;
            ImportUI->bCreatePhysicsAsset = FbxOptions.bImportAsSkeletal;

            if (ImportUI->MeshTypeToImport == FBXIT_StaticMesh && ImportUI->StaticMeshImportData)
            {
                ImportUI->StaticMeshImportData->bCombineMeshes = FbxOptions.bCombineMeshes;

                const FString NormalMethodLower = FbxOptions.NormalImportMethod.ToLower();
                if (NormalMethodLower == TEXT("importnormalsandtangents"))
                {
                    ImportUI->StaticMeshImportData->NormalImportMethod = ENormalImportMethod::FBXNormalImportMethod_ImportNormalsAndTangents;
                }
                else if (NormalMethodLower == TEXT("computenormals"))
                {
                    ImportUI->StaticMeshImportData->NormalImportMethod = ENormalImportMethod::FBXNormalImportMethod_ComputeNormals;
                }

                if (!FbxOptions.LodGroup.IsEmpty())
                {
                    ImportUI->StaticMeshImportData->StaticMeshLODGroupName = FName(*FbxOptions.LodGroup);
                }
            }

            if (ImportUI->MeshTypeToImport == FBXIT_SkeletalMesh && ImportUI->SkeletalMeshImportData)
            {
                ImportUI->SkeletalMeshImportData->bImportAnimations = FbxOptions.bImportAnimations;
                ImportUI->SkeletalMeshImportData->bImportMaterials = FbxOptions.bImportMaterials;
                ImportUI->SkeletalMeshImportData->bImportTextures = FbxOptions.bImportTextures;
            }

            if (!FbxOptions.SkeletonPath.IsEmpty())
            {
                UObject* SkeletonObject = LoadObjectIfAvailable(FbxOptions.SkeletonPath);
                if (SkeletonObject)
                {
                    ImportUI->Skeleton = Cast<USkeleton>(SkeletonObject);
                }
                else
                {
                    Entry.Warnings.Add(FString::Printf(TEXT("Missing skeleton %s"), *FbxOptions.SkeletonPath));
                }
            }

            UFbxFactory* Factory = NewObject<UFbxFactory>();
            Factory->ImportUI = ImportUI;
            OverrideFactory = Factory;
            OptionsObject = ImportUI;
            break;
        }
        case EImportKind::Texture:
        {
            UTextureFactory* TextureFactory = NewObject<UTextureFactory>();
            TextureFactory->SuppressImportOverwriteDialog();
            TextureFactory->bCreateMaterial = TextureOptions.bCreateMaterial;
            OverrideFactory = TextureFactory;
            break;
        }
        case EImportKind::Audio:
        {
            USoundFactory* SoundFactory = NewObject<USoundFactory>();
            OverrideFactory = SoundFactory;
            break;
        }
        default:
            break;
        }

        if (OptionsObject)
        {
            Task->Options = OptionsObject;
        }

        if (OverrideFactory)
        {
            Task->Factory = Cast<UFactory>(OverrideFactory);
            OwnedObjects.Add(TStrongObjectPtr<UObject>(OverrideFactory));
        }

        if (OptionsObject && OptionsObject != OverrideFactory)
        {
            OwnedObjects.Add(TStrongObjectPtr<UObject>(OptionsObject));
        }

        ImportTasks.Add(Task.Get());
        Task->AddToRoot();
        OwnedTasks.Add(MoveTemp(Task));
    }

    if (ImportTasks.Num() == 0)
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetBoolField(TEXT("ok"), true);

        TArray<TSharedPtr<FJsonValue>> SkippedArray;
        for (const FImportPlanEntry& Entry : PlanEntries)
        {
            if (!Entry.bShouldImport)
            {
                AddResultEntry(SkippedArray, Entry.SourceFile, {}, Entry.SkipReason);
            }
        }

        AppendArrayField(Data, TEXT("skipped"), SkippedArray);
        Data->SetObjectField(TEXT("audit"), MakeShared<FJsonObject>());
        return MakeSuccessResponse(Data);
    }

    FScopedTransaction Transaction(FText::FromString(FWriteGate::GetTransactionName()));

    AssetToolsModule.Get().ImportAssetTasks(ImportTasks);

    for (FAssetImportTask* Task : ImportTasks)
    {
        if (!Task)
        {
            continue;
        }

        FImportPlanEntry* MatchingEntry = PlanEntries.FindByPredicate([Task](const FImportPlanEntry& Entry)
        {
            return Entry.SourceFile == Task->Filename;
        });

        if (!MatchingEntry)
        {
            continue;
        }

        MatchingEntry->ImportedObjectPaths = Task->ImportedObjectPaths;
        if (MatchingEntry->ImportedObjectPaths.Num() == 0)
        {
            MatchingEntry->bImportFailed = true;
        }

        if (MatchingEntry->Kind == EImportKind::Texture)
        {
            for (const FString& ObjectPath : MatchingEntry->ImportedObjectPaths)
            {
                UObject* AssetObject = LoadObject<UObject>(nullptr, *ObjectPath);
                if (UTexture* Texture = Cast<UTexture>(AssetObject))
                {
                    bool bChanged = false;
                    ApplyTexturePostImport(Texture, TextureOptions, bChanged);
                }
            }
        }
        else if (MatchingEntry->Kind == EImportKind::Audio)
        {
            for (const FString& ObjectPath : MatchingEntry->ImportedObjectPaths)
            {
                UObject* AssetObject = LoadObject<UObject>(nullptr, *ObjectPath);
                if (USoundWave* SoundWave = Cast<USoundWave>(AssetObject))
                {
                    bool bChanged = false;
                    ApplyAudioPostImport(SoundWave, AudioOptions, bChanged);
                }
            }
        }
    }

    for (TStrongObjectPtr<UAssetImportTask>& TaskPtr : OwnedTasks)
    {
        if (TaskPtr.IsValid())
        {
            TaskPtr->RemoveFromRoot();
        }
    }

    TArray<FString> NewPackagePaths;
    TSet<FString> ExistingPackages;
    for (const FImportPlanEntry& Entry : PlanEntries)
    {
        ExistingPackages.Append(Entry.PreExistingPackages);
    }

    for (FImportPlanEntry& Entry : PlanEntries)
    {
        if (!Entry.bShouldImport || Entry.bImportFailed)
        {
            continue;
        }

        for (const FString& ObjectPath : Entry.ImportedObjectPaths)
        {
            const FString PackagePath = FPackageName::ObjectPathToPackageName(ObjectPath);
            if (!ExistingPackages.Contains(PackagePath))
            {
                NewPackagePaths.AddUnique(PackagePath);
            }
        }
    }

    FString MarkForAddError;
    if (NewPackagePaths.Num() > 0)
    {
        TArray<FString> Files;
        if (!ConvertPackagesToFiles(NewPackagePaths, Files, MarkForAddError))
        {
            return MakeErrorResponse(ErrorCodeSourceControlRequired, MarkForAddError);
        }

        FString MarkError;
        if (!MarkFilesForAdd(Files, MarkError))
        {
            return MakeErrorResponse(ErrorCodeSourceControlRequired, MarkError);
        }
    }

    TArray<TSharedPtr<FJsonValue>> CreatedArray;
    TArray<TSharedPtr<FJsonValue>> OverwrittenArray;
    TArray<TSharedPtr<FJsonValue>> SkippedArray;
    TArray<TSharedPtr<FJsonValue>> FailedArray;

    TArray<TSharedPtr<FJsonValue>> WarningValues;

    bool bAllOk = true;

    for (const FImportPlanEntry& Entry : PlanEntries)
    {
        if (!Entry.bShouldImport)
        {
            AddResultEntry(SkippedArray, Entry.SourceFile, {}, Entry.SkipReason);
            continue;
        }

        if (Entry.bImportFailed)
        {
            bAllOk = false;
            AddResultEntry(FailedArray, Entry.SourceFile, {}, TEXT("import_failed"));
            continue;
        }

        if (Entry.ImportedObjectPaths.Num() == 0)
        {
            AddResultEntry(SkippedArray, Entry.SourceFile, {}, TEXT("no_assets"));
            continue;
        }

        if (Entry.bWillOverwrite && ConflictOptions.Policy == EConflictPolicy::Overwrite)
        {
            AddResultEntry(OverwrittenArray, Entry.SourceFile, Entry.ImportedObjectPaths);
        }
        else
        {
            AddResultEntry(CreatedArray, Entry.SourceFile, Entry.ImportedObjectPaths);
        }

        if (Entry.Warnings.Num() > 0)
        {
            for (const FString& Warning : Entry.Warnings)
            {
                WarningValues.Add(MakeShared<FJsonValueString>(Warning));
            }
        }

        AddAuditAction(AuditActions, Entry.SourceFile, Entry.DestPath);
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("ok"), bAllOk);

    if (CreatedArray.Num() > 0)
    {
        AppendArrayField(Data, TEXT("created"), CreatedArray);
    }
    if (OverwrittenArray.Num() > 0)
    {
        AppendArrayField(Data, TEXT("overwritten"), OverwrittenArray);
    }
    if (SkippedArray.Num() > 0)
    {
        AppendArrayField(Data, TEXT("skipped"), SkippedArray);
    }
    if (FailedArray.Num() > 0)
    {
        AppendArrayField(Data, TEXT("failed"), FailedArray);
    }
    if (WarningValues.Num() > 0)
    {
        Data->SetArrayField(TEXT("warnings"), WarningValues);
    }

    TSharedPtr<FJsonObject> Audit = MakeShared<FJsonObject>();
    Audit->SetBoolField(TEXT("dryRun"), false);
    Audit->SetArrayField(TEXT("actions"), AuditActions);
    Data->SetObjectField(TEXT("audit"), Audit);

    return MakeSuccessResponse(Data);
}
