#include "Permissions/WriteGate.h"
#include "UnrealMCPSettings.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "UObject/Package.h"

#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SourceControlService.h"

namespace
{
        FString SerializeJsonValue(const TSharedPtr<FJsonValue>& Value);
        FString SerializeArrayField(const TSharedPtr<FJsonObject>& Object, const FString& FieldName);
        FString SerializeObjectField(const TSharedPtr<FJsonObject>& Object, const FString& FieldName);

        FCriticalSection GRemoteStateMutex;
        bool GRemoteAllowWrite = false;
        bool GRemoteDryRun = true;
        TArray<FString> GRemoteAllowedPaths;
        TArray<FString> GRemoteAllowedTools;
        TArray<FString> GRemoteDeniedTools;
        static constexpr const TCHAR* TransactionLabel = TEXT("MCP Mutation");

        FString ValueToAuditString(const TSharedPtr<FJsonValue>& Value)
        {
                if (!Value.IsValid())
                {
                        return TEXT("<null>");
                }

                switch (Value->Type)
                {
                case EJson::String:
                        return Value->AsString();
                case EJson::Number:
                        return FString::SanitizeFloat(Value->AsNumber());
                case EJson::Boolean:
                        return Value->AsBool() ? TEXT("true") : TEXT("false");
                case EJson::Array:
                case EJson::Object:
                        return SerializeJsonValue(Value);
                case EJson::Null:
                default:
                        return TEXT("<null>");
                }
        }

        TArray<FString> ReadRemoteAllowedPaths()
        {
                FScopeLock Lock(&GRemoteStateMutex);
                return GRemoteAllowedPaths;
        }

        TArray<FString> ReadRemoteAllowedTools()
        {
                FScopeLock Lock(&GRemoteStateMutex);
                return GRemoteAllowedTools;
        }

        TArray<FString> ReadRemoteDeniedTools()
        {
                FScopeLock Lock(&GRemoteStateMutex);
                return GRemoteDeniedTools;
        }

        bool MatchesTool(const FString& Pattern, const FString& Command)
        {
                return Pattern.Equals(Command, ESearchCase::IgnoreCase);
        }

        FString SerializeJsonValue(const TSharedPtr<FJsonValue>& Value)
        {
                if (!Value.IsValid())
                {
                        return TEXT("<null>");
                }

                FString Serialized;
                TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
                if (FJsonSerializer::Serialize(Value.ToSharedRef(), Writer))
                {
                        return Serialized;
                }

                return TEXT("<error>");
        }

        FString SerializeArrayField(const TSharedPtr<FJsonObject>& Object, const FString& FieldName)
        {
                if (!Object.IsValid())
                {
                        return FString();
                }

                const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
                if (!Object->TryGetArrayField(FieldName, Array))
                {
                        return FString();
                }

                FString Serialized;
                TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
                Writer->WriteArrayStart();
                for (const TSharedPtr<FJsonValue>& Value : *Array)
                {
                        if (!Value.IsValid())
                        {
                                Writer->WriteNull();
                        }
                        else if (Value->Type == EJson::Number)
                        {
                                Writer->WriteValue(Value->AsNumber());
                        }
                        else if (Value->Type == EJson::Boolean)
                        {
                                Writer->WriteValue(Value->AsBool());
                        }
                        else if (Value->Type == EJson::String)
                        {
                                Writer->WriteValue(Value->AsString());
                        }
                        else
                        {
                                Writer->WriteRawJSONValue(SerializeJsonValue(Value));
                        }
                }
                Writer->WriteArrayEnd();
                Writer->Close();

                return Serialized;
        }

        FString SerializeObjectField(const TSharedPtr<FJsonObject>& Object, const FString& FieldName)
        {
                if (!Object.IsValid() || !Object->HasField(FieldName))
                {
                        return FString();
                }

                const TSharedPtr<FJsonObject>* ChildObject = nullptr;
                if (!Object->TryGetObjectField(FieldName, ChildObject))
                {
                        return FString();
                }

                FString Serialized;
                TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
                FJsonSerializer::Serialize(ChildObject->ToSharedRef(), Writer);
                return Serialized;
        }
}

const UUnrealMCPSettings* FWriteGate::GetSettings()
{
        return GetDefault<UUnrealMCPSettings>();
}

