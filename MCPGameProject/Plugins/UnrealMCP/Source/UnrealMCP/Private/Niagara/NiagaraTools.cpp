#include "CoreMinimal.h"
#include "Niagara/NiagaraTools.h"

#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Engine/TextureRenderTarget.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Misc/Char.h"
#include "Misc/LexFromString.h"
#include "Misc/LexToString.h"
#include "NiagaraActor.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UObjectGlobals.h"

namespace
{
        constexpr const TCHAR* ErrorCodeInvalidParams = TEXT("INVALID_PARAMETERS");
        constexpr const TCHAR* ErrorCodeSystemNotFound = TEXT("SYSTEM_NOT_FOUND");
        constexpr const TCHAR* ErrorCodeActorNotFound = TEXT("ACTOR_NOT_FOUND");
        constexpr const TCHAR* ErrorCodeComponentNotFound = TEXT("COMPONENT_NOT_FOUND");
        constexpr const TCHAR* ErrorCodeSpawnFailed = TEXT("SPAWN_FAILED");
        constexpr const TCHAR* ErrorCodeParamUnsupported = TEXT("PARAM_TYPE_UNSUPPORTED");
        constexpr const TCHAR* ErrorCodeParamFailed = TEXT("PARAM_APPLY_FAILED");
        constexpr const TCHAR* ErrorCodeAssetNotFound = TEXT("ASSET_NOT_FOUND");

        TSharedPtr<FJsonObject> MakeErrorResponse(const FString& Code, const FString& Message)
        {
                TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
                Error->SetBoolField(TEXT("success"), false);
                Error->SetBoolField(TEXT("ok"), false);
                Error->SetStringField(TEXT("errorCode"), Code);
                Error->SetStringField(TEXT("error"), Message);
                return Error;
        }

        TSharedPtr<FJsonObject> MakeSuccessResponse()
        {
                TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
                Result->SetBoolField(TEXT("success"), true);
                Result->SetBoolField(TEXT("ok"), true);
                return Result;
        }

        TSharedPtr<FJsonObject> MakeAuditObject(bool bDryRun, const TArray<TSharedPtr<FJsonValue>>& Actions)
        {
                TSharedPtr<FJsonObject> Audit = MakeShared<FJsonObject>();
                Audit->SetBoolField(TEXT("dryRun"), bDryRun);
                Audit->SetArrayField(TEXT("actions"), Actions);
                return Audit;
        }

        TSharedPtr<FJsonObject> MakeAuditAction(const FString& Op, const TMap<FString, FString>& Args)
        {
                TSharedPtr<FJsonObject> Action = MakeShared<FJsonObject>();
                Action->SetStringField(TEXT("op"), Op);
                for (const TPair<FString, FString>& Pair : Args)
                {
                        Action->SetStringField(Pair.Key, Pair.Value);
                }
                return Action;
        }

        bool SerializeJsonValueInternal(const TSharedPtr<FJsonValue>& Value, const TSharedRef<TJsonWriter<>>& Writer)
        {
                if (!Value.IsValid())
                {
                        return false;
                }

                switch (Value->Type)
                {
                case EJson::Object:
                {
                        const TSharedPtr<FJsonObject> Object = Value->AsObject();
                        return Object.IsValid() && FJsonSerializer::Serialize(Object.ToSharedRef(), Writer, /*bCloseWriter=*/false);
                }
                case EJson::Array:
                {
                        const TArray<TSharedPtr<FJsonValue>> Array = Value->AsArray();
                        return FJsonSerializer::Serialize(Array, Writer, /*bCloseWriter=*/false);
                }
                case EJson::Null:
                        Writer->WriteNull();
                        return true;
                default:
                        return false;
                }
        }

        FString SerializeJsonValue(const TSharedPtr<FJsonValue>& Value)
        {
                if (!Value.IsValid())
                {
                        return TEXT("null");
                }

                if (Value->Type == EJson::String)
                {
                        return Value->AsString();
                }
                if (Value->Type == EJson::Number)
                {
                        return FString::SanitizeFloat(Value->AsNumber());
                }
                if (Value->Type == EJson::Boolean)
                {
                        return Value->AsBool() ? TEXT("true") : TEXT("false");
                }
                if (Value->Type == EJson::Null)
                {
                        return TEXT("null");
                }

                FString Serialized;
                TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
                if (!SerializeJsonValueInternal(Value, Writer))
                {
                        return TEXT("null");
                }
                Writer->Close();
                return Serialized;
        }

        UWorld* GetEditorWorld()
        {
#if WITH_EDITOR
                if (!GEditor)
                {
                        return nullptr;
                }

                if (FWorldContext* PieContext = GEditor->GetPIEWorldContext())
                {
                        if (PieContext->World())
                        {
                                return PieContext->World();
                        }
                }

                return GEditor->GetEditorWorldContext().World();
#else
                return nullptr;
#endif
        }

