#include "MetaSounds/MetaSoundTools.h"
#include "CoreMinimal.h"

#include "Dom/JsonObject.h"

namespace
{
        constexpr const TCHAR* ErrorCodeNotImplemented = TEXT("NOT_IMPLEMENTED");

        TSharedPtr<FJsonObject> MakeNotImplementedResponse(const FString& Command)
        {
                TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
                Result->SetBoolField(TEXT("ok"), false);
                Result->SetBoolField(TEXT("success"), false);
                Result->SetStringField(TEXT("errorCode"), ErrorCodeNotImplemented);
                Result->SetStringField(TEXT("error"), FString::Printf(TEXT("MetaSound command '%s' is not implemented yet."), *Command));
                return Result;
        }
}

TSharedPtr<FJsonObject> FMetaSoundTools::SpawnComponent(const TSharedPtr<FJsonObject>& /*Params*/)
{
        return MakeNotImplementedResponse(TEXT("metasound.spawn_component"));
}

TSharedPtr<FJsonObject> FMetaSoundTools::SetParameters(const TSharedPtr<FJsonObject>& /*Params*/)
{
        return MakeNotImplementedResponse(TEXT("metasound.set_params"));
}

TSharedPtr<FJsonObject> FMetaSoundTools::Play(const TSharedPtr<FJsonObject>& /*Params*/)
{
        return MakeNotImplementedResponse(TEXT("metasound.play"));
}

TSharedPtr<FJsonObject> FMetaSoundTools::Stop(const TSharedPtr<FJsonObject>& /*Params*/)
{
        return MakeNotImplementedResponse(TEXT("metasound.stop"));
}

TSharedPtr<FJsonObject> FMetaSoundTools::ExportInfo(const TSharedPtr<FJsonObject>& /*Params*/)
{
        return MakeNotImplementedResponse(TEXT("metasound.export_info"));
}

TSharedPtr<FJsonObject> FMetaSoundTools::PatchPreset(const TSharedPtr<FJsonObject>& /*Params*/)
{
        return MakeNotImplementedResponse(TEXT("metasound.patch_preset"));
}