bool FWriteGate::IsMutationCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
        static const TSet<FString> MutatingCommands = {
                TEXT("spawn_actor"),
                TEXT("create_actor"),
                TEXT("delete_actor"),
                TEXT("set_actor_transform"),
                TEXT("actor.spawn"),
                TEXT("actor.destroy"),
                TEXT("actor.attach"),
                TEXT("actor.transform"),
                TEXT("actor.tag"),
                TEXT("set_actor_property"),
                TEXT("spawn_blueprint_actor"),
                TEXT("create_blueprint"),
                TEXT("add_component_to_blueprint"),
                TEXT("set_component_property"),
                TEXT("set_physics_properties"),
                TEXT("compile_blueprint"),
                TEXT("set_blueprint_property"),
                TEXT("set_static_mesh_properties"),
                TEXT("set_pawn_properties"),
                TEXT("connect_blueprint_nodes"),
                TEXT("add_blueprint_get_self_component_reference"),
                TEXT("add_blueprint_self_reference"),
                TEXT("add_blueprint_event_node"),
                TEXT("add_blueprint_input_action_node"),
                TEXT("add_blueprint_function_node"),
                TEXT("add_blueprint_get_component_node"),
                TEXT("add_blueprint_variable"),
                TEXT("create_input_mapping"),
                TEXT("create_umg_widget_blueprint"),
                TEXT("add_text_block_to_widget"),
                TEXT("add_button_to_widget"),
                TEXT("bind_widget_event"),
                TEXT("set_text_block_binding"),
                TEXT("add_widget_to_viewport"),
                TEXT("sc.status"),
                TEXT("sc.checkout"),
                TEXT("sc.add"),
                TEXT("sc.revert"),
                TEXT("sc.submit"),
                TEXT("asset.batch_import"),
                TEXT("sequence.create"),
                TEXT("sequence.bind_actors"),
                TEXT("sequence.unbind")
        };

        if (MutatingCommands.Contains(CommandType))
        {
                return true;
        }

        if (CommandType == TEXT("camera.bookmark") && Params.IsValid())
        {
                FString Op;
                if (Params->TryGetStringField(TEXT("op"), Op) && Op.Equals(TEXT("set"), ESearchCase::IgnoreCase))
                {
                        if (Params->HasTypedField<EJson::Boolean>(TEXT("persist")) && Params->GetBoolField(TEXT("persist")))
                        {
                                return true;
                        }
                }
        }

        return false;
}

bool FWriteGate::IsWriteAllowed()
{
        const UUnrealMCPSettings* Settings = GetSettings();
        if (!Settings)
        {
                return false;
        }

        bool bRemoteAllow = false;
        {
                FScopeLock Lock(&GRemoteStateMutex);
                bRemoteAllow = GRemoteAllowWrite;
        }

        return Settings->AllowWrite && bRemoteAllow;
}

bool FWriteGate::ShouldDryRun()
{
        const UUnrealMCPSettings* Settings = GetSettings();
        if (!Settings)
        {
                return true;
        }

        bool bRemoteDryRun = true;
        {
                FScopeLock Lock(&GRemoteStateMutex);
                bRemoteDryRun = GRemoteDryRun;
        }

        return Settings->DryRun || bRemoteDryRun;
}

TArray<FString> FWriteGate::GetEffectiveAllowedRoots()
{
        const UUnrealMCPSettings* Settings = GetSettings();
        TArray<FString> LocalRoots;
        if (Settings)
        {
                for (const FDirectoryPath& DirectoryPath : Settings->AllowedContentRoots)
                {
                        const FString Normalized = NormalizeContentPath(DirectoryPath.Path);
                        if (!Normalized.IsEmpty())
                        {
                                LocalRoots.AddUnique(Normalized);
                        }
                }
        }

        const TArray<FString> RemoteRoots = ReadRemoteAllowedPaths();

        if (LocalRoots.Num() == 0 || RemoteRoots.Num() == 0)
        {
                return {};
        }

        TArray<FString> Intersection;
        for (const FString& LocalRoot : LocalRoots)
        {
                for (const FString& RemoteRoot : RemoteRoots)
                {
                        if (LocalRoot.StartsWith(RemoteRoot))
                        {
                                Intersection.AddUnique(LocalRoot);
                        }
                        else if (RemoteRoot.StartsWith(LocalRoot))
                        {
                                Intersection.AddUnique(RemoteRoot);
                        }
                }
        }

        return Intersection;
}

bool FWriteGate::IsPathAllowed(const FString& ContentPath, FString& OutReason)
{
        if (ContentPath.IsEmpty())
        {
                return true;
        }

        const FString Normalized = NormalizeContentPath(ContentPath);
        if (Normalized.IsEmpty())
        {
                OutReason = TEXT("Invalid content path");
                return false;
        }

        const TArray<FString> AllowedRoots = GetEffectiveAllowedRoots();
        if (AllowedRoots.Num() == 0)
        {
                OutReason = TEXT("No allowed content roots configured");
                return false;
        }

        for (const FString& Root : AllowedRoots)
        {
                if (Normalized.StartsWith(Root))
                {
                        return true;
                }
        }

        OutReason = FString::Printf(TEXT("Path %s is not within allowed roots"), *Normalized);
        return false;
}

bool FWriteGate::CanMutate(const FString& CommandType, const FString& ContentPath, FString& OutReason)
{
        if (!IsToolAllowed(CommandType, OutReason))
        {
                return false;
        }

        if (!IsWriteAllowed() && CommandType != TEXT("sc.status"))
        {
                OutReason = TEXT("Write operations are disabled (allowWrite=false)");
                return false;
        }

        if (!IsPathAllowed(ContentPath, OutReason))
        {
                return false;
        }

        return true;
}