        AActor* ResolveActor(const FString& ActorPath)
        {
                FString Trimmed = ActorPath;
                Trimmed.TrimStartAndEndInline();
                if (Trimmed.IsEmpty())
                {
                        return nullptr;
                }

                if (AActor* Actor = FindObject<AActor>(nullptr, *Trimmed))
                {
                        return Actor;
                }

                if (UWorld* World = GetEditorWorld())
                {
                        for (TActorIterator<AActor> It(World); It; ++It)
                        {
                                if (It->GetPathName() == Trimmed || It->GetName() == Trimmed)
                                {
                                        return *It;
                                }
                        }
                }

                return nullptr;
        }

        UNiagaraComponent* ResolveNiagaraComponent(const FString& ComponentPath)
        {
                FString Trimmed = ComponentPath;
                Trimmed.TrimStartAndEndInline();
                if (Trimmed.IsEmpty())
                {
                        return nullptr;
                }

                if (UNiagaraComponent* Component = FindObject<UNiagaraComponent>(nullptr, *Trimmed))
                {
                        return Component;
                }

                if (UWorld* World = GetEditorWorld())
                {
                        for (TActorIterator<AActor> It(World); It; ++It)
                        {
                                TInlineComponentArray<UNiagaraComponent*> NiagaraComponents;
                                It->GetComponents(NiagaraComponents);
                                for (UNiagaraComponent* Component : NiagaraComponents)
                                {
                                        if (!Component)
                                        {
                                                continue;
                                        }

                                        if (Component->GetPathName() == Trimmed || Component->GetName() == Trimmed)
                                        {
                                                return Component;
                                        }
                                }
                        }
                }

                return nullptr;
        }

        bool ParseNumber(const TSharedPtr<FJsonValue>& Value, double& OutNumber)
        {
                if (!Value.IsValid())
                {
                        return false;
                }

                if (Value->Type == EJson::Number)
                {
                        OutNumber = Value->AsNumber();
                        return true;
                }

                if (Value->Type == EJson::String)
                {
                        const FString StringValue = Value->AsString();
                        return LexTryParseString(OutNumber, *StringValue);
                }

                return false;
        }

        bool ParseBool(const TSharedPtr<FJsonValue>& Value, bool& bOutValue)
        {
                if (!Value.IsValid())
                {
                        return false;
                }

                if (Value->Type == EJson::Boolean)
                {
                        bOutValue = Value->AsBool();
                        return true;
                }

                if (Value->Type == EJson::Number)
                {
                        bOutValue = !FMath::IsNearlyZero(Value->AsNumber());
                        return true;
                }

                if (Value->Type == EJson::String)
                {
                        const FString StringValue = Value->AsString();
                        if (StringValue.Equals(TEXT("true"), ESearchCase::IgnoreCase) ||
                            StringValue.Equals(TEXT("1")))
                        {
                                bOutValue = true;
                                return true;
                        }
                        if (StringValue.Equals(TEXT("false"), ESearchCase::IgnoreCase) ||
                            StringValue.Equals(TEXT("0")))
                        {
                                bOutValue = false;
                                return true;
                        }
                }

                return false;
        }

        bool ParseVector(const TSharedPtr<FJsonValue>& Value, FVector& OutVector)
        {
                if (!Value.IsValid() || Value->Type != EJson::Array)
                {
                        return false;
                }

                const TArray<TSharedPtr<FJsonValue>>& Array = Value->AsArray();
                if (Array.Num() != 3)
                {
                        return false;
                }

                double Components[3];
                for (int32 Index = 0; Index < 3; ++Index)
                {
                        if (!ParseNumber(Array[Index], Components[Index]))
                        {
                                return false;
                        }
                }

                OutVector = FVector(static_cast<float>(Components[0]), static_cast<float>(Components[1]), static_cast<float>(Components[2]));
                return true;
        }

        bool ParseVector2D(const TSharedPtr<FJsonValue>& Value, FVector2D& OutVector)
        {
                if (!Value.IsValid() || Value->Type != EJson::Array)
                {
                        return false;
                }

                const TArray<TSharedPtr<FJsonValue>>& Array = Value->AsArray();
                if (Array.Num() != 2)
                {
                        return false;
                }

                double Components[2];
                for (int32 Index = 0; Index < 2; ++Index)
                {
                        if (!ParseNumber(Array[Index], Components[Index]))
                        {
                                return false;
                        }
                }

                OutVector = FVector2D(static_cast<float>(Components[0]), static_cast<float>(Components[1]));
                return true;
        }

        bool ParseVector4(const TSharedPtr<FJsonValue>& Value, FVector4& OutVector)
        {
                if (!Value.IsValid() || Value->Type != EJson::Array)
                {
                        return false;
                }

                const TArray<TSharedPtr<FJsonValue>>& Array = Value->AsArray();
                if (Array.Num() != 4)
                {
                        return false;
                }

                double Components[4];
                for (int32 Index = 0; Index < 4; ++Index)
                {
                        if (!ParseNumber(Array[Index], Components[Index]))
                        {
                                return false;
                        }
                }

                OutVector = FVector4(static_cast<float>(Components[0]), static_cast<float>(Components[1]), static_cast<float>(Components[2]), static_cast<float>(Components[3]));
                return true;
        }