bool FWriteGate::IsToolAllowed(const FString& CommandType, FString& OutReason)
{
        const UUnrealMCPSettings* Settings = GetSettings();
        if (Settings)
        {
                for (const FString& DeniedTool : Settings->DeniedTools)
                {
                        FString Normalized = DeniedTool;
                        Normalized.TrimStartAndEndInline();
                        if (!Normalized.IsEmpty() && MatchesTool(Normalized, CommandType))
                        {
                                OutReason = TEXT("Tool denied by project settings");
                                return false;
                        }
                }

                if (Settings->AllowedTools.Num() > 0)
                {
                        bool bAllowed = false;
                        for (const FString& AllowedTool : Settings->AllowedTools)
                        {
                                FString Normalized = AllowedTool;
                                Normalized.TrimStartAndEndInline();
                                if (!Normalized.IsEmpty() && MatchesTool(Normalized, CommandType))
                                {
                                        bAllowed = true;
                                        break;
                                }
                        }

                        if (!bAllowed)
                        {
                                OutReason = TEXT("Tool not present in AllowedTools");
                                return false;
                        }
                }
        }

        const TArray<FString> RemoteDenied = ReadRemoteDeniedTools();
        for (const FString& DeniedTool : RemoteDenied)
        {
                FString Normalized = DeniedTool;
                Normalized.TrimStartAndEndInline();
                if (!Normalized.IsEmpty() && MatchesTool(Normalized, CommandType))
                {
                        OutReason = TEXT("Tool denied by remote enforcement");
                        return false;
                }
        }

        const TArray<FString> RemoteAllowed = ReadRemoteAllowedTools();
        if (RemoteAllowed.Num() > 0)
        {
                bool bAllowed = false;
                for (const FString& AllowedTool : RemoteAllowed)
                {
                        FString Normalized = AllowedTool;
                        Normalized.TrimStartAndEndInline();
                        if (!Normalized.IsEmpty() && MatchesTool(Normalized, CommandType))
                        {
                                bAllowed = true;
                                break;
                        }
                }

                if (!bAllowed)
                {
                        OutReason = TEXT("Tool not permitted by remote enforcement");
                        return false;
                }
        }

        OutReason.Reset();
        return true;
}

FMutationPlan FWriteGate::BuildPlan(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
        FMutationPlan Plan;
        Plan.bDryRun = ShouldDryRun();

        bool bAddedSpecificActions = false;

        if (CommandType == TEXT("camera.bookmark") && Params.IsValid())
        {
                FString Op;
                if (Params->TryGetStringField(TEXT("op"), Op) && Op.Equals(TEXT("set"), ESearchCase::IgnoreCase))
                {
                        if (Params->HasTypedField<EJson::Boolean>(TEXT("persist")) && Params->GetBoolField(TEXT("persist")))
                        {
                                FMutationAction Action;
                                Action.Op = TEXT("bookmark_persist");

                                if (Params->HasTypedField<EJson::Number>(TEXT("index")))
                                {
                                        Action.Args.Add(TEXT("index"), FString::FromInt(static_cast<int32>(Params->GetNumberField(TEXT("index")))));
                                }
                                else if (Params->HasTypedField<EJson::String>(TEXT("index")))
                                {
                                        Action.Args.Add(TEXT("index"), Params->GetStringField(TEXT("index")));
                                }
                                else
                                {
                                        Action.Args.Add(TEXT("index"), TEXT("0"));
                                }

                                Plan.Actions.Add(Action);
                                return Plan;
                        }
                }
        }
        else if (CommandType == TEXT("asset.create_folder") && Params.IsValid())
        {
                FMutationAction Action;
                Action.Op = TEXT("mkdir");

                FString Path;
                if (Params->TryGetStringField(TEXT("path"), Path))
                {
                        Action.Args.Add(TEXT("path"), NormalizeContentPath(Path));
                }

                Plan.Actions.Add(Action);
                return Plan;
        }
        else if (CommandType == TEXT("asset.rename") && Params.IsValid())
        {
                FMutationAction Action;
                Action.Op = TEXT("rename");

                FString FromObjectPath;
                if (Params->TryGetStringField(TEXT("fromObjectPath"), FromObjectPath))
                {
                        const FString FromPackage = FPackageName::ObjectPathToPackageName(NormalizeContentPath(FromObjectPath));
                        Action.Args.Add(TEXT("from"), FromPackage);
                }

                FString ToPackagePath;
                if (Params->TryGetStringField(TEXT("toPackagePath"), ToPackagePath))
                {
                        Action.Args.Add(TEXT("to"), NormalizeContentPath(ToPackagePath));
                }

                Plan.Actions.Add(Action);
                return Plan;
        }
        else if (CommandType == TEXT("asset.delete") && Params.IsValid())
        {
                const TArray<TSharedPtr<FJsonValue>>* ObjectPaths = nullptr;
                if (Params->TryGetArrayField(TEXT("objectPaths"), ObjectPaths))
                {
                        for (const TSharedPtr<FJsonValue>& Value : *ObjectPaths)
                        {
                                if (Value->Type == EJson::String)
                                {
                                        FMutationAction Action;
                                        Action.Op = TEXT("delete");
                                        Action.Args.Add(TEXT("objectPath"), NormalizeContentPath(Value->AsString()));
                                        Plan.Actions.Add(Action);
                                }
                        }

                        if (Plan.Actions.Num() > 0)
                        {
                                return Plan;
                        }
                }
        }
        else if (CommandType == TEXT("actor.spawn") && Params.IsValid())
        {
                FMutationAction Action;
                Action.Op = TEXT("spawn");

                FString ClassPath;
                if (Params->TryGetStringField(TEXT("classPath"), ClassPath))
                {
                        Action.Args.Add(TEXT("class"), ClassPath);
                }

                const FString LocationString = SerializeArrayField(Params, TEXT("location"));
                if (!LocationString.IsEmpty())
                {
                        Action.Args.Add(TEXT("location"), LocationString);
                }

                const FString RotationString = SerializeArrayField(Params, TEXT("rotation"));
                if (!RotationString.IsEmpty())
                {
                        Action.Args.Add(TEXT("rotation"), RotationString);
                }

                const FString ScaleString = SerializeArrayField(Params, TEXT("scale"));
                if (!ScaleString.IsEmpty())
                {
                        Action.Args.Add(TEXT("scale"), ScaleString);
                }

                const FString TagsString = SerializeArrayField(Params, TEXT("tags"));
                if (!TagsString.IsEmpty())
                {
                        Action.Args.Add(TEXT("tags"), TagsString);
                }

                if (Params->HasTypedField<EJson::Boolean>(TEXT("select")))
                {
                        Action.Args.Add(TEXT("select"), Params->GetBoolField(TEXT("select")) ? TEXT("true") : TEXT("false"));
                }

                if (Params->HasTypedField<EJson::Boolean>(TEXT("deferred")))
                {
                        Action.Args.Add(TEXT("deferred"), Params->GetBoolField(TEXT("deferred")) ? TEXT("true") : TEXT("false"));
                }

                Plan.Actions.Add(Action);
                return Plan;
        }
        else if (CommandType == TEXT("actor.destroy") && Params.IsValid())
        {
                const bool bHasAllowMissingFlag = Params->HasTypedField<EJson::Boolean>(TEXT("allowMissing"));
                const bool bAllowMissing = bHasAllowMissingFlag && Params->GetBoolField(TEXT("allowMissing"));

                const TArray<TSharedPtr<FJsonValue>>* ActorsArray = nullptr;
                if (Params->TryGetArrayField(TEXT("actors"), ActorsArray))
                {
                        for (const TSharedPtr<FJsonValue>& Value : *ActorsArray)
                        {
                                if (Value.IsValid() && Value->Type == EJson::String)
                                {
                                        FMutationAction Action;
                                        Action.Op = TEXT("destroy");
                                        Action.Args.Add(TEXT("actor"), Value->AsString());
                                        if (bHasAllowMissingFlag)
                                        {
                                                Action.Args.Add(TEXT("allowMissing"), bAllowMissing ? TEXT("true") : TEXT("false"));
                                        }
                                        Plan.Actions.Add(Action);
                                }
                        }

                        if (Plan.Actions.Num() > 0)
                        {
                                return Plan;
                        }
                }
        }
        else if (CommandType == TEXT("actor.attach") && Params.IsValid())
        {
                FMutationAction Action;
                Action.Op = TEXT("attach");

                FString Child;
                if (Params->TryGetStringField(TEXT("child"), Child))
                {
                        Action.Args.Add(TEXT("child"), Child);
                }

                FString Parent;
                if (Params->TryGetStringField(TEXT("parent"), Parent))
                {
                        Action.Args.Add(TEXT("parent"), Parent);
                }

                if (Params->HasField(TEXT("socketName")))
                {
                        FString Socket;
                        if (Params->TryGetStringField(TEXT("socketName"), Socket))
                        {
                                Action.Args.Add(TEXT("socket"), Socket);
                        }
                }

                if (Params->HasTypedField<EJson::Boolean>(TEXT("keepWorldTransform")))
                {
                        Action.Args.Add(TEXT("keepWorldTransform"), Params->GetBoolField(TEXT("keepWorldTransform")) ? TEXT("true") : TEXT("false"));
                }

                if (Params->HasTypedField<EJson::Boolean>(TEXT("weldSimulatedBodies")))
                {
                        Action.Args.Add(TEXT("weldSimulatedBodies"), Params->GetBoolField(TEXT("weldSimulatedBodies")) ? TEXT("true") : TEXT("false"));
                }

                Plan.Actions.Add(Action);
                return Plan;
        }
        else if (CommandType == TEXT("actor.transform") && Params.IsValid())
        {
                FMutationAction Action;
                Action.Op = TEXT("transform");

                FString ActorId;
                if (Params->TryGetStringField(TEXT("actor"), ActorId))
                {
                        Action.Args.Add(TEXT("actor"), ActorId);
                }

                const FString SetString = SerializeObjectField(Params, TEXT("set"));
                if (!SetString.IsEmpty())
                {
                        Action.Args.Add(TEXT("set"), SetString);
                }

                const FString AddString = SerializeObjectField(Params, TEXT("add"));
                if (!AddString.IsEmpty())
                {
                        Action.Args.Add(TEXT("add"), AddString);
                }

                Plan.Actions.Add(Action);
                return Plan;
        }
        else if (CommandType == TEXT("actor.tag") && Params.IsValid())
        {
                FMutationAction Action;
                Action.Op = TEXT("tag");

                FString ActorId;
                if (Params->TryGetStringField(TEXT("actor"), ActorId))
                {
                        Action.Args.Add(TEXT("actor"), ActorId);
                }

                if (Params->HasField(TEXT("replace")))
                {
                        const TSharedPtr<FJsonValue> ReplaceValue = Params->TryGetField(TEXT("replace"));
                        if (!ReplaceValue.IsValid() || ReplaceValue->Type == EJson::Null)
                        {
                                Action.Args.Add(TEXT("replace"), TEXT("null"));
                        }
                        else
                        {
                                Action.Args.Add(TEXT("replace"), SerializeJsonValue(ReplaceValue));
                        }
                }

                const FString AddTags = SerializeArrayField(Params, TEXT("add"));
                if (!AddTags.IsEmpty())
                {
                        Action.Args.Add(TEXT("add"), AddTags);
                }

                const FString RemoveTags = SerializeArrayField(Params, TEXT("remove"));
                if (!RemoveTags.IsEmpty())
                {
                        Action.Args.Add(TEXT("remove"), RemoveTags);
                }

                Plan.Actions.Add(Action);
                return Plan;
        }
        else if (CommandType == TEXT("asset.fix_redirectors") && Params.IsValid())
        {
                const TArray<TSharedPtr<FJsonValue>>* Paths = nullptr;
                if (Params->TryGetArrayField(TEXT("paths"), Paths))
                {
                        bool bAny = false;
                        for (const TSharedPtr<FJsonValue>& Value : *Paths)
                        {
                                if (Value->Type == EJson::String)
                                {
                                        FMutationAction Action;
                                        Action.Op = TEXT("fix_redirectors");
                                        Action.Args.Add(TEXT("path"), NormalizeContentPath(Value->AsString()));
                                        if (Params->HasTypedField<EJson::Boolean>(TEXT("recursive")))
                                        {
                                                Action.Args.Add(TEXT("recursive"), Params->GetBoolField(TEXT("recursive")) ? TEXT("true") : TEXT("false"));
                                        }
                                        Plan.Actions.Add(Action);
                                        bAny = true;
                                }
                        }

                        if (bAny)
                        {
                                return Plan;
                        }
                }
        }
        else if (CommandType == TEXT("asset.save_all") && Params.IsValid())
        {
                FMutationAction Action;
                Action.Op = TEXT("save_all");

                const TArray<TSharedPtr<FJsonValue>>* Paths = nullptr;
                if (Params->TryGetArrayField(TEXT("paths"), Paths))
                {
                        TArray<FString> NormalizedPaths;
                        for (const TSharedPtr<FJsonValue>& Value : *Paths)
                        {
                                if (Value->Type == EJson::String)
                                {
                                        NormalizedPaths.Add(NormalizeContentPath(Value->AsString()));
                                }
                        }

                        if (NormalizedPaths.Num() > 0)
                        {
                                Action.Args.Add(TEXT("paths"), FString::Join(NormalizedPaths, TEXT(",")));
                        }
                }

                if (Params->HasTypedField<EJson::Boolean>(TEXT("modifiedOnly")))
                {
                        Action.Args.Add(TEXT("modifiedOnly"), Params->GetBoolField(TEXT("modifiedOnly")) ? TEXT("true") : TEXT("false"));
                }

                Plan.Actions.Add(Action);
                return Plan;
        }
        else if (CommandType == TEXT("asset.batch_import") && Params.IsValid())
        {
                FString DestPath;
                if (Params->TryGetStringField(TEXT("destPath"), DestPath))
                {
                        const FString NormalizedDest = NormalizeContentPath(DestPath);
                        const TArray<TSharedPtr<FJsonValue>>* Files = nullptr;
                        if (Params->TryGetArrayField(TEXT("files"), Files))
                        {
                                for (const TSharedPtr<FJsonValue>& Value : *Files)
                                {
                                        if (Value->Type == EJson::String)
                                        {
                                                FMutationAction Action;
                                                Action.Op = TEXT("import");
                                                Action.Args.Add(TEXT("file"), Value->AsString());
                                                Action.Args.Add(TEXT("dest"), NormalizedDest);
                                                Plan.Actions.Add(Action);
                                        }
                                }

                                if (Plan.Actions.Num() > 0)
                                {
                                        return Plan;
                                }
                        }
                }
        }
        else if (CommandType == TEXT("sequence.create") && Params.IsValid())
        {
                FString SequencePath;
                const bool bHasPath = Params->TryGetStringField(TEXT("sequencePath"), SequencePath);
                const FString NormalizedPath = bHasPath ? NormalizeContentPath(SequencePath) : FString();

                const bool bOverwrite = Params->HasTypedField<EJson::Boolean>(TEXT("overwriteIfExists")) && Params->GetBoolField(TEXT("overwriteIfExists"));
                if (bOverwrite)
                {
                        FMutationAction DeleteAction;
                        DeleteAction.Op = TEXT("delete_sequence");
                        if (!NormalizedPath.IsEmpty())
                        {
                                DeleteAction.Args.Add(TEXT("path"), NormalizedPath);
                        }
                        Plan.Actions.Add(DeleteAction);
                }

                FMutationAction CreateAction;
                CreateAction.Op = TEXT("create_sequence");
                if (!NormalizedPath.IsEmpty())
                {
                        CreateAction.Args.Add(TEXT("path"), NormalizedPath);
                }
                Plan.Actions.Add(CreateAction);

                const bool bCreateCamera = Params->HasTypedField<EJson::Boolean>(TEXT("createCamera")) && Params->GetBoolField(TEXT("createCamera"));
                const bool bAddCameraCut = Params->HasTypedField<EJson::Boolean>(TEXT("addCameraCut")) && Params->GetBoolField(TEXT("addCameraCut"));

                if (bCreateCamera)
                {
                        FMutationAction CameraAction;
                        CameraAction.Op = TEXT("spawn_camera");
                        FString CameraName;
                        if (Params->TryGetStringField(TEXT("cameraName"), CameraName) && !CameraName.IsEmpty())
                        {
                                CameraAction.Args.Add(TEXT("name"), CameraName);
                        }
                        Plan.Actions.Add(CameraAction);

                        if (bAddCameraCut)
                        {
                                FMutationAction CutAction;
                                CutAction.Op = TEXT("add_camera_cut");
                                Plan.Actions.Add(CutAction);
                        }
                }

                const TArray<TSharedPtr<FJsonValue>>* ActorsArray = nullptr;
                if (Params->TryGetArrayField(TEXT("bindActors"), ActorsArray) && ActorsArray)
                {
                        for (const TSharedPtr<FJsonValue>& Value : *ActorsArray)
                        {
                                if (Value.IsValid() && Value->Type == EJson::String)
                                {
                                        FMutationAction BindAction;
                                        BindAction.Op = TEXT("bind_actor");
                                        BindAction.Args.Add(TEXT("actor"), Value->AsString());
                                        Plan.Actions.Add(BindAction);
                                }
                        }
                }

                return Plan;
        }
        else if (CommandType == TEXT("sequence.bind_actors") && Params.IsValid())
        {
                const TArray<TSharedPtr<FJsonValue>>* ActorsArray = nullptr;
                if (Params->TryGetArrayField(TEXT("actorPaths"), ActorsArray) && ActorsArray)
                {
                        for (const TSharedPtr<FJsonValue>& Value : *ActorsArray)
                        {
                                if (Value.IsValid() && Value->Type == EJson::String)
                                {
                                        FString ActorPath = Value->AsString();
                                        ActorPath.TrimStartAndEndInline();
                                        if (!ActorPath.IsEmpty())
                                        {
                                                FMutationAction Action;
                                                Action.Op = TEXT("bind");
                                                Action.Args.Add(TEXT("actor"), ActorPath);
                                                Plan.Actions.Add(Action);
                                        }
                                }
                        }
                }

                return Plan;
        }
        else if (CommandType == TEXT("sequence.unbind") && Params.IsValid())
        {
                const TArray<TSharedPtr<FJsonValue>>* BindingArray = nullptr;
                if (Params->TryGetArrayField(TEXT("bindingIds"), BindingArray) && BindingArray)
                {
                        for (const TSharedPtr<FJsonValue>& Value : *BindingArray)
                        {
                                if (Value.IsValid() && Value->Type == EJson::String)
                                {
                                        FString BindingId = Value->AsString();
                                        BindingId.TrimStartAndEndInline();
                                        if (!BindingId.IsEmpty())
                                        {
                                                FMutationAction Action;
                                                Action.Op = TEXT("unbind");
                                                Action.Args.Add(TEXT("bindingId"), BindingId);
                                                Plan.Actions.Add(Action);
                                        }
                                }
                        }
                }

                const TArray<TSharedPtr<FJsonValue>>* ActorsArray = nullptr;
                if (Params->TryGetArrayField(TEXT("actorPaths"), ActorsArray) && ActorsArray)
                {
                        for (const TSharedPtr<FJsonValue>& Value : *ActorsArray)
                        {
                                if (Value.IsValid() && Value->Type == EJson::String)
                                {
                                        FString ActorPath = Value->AsString();
                                        ActorPath.TrimStartAndEndInline();
                                        if (!ActorPath.IsEmpty())
                                        {
                                                FMutationAction Action;
                                                Action.Op = TEXT("unbind");
                                                Action.Args.Add(TEXT("actor"), ActorPath);
                                                Plan.Actions.Add(Action);
                                        }
                                }
                        }
                }

                return Plan;
        }
        else if (CommandType.StartsWith(TEXT("sc.")) && Params.IsValid())
        {
                const TArray<TSharedPtr<FJsonValue>>* AssetsArray = nullptr;
                if (Params->TryGetArrayField(TEXT("assets"), AssetsArray))
                {
                        for (const TSharedPtr<FJsonValue>& Value : *AssetsArray)
                        {
                                if (Value->Type == EJson::String)
                                {
                                        FMutationAction AssetAction;
                                        AssetAction.Op = CommandType;
                                        AssetAction.Args.Add(TEXT("asset"), NormalizeContentPath(Value->AsString()));
                                        Plan.Actions.Add(AssetAction);
                                        bAddedSpecificActions = true;
                                }
                        }
                }

                const TArray<TSharedPtr<FJsonValue>>* FilesArray = nullptr;
                if (Params->TryGetArrayField(TEXT("files"), FilesArray))
                {
                        for (const TSharedPtr<FJsonValue>& Value : *FilesArray)
                        {
                                if (Value->Type == EJson::String)
                                {
                                        FMutationAction FileAction;
                                        FileAction.Op = CommandType;
                                        FileAction.Args.Add(TEXT("file"), Value->AsString());
                                        Plan.Actions.Add(FileAction);
                                        bAddedSpecificActions = true;
                                }
                        }
                }
        }

        if (!bAddedSpecificActions)
        {
                FMutationAction Action;
                Action.Op = CommandType;

                if (Params.IsValid())
                {
                        for (const auto& Pair : Params->Values)
                        {
                                Action.Args.Add(Pair.Key, ValueToAuditString(Pair.Value));
                        }
                }

                Plan.Actions.Add(Action);
        }

        return Plan;
}