        bool ParseLinearColor(const TSharedPtr<FJsonValue>& Value, FLinearColor& OutColor)
        {
                FVector4 Vector;
                if (!ParseVector4(Value, Vector))
                {
                        return false;
                }

                OutColor = FLinearColor(Vector.X, Vector.Y, Vector.Z, Vector.W);
                return true;
        }

        bool ParseQuat(const TSharedPtr<FJsonValue>& Value, FQuat& OutQuat)
        {
                if (!Value.IsValid() || Value->Type != EJson::Array)
                {
                        return false;
                }

                const TArray<TSharedPtr<FJsonValue>>& Array = Value->AsArray();
                if (Array.Num() != 4)
                {
                        return false;
                }

                double Components[4];
                for (int32 Index = 0; Index < 4; ++Index)
                {
                        if (!ParseNumber(Array[Index], Components[Index]))
                        {
                                return false;
                        }
                }

                OutQuat = FQuat(static_cast<float>(Components[0]), static_cast<float>(Components[1]), static_cast<float>(Components[2]), static_cast<float>(Components[3]));
                return true;
        }

        bool ParseMatrix(const TSharedPtr<FJsonValue>& Value, FMatrix& OutMatrix)
        {
                if (!Value.IsValid() || Value->Type != EJson::Array)
                {
                        return false;
                }

                const TArray<TSharedPtr<FJsonValue>>& Array = Value->AsArray();
                if (Array.Num() != 16)
                {
                        return false;
                }

                double Components[16];
                for (int32 Index = 0; Index < 16; ++Index)
                {
                        if (!ParseNumber(Array[Index], Components[Index]))
                        {
                                return false;
                        }
                }

                OutMatrix = FMatrix(
                        FPlane(Components[0], Components[1], Components[2], Components[3]),
                        FPlane(Components[4], Components[5], Components[6], Components[7]),
                        FPlane(Components[8], Components[9], Components[10], Components[11]),
                        FPlane(Components[12], Components[13], Components[14], Components[15]));
                return true;
        }

        bool ParseTransform(const TSharedPtr<FJsonObject>& Params, FTransform& OutTransform)
        {
                if (!Params.IsValid() || !Params->HasTypedField<EJson::Object>(TEXT("transform")))
                {
                        OutTransform = FTransform::Identity;
                        return false;
                }

                const TSharedPtr<FJsonObject> TransformObject = Params->GetObjectField(TEXT("transform"));

                FVector Location = FVector::ZeroVector;
                if (TransformObject->HasField(TEXT("location")))
                {
                        ParseVector(TransformObject->TryGetField(TEXT("location")), Location);
                }

                FVector RotationVector = FVector::ZeroVector;
                if (TransformObject->HasField(TEXT("rotation")))
                {
                        ParseVector(TransformObject->TryGetField(TEXT("rotation")), RotationVector);
                }

                FVector Scale = FVector::OneVector;
                if (TransformObject->HasField(TEXT("scale")))
                {
                        ParseVector(TransformObject->TryGetField(TEXT("scale")), Scale);
                }

                FRotator Rotator(RotationVector.X, RotationVector.Y, RotationVector.Z);
                OutTransform = FTransform(Rotator, Location, Scale);
                return true;
        }

        template <typename TObjectType>
        TObjectType* LoadAssetByPath(const FString& Path)
        {
                FString Trimmed = Path;
                Trimmed.TrimStartAndEndInline();
                if (Trimmed.IsEmpty())
                {
                        return nullptr;
                }

                return LoadObject<TObjectType>(nullptr, *Trimmed);
        }

        bool ApplyUserParameter(UNiagaraComponent& Component, const FString& RawKey, const TSharedPtr<FJsonValue>& Value, FString& OutErrorCode, FString& OutErrorMessage, TArray<FString>& OutApplied, TArray<TSharedPtr<FJsonValue>>& OutAuditActions)
        {
                FString Type;
                FString Name;
                if (!RawKey.Split(TEXT(":"), &Type, &Name))
                {
                        OutErrorCode = ErrorCodeInvalidParams;
                        OutErrorMessage = FString::Printf(TEXT("Parameter key '%s' must be formatted as Type:Name"), *RawKey);
                        return false;
                }

                Type.TrimStartAndEndInline();
                Name.TrimStartAndEndInline();
                if (Type.IsEmpty() || Name.IsEmpty())
                {
                        OutErrorCode = ErrorCodeInvalidParams;
                        OutErrorMessage = FString::Printf(TEXT("Parameter key '%s' is missing type or name"), *RawKey);
                        return false;
                }

                const FName ParameterName(*Name);
                const FString TypeUpper = Type.ToUpper();

                if (TypeUpper == TEXT("FLOAT"))
                {
                        double Number = 0.0;
                        if (!ParseNumber(Value, Number))
                        {
                                OutErrorCode = ErrorCodeParamFailed;
                                OutErrorMessage = FString::Printf(TEXT("Parameter '%s' expected numeric value"), *RawKey);
                                return false;
                        }

                        Component.SetVariableFloat(ParameterName, static_cast<float>(Number));
                }
                else if (TypeUpper == TEXT("INT") || TypeUpper == TEXT("INTEGER"))
                {
                        double Number = 0.0;
                        if (!ParseNumber(Value, Number))
                        {
                                OutErrorCode = ErrorCodeParamFailed;
                                OutErrorMessage = FString::Printf(TEXT("Parameter '%s' expected integer value"), *RawKey);
                                return false;
                        }

                        Component.SetVariableInt(ParameterName, static_cast<int32>(Number));
                }
                else if (TypeUpper == TEXT("BOOL") || TypeUpper == TEXT("BOOLEAN"))
                {
                        bool bBoolValue = false;
                        if (!ParseBool(Value, bBoolValue))
                        {
                                OutErrorCode = ErrorCodeParamFailed;
                                OutErrorMessage = FString::Printf(TEXT("Parameter '%s' expected boolean value"), *RawKey);
                                return false;
                        }

                        Component.SetVariableBool(ParameterName, bBoolValue);
                }
                else if (TypeUpper == TEXT("VECTOR2") || TypeUpper == TEXT("VEC2"))
                {
                        FVector2D VectorValue;
                        if (!ParseVector2D(Value, VectorValue))
                        {
                                OutErrorCode = ErrorCodeParamFailed;
                                OutErrorMessage = FString::Printf(TEXT("Parameter '%s' expected 2-component array"), *RawKey);
                                return false;
                        }

                        Component.SetVariableVec2(ParameterName, VectorValue);
                }
                else if (TypeUpper == TEXT("VECTOR") || TypeUpper == TEXT("VECTOR3") || TypeUpper == TEXT("VEC3"))
                {
                        FVector VectorValue;
                        if (!ParseVector(Value, VectorValue))
                        {
                                OutErrorCode = ErrorCodeParamFailed;
                                OutErrorMessage = FString::Printf(TEXT("Parameter '%s' expected 3-component array"), *RawKey);
                                return false;
                        }

                        Component.SetVariableVec3(ParameterName, VectorValue);
                }
                else if (TypeUpper == TEXT("VECTOR4") || TypeUpper == TEXT("VEC4"))
                {
                        FVector4 VectorValue;
                        if (!ParseVector4(Value, VectorValue))
                        {
                                OutErrorCode = ErrorCodeParamFailed;
                                OutErrorMessage = FString::Printf(TEXT("Parameter '%s' expected 4-component array"), *RawKey);
                                return false;
                        }

                        Component.SetVariableVec4(ParameterName, VectorValue);
                }
                else if (TypeUpper == TEXT("COLOR") || TypeUpper == TEXT("LINEARCOLOR"))
                {
                        FLinearColor ColorValue;
                        if (!ParseLinearColor(Value, ColorValue))
                        {
                                OutErrorCode = ErrorCodeParamFailed;
                                OutErrorMessage = FString::Printf(TEXT("Parameter '%s' expected 4-component color array"), *RawKey);
                                return false;
                        }

                        Component.SetVariableLinearColor(ParameterName, ColorValue);
                }
                else if (TypeUpper == TEXT("QUAT") || TypeUpper == TEXT("QUATERNION"))
                {
                        FQuat QuatValue;
                        if (!ParseQuat(Value, QuatValue))
                        {
                                OutErrorCode = ErrorCodeParamFailed;
                                OutErrorMessage = FString::Printf(TEXT("Parameter '%s' expected quaternion array"), *RawKey);
                                return false;
                        }

                        Component.SetVariableQuat(ParameterName, QuatValue);
                }
                else if (TypeUpper == TEXT("MATRIX"))
                {
                        FMatrix MatrixValue;
                        if (!ParseMatrix(Value, MatrixValue))
                        {
                                OutErrorCode = ErrorCodeParamFailed;
                                OutErrorMessage = FString::Printf(TEXT("Parameter '%s' expected 16-component matrix array"), *RawKey);
                                return false;
                        }

                        Component.SetVariableMatrix(ParameterName, MatrixValue);
                }
                else if (TypeUpper == TEXT("TEXTURE"))
                {
                        if (!Value.IsValid() || Value->Type != EJson::String)
                        {
                                OutErrorCode = ErrorCodeParamFailed;
                                OutErrorMessage = FString::Printf(TEXT("Parameter '%s' expected texture asset path"), *RawKey);
                                return false;
                        }

                        FString AssetPath = Value->AsString();
                        if (UTexture* Texture = LoadAssetByPath<UTexture>(AssetPath))
                        {
                                Component.SetVariableTexture(ParameterName, Texture);
                        }
                        else
                        {
                                OutErrorCode = ErrorCodeAssetNotFound;
                                OutErrorMessage = FString::Printf(TEXT("Texture asset '%s' not found"), *AssetPath);
                                return false;
                        }
                }
                else if (TypeUpper == TEXT("RENDERTARGET") || TypeUpper == TEXT("TEXTURERENDERTARGET"))
                {
                        if (!Value.IsValid() || Value->Type != EJson::String)
                        {
                                OutErrorCode = ErrorCodeParamFailed;
                                OutErrorMessage = FString::Printf(TEXT("Parameter '%s' expected render target asset path"), *RawKey);
                                return false;
                        }

                        FString AssetPath = Value->AsString();
                        if (UTextureRenderTarget* RenderTarget = LoadAssetByPath<UTextureRenderTarget>(AssetPath))
                        {
                                Component.SetVariableTextureRenderTarget(ParameterName, RenderTarget);
                        }
                        else
                        {
                                OutErrorCode = ErrorCodeAssetNotFound;
                                OutErrorMessage = FString::Printf(TEXT("Render target asset '%s' not found"), *AssetPath);
                                return false;
                        }
                }
                else if (TypeUpper == TEXT("TEXTURERENDERTARGET2D") || TypeUpper == TEXT("RENDERTARGET2D"))
                {
                        if (!Value.IsValid() || Value->Type != EJson::String)
                        {
                                OutErrorCode = ErrorCodeParamFailed;
                                OutErrorMessage = FString::Printf(TEXT("Parameter '%s' expected render target asset path"), *RawKey);
                                return false;
                        }

                        FString AssetPath = Value->AsString();
                        if (UTextureRenderTarget2D* RenderTarget = LoadAssetByPath<UTextureRenderTarget2D>(AssetPath))
                        {
                                Component.SetVariableTextureRenderTarget(ParameterName, RenderTarget);
                        }
                        else
                        {
                                OutErrorCode = ErrorCodeAssetNotFound;
                                OutErrorMessage = FString::Printf(TEXT("Render target asset '%s' not found"), *AssetPath);
                                return false;
                        }
                }
                else if (TypeUpper == TEXT("STATICMESH"))
                {
                        if (!Value.IsValid() || Value->Type != EJson::String)
                        {
                                OutErrorCode = ErrorCodeParamFailed;
                                OutErrorMessage = FString::Printf(TEXT("Parameter '%s' expected static mesh asset path"), *RawKey);
                                return false;
                        }

                        FString AssetPath = Value->AsString();
                        if (UStaticMesh* StaticMesh = LoadAssetByPath<UStaticMesh>(AssetPath))
                        {
                                Component.SetVariableStaticMesh(ParameterName, StaticMesh);
                        }
                        else
                        {
                                OutErrorCode = ErrorCodeAssetNotFound;
                                OutErrorMessage = FString::Printf(TEXT("Static mesh asset '%s' not found"), *AssetPath);
                                return false;
                        }
                }
                else if (TypeUpper == TEXT("SKELETALMESH"))
                {
                        if (!Value.IsValid() || Value->Type != EJson::String)
                        {
                                OutErrorCode = ErrorCodeParamFailed;
                                OutErrorMessage = FString::Printf(TEXT("Parameter '%s' expected skeletal mesh asset path"), *RawKey);
                                return false;
                        }

                        FString AssetPath = Value->AsString();
                        if (USkeletalMesh* SkeletalMesh = LoadAssetByPath<USkeletalMesh>(AssetPath))
                        {
                                Component.SetVariableObject(ParameterName, SkeletalMesh);
                        }
                        else
                        {
                                OutErrorCode = ErrorCodeAssetNotFound;
                                OutErrorMessage = FString::Printf(TEXT("Skeletal mesh asset '%s' not found"), *AssetPath);
                                return false;
                        }
                }
                else
                {
                        OutErrorCode = ErrorCodeParamUnsupported;
                        OutErrorMessage = FString::Printf(TEXT("Unsupported Niagara user parameter type '%s'"), *Type);
                        return false;
                }

                OutApplied.Add(RawKey);

                TMap<FString, FString> Args;
                Args.Add(TEXT("component"), Component.GetPathName());
                Args.Add(TEXT("name"), RawKey);
                Args.Add(TEXT("value"), SerializeJsonValue(Value));
                OutAuditActions.Add(MakeShared<FJsonValueObject>(MakeAuditAction(TEXT("set_user_param"), Args)));
                return true;
        }