TSharedPtr<FJsonObject> FWriteGate::BuildAuditJson(const FMutationPlan& Plan, bool bExecuted)
{
        TSharedPtr<FJsonObject> Audit = MakeShared<FJsonObject>();
        Audit->SetBoolField(TEXT("mutation"), true);
        Audit->SetBoolField(TEXT("dryRun"), Plan.bDryRun);
        Audit->SetBoolField(TEXT("executed"), bExecuted);
        Audit->SetStringField(TEXT("transaction"), GetTransactionName());
        Audit->SetBoolField(TEXT("undoAvailable"), bExecuted);

        TArray<TSharedPtr<FJsonValue>> Actions;
        for (const FMutationAction& Action : Plan.Actions)
        {
                        TSharedPtr<FJsonObject> ActionObject = MakeShared<FJsonObject>();
                        ActionObject->SetStringField(TEXT("op"), Action.Op);

                        TSharedPtr<FJsonObject> ArgsObject = MakeShared<FJsonObject>();
                        for (const auto& ArgPair : Action.Args)
                        {
                                ArgsObject->SetStringField(ArgPair.Key, ArgPair.Value);
                        }
                        ActionObject->SetObjectField(TEXT("args"), ArgsObject);
                        Actions.Add(MakeShared<FJsonValueObject>(ActionObject));
        }

        Audit->SetArrayField(TEXT("actions"), Actions);
        return Audit;
}