        bool ApplyUserParameterSet(UNiagaraComponent& Component, const TSharedPtr<FJsonObject>& ParamObject, FString& OutErrorCode, FString& OutErrorMessage, TArray<FString>& OutApplied, TArray<TSharedPtr<FJsonValue>>& OutAuditActions)
        {
                if (!ParamObject.IsValid())
                {
                        return true;
                }

                for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : ParamObject->Values)
                {
                        if (!ApplyUserParameter(Component, Pair.Key, Pair.Value, OutErrorCode, OutErrorMessage, OutApplied, OutAuditActions))
                        {
                                return false;
                        }
                }

                return true;
        }

        void SelectActor(AActor* Actor)
        {
#if WITH_EDITOR
                if (!GEditor || !Actor)
                {
                        return;
                }

                GEditor->SelectNone(false, true, false);
                GEditor->SelectActor(Actor, true, true, true);
                GEditor->NoteSelectionChange();
#endif
        }
}

TSharedPtr<FJsonObject> FNiagaraTools::SpawnComponent(const TSharedPtr<FJsonObject>& Params)
{
        if (!Params.IsValid())
        {
                return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Missing parameters"));
        }

        FString SystemPath;
        if (!Params->TryGetStringField(TEXT("systemPath"), SystemPath))
        {
                return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Missing systemPath parameter"));
        }

        SystemPath.TrimStartAndEndInline();
        if (SystemPath.IsEmpty())
        {
                return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Missing systemPath parameter"));
        }

        UNiagaraSystem* NiagaraSystem = LoadObject<UNiagaraSystem>(nullptr, *SystemPath);
        if (!NiagaraSystem)
        {
                return MakeErrorResponse(ErrorCodeSystemNotFound, FString::Printf(TEXT("Niagara system '%s' not found"), *SystemPath));
        }

        UWorld* World = GetEditorWorld();
        if (!World)
        {
                return MakeErrorResponse(ErrorCodeSpawnFailed, TEXT("Unable to resolve editor world"));
        }

        const bool bAutoActivate = !Params->HasField(TEXT("autoActivate")) || Params->GetBoolField(TEXT("autoActivate"));
        const bool bSelect = Params->HasTypedField<EJson::Boolean>(TEXT("select")) && Params->GetBoolField(TEXT("select"));

        UNiagaraComponent* SpawnedComponent = nullptr;
        AActor* OwningActor = nullptr;
        bool bAttached = false;
        FString AttachSocket;
        bool bKeepWorld = false;

        const TSharedPtr<FJsonObject>* AttachObject = nullptr;
        if (Params->TryGetObjectField(TEXT("attach"), AttachObject) && AttachObject && AttachObject->IsValid())
        {
                FString ActorPath;
                if (!(*AttachObject)->TryGetStringField(TEXT("actorPath"), ActorPath))
                {
                        return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("attach.actorPath is required when attach is provided"));
                }

                ActorPath.TrimStartAndEndInline();
                if (ActorPath.IsEmpty())
                {
                        return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("attach.actorPath is required when attach is provided"));
                }

                OwningActor = ResolveActor(ActorPath);
                if (!OwningActor)
                {
                        return MakeErrorResponse(ErrorCodeActorNotFound, FString::Printf(TEXT("Actor '%s' not found"), *ActorPath));
                }

                (*AttachObject)->TryGetStringField(TEXT("socketName"), AttachSocket);

                bKeepWorld = (*AttachObject)->HasTypedField<EJson::Boolean>(TEXT("keepWorldTransform")) && (*AttachObject)->GetBoolField(TEXT("keepWorldTransform"));

                USceneComponent* ParentComponent = OwningActor->GetRootComponent();
                if (!ParentComponent)
                {
                        return MakeErrorResponse(ErrorCodeSpawnFailed, TEXT("Target actor has no root component"));
                }

                OwningActor->Modify();

                SpawnedComponent = NewObject<UNiagaraComponent>(OwningActor, NAME_None, RF_Transactional);
                if (!SpawnedComponent)
                {
                        return MakeErrorResponse(ErrorCodeSpawnFailed, TEXT("Failed to create Niagara component"));
                }

                SpawnedComponent->SetFlags(RF_Transactional);
                SpawnedComponent->SetAsset(NiagaraSystem);
                SpawnedComponent->SetAutoActivate(false);
                SpawnedComponent->SetUsingAbsoluteLocation(bKeepWorld);
                SpawnedComponent->SetUsingAbsoluteRotation(bKeepWorld);
                SpawnedComponent->SetUsingAbsoluteScale(bKeepWorld);
                SpawnedComponent->SetupAttachment(ParentComponent, FName(*AttachSocket));
                OwningActor->AddInstanceComponent(SpawnedComponent);
                SpawnedComponent->OnComponentCreated();
                SpawnedComponent->RegisterComponent();

                if (bKeepWorld)
                {
                        SpawnedComponent->AttachToComponent(ParentComponent, FAttachmentTransformRules::KeepWorldTransform, FName(*AttachSocket));
                }
                else
                {
                        SpawnedComponent->AttachToComponent(ParentComponent, FAttachmentTransformRules::SnapToTargetNotIncludingScale, FName(*AttachSocket));
                        SpawnedComponent->SetRelativeLocation(FVector::ZeroVector);
                        SpawnedComponent->SetRelativeRotation(FRotator::ZeroRotator);
                        SpawnedComponent->SetRelativeScale3D(FVector::OneVector);
                }

                bAttached = true;
        }
        else
        {
                FTransform SpawnTransform = FTransform::Identity;
                ParseTransform(Params, SpawnTransform);

                FActorSpawnParameters SpawnParameters;
                SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
                SpawnParameters.ObjectFlags |= RF_Transactional;

                ANiagaraActor* NiagaraActor = World->SpawnActor<ANiagaraActor>(ANiagaraActor::StaticClass(), SpawnTransform, SpawnParameters);
                if (!NiagaraActor)
                {
                        return MakeErrorResponse(ErrorCodeSpawnFailed, TEXT("Failed to spawn Niagara actor"));
                }

                NiagaraActor->SetActorTransform(SpawnTransform);
                SpawnedComponent = NiagaraActor->GetNiagaraComponent();
                if (!SpawnedComponent)
                {
                        SpawnedComponent = NewObject<UNiagaraComponent>(NiagaraActor, NAME_None, RF_Transactional);
                        SpawnedComponent->SetupAttachment(NiagaraActor->GetRootComponent());
                        NiagaraActor->AddInstanceComponent(SpawnedComponent);
                        SpawnedComponent->OnComponentCreated();
                        SpawnedComponent->RegisterComponent();
                }

                SpawnedComponent->SetAsset(NiagaraSystem);
                SpawnedComponent->SetAutoActivate(false);
                OwningActor = NiagaraActor;
        }

        if (!SpawnedComponent)
        {
                return MakeErrorResponse(ErrorCodeSpawnFailed, TEXT("Failed to create Niagara component"));
        }

        SpawnedComponent->Modify();

        TArray<TSharedPtr<FJsonValue>> AuditActions;
        TMap<FString, FString> SpawnArgs;
        SpawnArgs.Add(TEXT("system"), NiagaraSystem->GetPathName());
        SpawnArgs.Add(TEXT("component"), SpawnedComponent->GetPathName());
        if (bAttached && OwningActor)
        {
                SpawnArgs.Add(TEXT("attachTo"), OwningActor->GetPathName());
                if (!AttachSocket.IsEmpty())
                {
                        SpawnArgs.Add(TEXT("socket"), AttachSocket);
                }
                SpawnArgs.Add(TEXT("keepWorld"), bKeepWorld ? TEXT("true") : TEXT("false"));
        }
        else if (OwningActor)
        {
                SpawnArgs.Add(TEXT("actor"), OwningActor->GetPathName());
                const FVector Location = OwningActor->GetActorLocation();
                const FRotator Rotation = OwningActor->GetActorRotation();
                const FVector Scale = OwningActor->GetActorScale3D();
                SpawnArgs.Add(TEXT("location"), FString::Printf(TEXT("[%g,%g,%g]"), Location.X, Location.Y, Location.Z));
                SpawnArgs.Add(TEXT("rotation"), FString::Printf(TEXT("[%g,%g,%g]"), Rotation.Pitch, Rotation.Yaw, Rotation.Roll));
                SpawnArgs.Add(TEXT("scale"), FString::Printf(TEXT("[%g,%g,%g]"), Scale.X, Scale.Y, Scale.Z));
        }

        AuditActions.Add(MakeShared<FJsonValueObject>(MakeAuditAction(TEXT("spawn_niagara"), SpawnArgs)));

        if (Params->HasTypedField<EJson::Object>(TEXT("initialUserParams")))
        {
                FString ErrorCode;
                FString ErrorMessage;
                TArray<FString> AppliedParams;
                if (!ApplyUserParameterSet(*SpawnedComponent, Params->GetObjectField(TEXT("initialUserParams")), ErrorCode, ErrorMessage, AppliedParams, AuditActions))
                {
                        if (SpawnedComponent)
                        {
                                SpawnedComponent->DeactivateImmediate();
                                SpawnedComponent->DestroyComponent();
                                if (OwningActor)
                                {
                                        OwningActor->RemoveInstanceComponent(SpawnedComponent);
                                }
                        }

                        if (!bAttached && OwningActor)
                        {
                                OwningActor->Destroy();
                        }

                        return MakeErrorResponse(ErrorCode, ErrorMessage);
                }
        }

        SpawnedComponent->SetAutoActivate(bAutoActivate);
        if (bAutoActivate)
        {
                SpawnedComponent->Activate(true);
        }
        else
        {
                SpawnedComponent->DeactivateImmediate();
        }

        if (bSelect && OwningActor)
        {
                SelectActor(OwningActor);
        }

        TSharedPtr<FJsonObject> Result = MakeSuccessResponse();
        Result->SetStringField(TEXT("componentPath"), SpawnedComponent->GetPathName());
        Result->SetStringField(TEXT("system"), NiagaraSystem->GetPathName());
        if (bAttached && OwningActor)
        {
                Result->SetStringField(TEXT("attachedTo"), OwningActor->GetPathName());
        }
        else if (OwningActor)
        {
                Result->SetStringField(TEXT("actor"), OwningActor->GetPathName());
        }
        Result->SetObjectField(TEXT("audit"), MakeAuditObject(false, AuditActions));

        return Result;
}