const FString& FWriteGate::GetTransactionName()
{
        static const FString TransactionName(TransactionLabel);
        return TransactionName;
}

bool FWriteGate::EnsureCheckoutForContentPath(const FString& ContentPath, TSharedPtr<FJsonObject>& OutError)
{
        const UUnrealMCPSettings* Settings = GetSettings();
        if (!Settings || !Settings->RequireCheckout)
        {
                return true;
        }

        if (ContentPath.IsEmpty())
        {
                return true;
        }

        FString PackagePath = ContentPath;
        if (PackagePath.Contains(TEXT(".")))
        {
                PackagePath = FPackageName::ObjectPathToPackageName(PackagePath);
        }

        if (!FPackageName::IsValidLongPackageName(PackagePath))
        {
                return true;
        }

        TArray<FString> AssetPaths;
        AssetPaths.Add(PackagePath);

        TArray<FString> Files;
        FString ConversionError;
        if (!FSourceControlService::AssetPathsToFiles(AssetPaths, Files, ConversionError))
        {
                OutError = MakeSourceControlRequiredError(ContentPath, ConversionError);
                return false;
        }

        if (Files.Num() == 0)
        {
                return true;
        }

        TMap<FString, bool> PerFileResult;
        FString CheckoutError;
        if (!FSourceControlService::Checkout(Files, PerFileResult, CheckoutError))
        {
                OutError = MakeSourceControlRequiredError(ContentPath, CheckoutError);
                return false;
        }

        for (const TPair<FString, bool>& Pair : PerFileResult)
        {
                if (!Pair.Value)
                {
                        OutError = MakeSourceControlRequiredError(ContentPath, CheckoutError);
                        return false;
                }
        }

        return true;
}