TSharedPtr<FJsonObject> FNiagaraTools::SetUserParameters(const TSharedPtr<FJsonObject>& Params)
{
        if (!Params.IsValid())
        {
                return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Missing parameters"));
        }

        FString ComponentPath;
        if (!Params->TryGetStringField(TEXT("componentPath"), ComponentPath))
        {
                return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Missing componentPath parameter"));
        }

        ComponentPath.TrimStartAndEndInline();
        if (ComponentPath.IsEmpty())
        {
                return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Missing componentPath parameter"));
        }

        if (!Params->HasTypedField<EJson::Object>(TEXT("params")))
        {
                return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("params must be an object"));
        }

        UNiagaraComponent* NiagaraComponent = ResolveNiagaraComponent(ComponentPath);
        if (!NiagaraComponent)
        {
                return MakeErrorResponse(ErrorCodeComponentNotFound, FString::Printf(TEXT("Niagara component '%s' not found"), *ComponentPath));
        }

        NiagaraComponent->Modify();
        AActor* OwnerActor = NiagaraComponent->GetOwner();
        if (OwnerActor)
        {
                OwnerActor->Modify();
        }

        TArray<TSharedPtr<FJsonValue>> AuditActions;
        TArray<FString> AppliedParams;
        FString ErrorCode;
        FString ErrorMessage;
        if (!ApplyUserParameterSet(*NiagaraComponent, Params->GetObjectField(TEXT("params")), ErrorCode, ErrorMessage, AppliedParams, AuditActions))
        {
                return MakeErrorResponse(ErrorCode, ErrorMessage);
        }

        const bool bSaveActor = Params->HasTypedField<EJson::Boolean>(TEXT("saveActor")) && Params->GetBoolField(TEXT("saveActor"));
        if (bSaveActor && OwnerActor)
        {
                OwnerActor->MarkPackageDirty();
        }

        TSharedPtr<FJsonObject> Result = MakeSuccessResponse();
        TArray<TSharedPtr<FJsonValue>> AppliedArray;
        for (const FString& Applied : AppliedParams)
        {
                AppliedArray.Add(MakeShared<FJsonValueString>(Applied));
        }
        Result->SetArrayField(TEXT("applied"), AppliedArray);
        TArray<TSharedPtr<FJsonValue>> NotFoundArray;
        Result->SetArrayField(TEXT("notFound"), NotFoundArray);
        Result->SetObjectField(TEXT("audit"), MakeAuditObject(false, AuditActions));

        return Result;
}