TSharedPtr<FJsonObject> FWriteGate::MakeSourceControlRequiredError(const FString& AssetPath, const FString& FailureMessage)
{
        TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
        Error->SetStringField(TEXT("code"), TEXT("SOURCE_CONTROL_REQUIRED"));
        Error->SetStringField(TEXT("message"), TEXT("Asset must be checked out before mutation"));

        TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
        Details->SetStringField(TEXT("asset"), NormalizeContentPath(AssetPath));
        if (!FailureMessage.IsEmpty())
        {
                Details->SetStringField(TEXT("reason"), FailureMessage);
        }

        Error->SetObjectField(TEXT("details"), Details);
        return Error;
}

TSharedPtr<FJsonObject> FWriteGate::MakeWriteNotAllowedError(const FString& CommandType)
{
        TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
        Error->SetStringField(TEXT("code"), TEXT("WRITE_NOT_ALLOWED"));
        Error->SetStringField(TEXT("message"), TEXT("Write operations are disabled (allowWrite=false)"));
        TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
        Details->SetStringField(TEXT("tool"), CommandType);
        Error->SetObjectField(TEXT("details"), Details);
        return Error;
}

TSharedPtr<FJsonObject> FWriteGate::MakePathNotAllowedError(const FString& Path, const FString& Reason)
{
        TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
        Error->SetStringField(TEXT("code"), TEXT("PATH_NOT_ALLOWED"));
        Error->SetStringField(TEXT("message"), Reason);
        TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
        Details->SetStringField(TEXT("path"), NormalizeContentPath(Path));
        Error->SetObjectField(TEXT("details"), Details);
        return Error;
}

TSharedPtr<FJsonObject> FWriteGate::MakeToolNotAllowedError(const FString& CommandType, const FString& Reason)
{
        TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
        Error->SetStringField(TEXT("code"), TEXT("TOOL_DENIED"));
        Error->SetStringField(TEXT("message"), Reason);

        TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
        Details->SetStringField(TEXT("tool"), CommandType);
        Error->SetObjectField(TEXT("details"), Details);
        return Error;
}