TSharedPtr<FJsonObject> FNiagaraTools::Activate(const TSharedPtr<FJsonObject>& Params)
{
        if (!Params.IsValid())
        {
                return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Missing parameters"));
        }

        FString ComponentPath;
        if (!Params->TryGetStringField(TEXT("componentPath"), ComponentPath))
        {
                return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Missing componentPath parameter"));
        }

        ComponentPath.TrimStartAndEndInline();
        if (ComponentPath.IsEmpty())
        {
                return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Missing componentPath parameter"));
        }

        UNiagaraComponent* NiagaraComponent = ResolveNiagaraComponent(ComponentPath);
        if (!NiagaraComponent)
        {
                return MakeErrorResponse(ErrorCodeComponentNotFound, FString::Printf(TEXT("Niagara component '%s' not found"), *ComponentPath));
        }

        const bool bReset = Params->HasTypedField<EJson::Boolean>(TEXT("reset")) && Params->GetBoolField(TEXT("reset"));

        NiagaraComponent->Modify();
        NiagaraComponent->Activate(bReset);

        TMap<FString, FString> Args;
        Args.Add(TEXT("component"), NiagaraComponent->GetPathName());
        Args.Add(TEXT("reset"), bReset ? TEXT("true") : TEXT("false"));

        TArray<TSharedPtr<FJsonValue>> AuditActions;
        AuditActions.Add(MakeShared<FJsonValueObject>(MakeAuditAction(TEXT("activate_niagara"), Args)));

        TSharedPtr<FJsonObject> Result = MakeSuccessResponse();
        Result->SetBoolField(TEXT("activated"), true);
        Result->SetObjectField(TEXT("audit"), MakeAuditObject(false, AuditActions));
        return Result;
}

TSharedPtr<FJsonObject> FNiagaraTools::Deactivate(const TSharedPtr<FJsonObject>& Params)
{
        if (!Params.IsValid())
        {
                return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Missing parameters"));
        }

        FString ComponentPath;
        if (!Params->TryGetStringField(TEXT("componentPath"), ComponentPath))
        {
                return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Missing componentPath parameter"));
        }

        ComponentPath.TrimStartAndEndInline();
        if (ComponentPath.IsEmpty())
        {
                return MakeErrorResponse(ErrorCodeInvalidParams, TEXT("Missing componentPath parameter"));
        }

        UNiagaraComponent* NiagaraComponent = ResolveNiagaraComponent(ComponentPath);
        if (!NiagaraComponent)
        {
                return MakeErrorResponse(ErrorCodeComponentNotFound, FString::Printf(TEXT("Niagara component '%s' not found"), *ComponentPath));
        }

        const bool bImmediate = Params->HasTypedField<EJson::Boolean>(TEXT("immediate")) && Params->GetBoolField(TEXT("immediate"));

        NiagaraComponent->Modify();
        if (bImmediate)
        {
                NiagaraComponent->DeactivateImmediate();
        }
        else
        {
                NiagaraComponent->Deactivate();
        }

        TMap<FString, FString> Args;
        Args.Add(TEXT("component"), NiagaraComponent->GetPathName());
        Args.Add(TEXT("immediate"), bImmediate ? TEXT("true") : TEXT("false"));

        TArray<TSharedPtr<FJsonValue>> AuditActions;
        AuditActions.Add(MakeShared<FJsonValueObject>(MakeAuditAction(TEXT("deactivate_niagara"), Args)));

        TSharedPtr<FJsonObject> Result = MakeSuccessResponse();
        Result->SetBoolField(TEXT("deactivated"), true);
        Result->SetObjectField(TEXT("audit"), MakeAuditObject(false, AuditActions));
        return Result;
}