void FWriteGate::UpdateRemoteEnforcement(bool bAllowWrite, bool bDryRun, const TArray<FString>& AllowedPaths, const TArray<FString>& AllowedTools, const TArray<FString>& DeniedTools)
{
        FScopeLock Lock(&GRemoteStateMutex);
        GRemoteAllowWrite = bAllowWrite;
        GRemoteDryRun = bDryRun;

        GRemoteAllowedPaths.Reset();
        for (const FString& Path : AllowedPaths)
        {
                const FString Normalized = NormalizeContentPath(Path);
                if (!Normalized.IsEmpty())
                {
                        GRemoteAllowedPaths.AddUnique(Normalized);
                }
        }

        GRemoteAllowedTools.Reset();
        for (const FString& Tool : AllowedTools)
        {
                FString Normalized = Tool;
                Normalized.TrimStartAndEndInline();
                if (!Normalized.IsEmpty())
                {
                        GRemoteAllowedTools.AddUnique(Normalized);
                }
        }

        GRemoteDeniedTools.Reset();
        for (const FString& Tool : DeniedTools)
        {
                FString Normalized = Tool;
                Normalized.TrimStartAndEndInline();
                if (!Normalized.IsEmpty())
                {
                        GRemoteDeniedTools.AddUnique(Normalized);
                }
        }
}

FString FWriteGate::ResolvePathForCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
        if (!Params.IsValid())
        {
                return FString();
        }

        if (CommandType == TEXT("camera.bookmark") && Params.IsValid())
        {
                FString Op;
                if (Params->TryGetStringField(TEXT("op"), Op) && Op.Equals(TEXT("set"), ESearchCase::IgnoreCase))
                {
                        if (Params->HasTypedField<EJson::Boolean>(TEXT("persist")) && Params->GetBoolField(TEXT("persist")))
                        {
#if WITH_EDITOR
                                if (GEditor)
                                {
                                        if (UWorld* World = GEditor->GetEditorWorldContext().World())
                                        {
                                                if (ULevel* Level = World->GetCurrentLevel())
                                                {
                                                        if (UPackage* Package = Level->GetOutermost())
                                                        {
                                                                return NormalizeContentPath(Package->GetName());
                                                        }
                                                }
                                        }
                                }
#endif
                        }
                }
        }

        if (CommandType.StartsWith(TEXT("sc.")))
        {
                const TArray<TSharedPtr<FJsonValue>>* AssetsArray = nullptr;
                if (Params->TryGetArrayField(TEXT("assets"), AssetsArray))
                {
                        for (const TSharedPtr<FJsonValue>& Value : *AssetsArray)
                        {
                                if (Value->Type == EJson::String)
                                {
                                        return NormalizeContentPath(Value->AsString());
                                }
                        }
                }
        }

        static const TArray<FString> CandidateKeys = {
                TEXT("path"),
                TEXT("asset_path"),
                TEXT("asset"),
                TEXT("objectPath"),
                TEXT("fromObjectPath"),
                TEXT("toPackagePath"),
                TEXT("blueprint_path"),
                TEXT("content_path"),
                TEXT("target_path"),
                TEXT("parent_path"),
                TEXT("widget_path"),
                TEXT("source_path"),
                TEXT("package_path"),
                TEXT("destPath"),
                TEXT("sequencePath")
        };

        for (const FString& Key : CandidateKeys)
        {
                if (Params->HasField(Key) && Params->HasTypedField<EJson::String>(Key))
                {
                        return NormalizeContentPath(Params->GetStringField(Key));
                }
        }

        // Some commands use "name" to represent a content path (e.g. blueprint creation)
        if (CommandType.Contains(TEXT("blueprint")) && Params->HasTypedField<EJson::String>(TEXT("name")))
        {
                return NormalizeContentPath(Params->GetStringField(TEXT("name")));
        }

        if (Params->HasTypedField<EJson::String>(TEXT("widget_name")))
        {
                return NormalizeContentPath(Params->GetStringField(TEXT("widget_name")));
        }

        if (CommandType == TEXT("asset.delete") && Params.IsValid())
        {
                const TArray<TSharedPtr<FJsonValue>>* ObjectPaths = nullptr;
                if (Params->TryGetArrayField(TEXT("objectPaths"), ObjectPaths))
                {
                        for (const TSharedPtr<FJsonValue>& Value : *ObjectPaths)
                        {
                                if (Value->Type == EJson::String)
                                {
                                        return NormalizeContentPath(Value->AsString());
                                }
                        }
                }
        }

        if (CommandType == TEXT("asset.fix_redirectors") && Params.IsValid())
        {
                const TArray<TSharedPtr<FJsonValue>>* Paths = nullptr;
                if (Params->TryGetArrayField(TEXT("paths"), Paths))
                {
                        for (const TSharedPtr<FJsonValue>& Value : *Paths)
                        {
                                if (Value->Type == EJson::String)
                                {
                                        return NormalizeContentPath(Value->AsString());
                                }
                        }
                }
        }

        if (CommandType == TEXT("asset.save_all") && Params.IsValid())
        {
                const TArray<TSharedPtr<FJsonValue>>* Paths = nullptr;
                if (Params->TryGetArrayField(TEXT("paths"), Paths))
                {
                        for (const TSharedPtr<FJsonValue>& Value : *Paths)
                        {
                                if (Value->Type == EJson::String)
                                {
                                        return NormalizeContentPath(Value->AsString());
                                }
                        }
                }
        }

        return FString();
}

FString FWriteGate::NormalizeContentPath(const FString& InPath)
{
        FString Trimmed = InPath;
        Trimmed.TrimStartAndEndInline();
        if (Trimmed.IsEmpty())
        {
                return FString();
        }

        if (!Trimmed.StartsWith(TEXT("/")))
        {
                Trimmed = FString::Printf(TEXT("/Game/%s"), *Trimmed);
        }

        return Trimmed;
}
