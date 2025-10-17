#include "MCPJsonHelpers.h"
#include "UnrealMCPModule.h"

bool FMCPJsonHelpers::ParseJson(const FString& JsonString, TSharedPtr<FJsonObject>& OutJsonObject, FString& OutErrorMessage)
{
	if (JsonString.IsEmpty())
	{
		OutErrorMessage = TEXT("JSON string is empty");
		return false;
	}

	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
	if (!FJsonSerializer::Deserialize(Reader, OutJsonObject) || !OutJsonObject.IsValid())
	{
		OutErrorMessage = FString::Printf(TEXT("Failed to parse JSON: %s"), *JsonString.Left(100));
		return false;
	}

	return true;
}

bool FMCPJsonHelpers::ValidateRequiredFields(const TSharedPtr<FJsonObject>& JsonObject, const TArray<FString>& RequiredFields, FString& OutErrorMessage)
{
	if (!JsonObject.IsValid())
	{
		OutErrorMessage = TEXT("JSON object is null");
		return false;
	}

	for (const FString& Field : RequiredFields)
	{
		if (!JsonObject->HasField(Field))
		{
			OutErrorMessage = FString::Printf(TEXT("Missing required field: %s"), *Field);
			return false;
		}
	}

	return true;
}

FString FMCPJsonHelpers::GetStringField(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, const FString& DefaultValue)
{
	if (JsonObject.IsValid() && JsonObject->HasField(FieldName))
	{
		return JsonObject->GetStringField(FieldName);
	}
	return DefaultValue;
}

int32 FMCPJsonHelpers::GetIntField(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, int32 DefaultValue)
{
	if (JsonObject.IsValid() && JsonObject->HasField(FieldName))
	{
		return JsonObject->GetIntegerField(FieldName);
	}
	return DefaultValue;
}

float FMCPJsonHelpers::GetFloatField(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, float DefaultValue)
{
	if (JsonObject.IsValid() && JsonObject->HasField(FieldName))
	{
		return JsonObject->GetNumberField(FieldName);
	}
	return DefaultValue;
}

bool FMCPJsonHelpers::GetBoolField(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, bool DefaultValue)
{
	if (JsonObject.IsValid() && JsonObject->HasField(FieldName))
	{
		return JsonObject->GetBoolField(FieldName);
	}
	return DefaultValue;
}

TSharedPtr<FJsonObject> FMCPJsonHelpers::CreateSuccessResponse(const TSharedPtr<FJsonObject>& ResultData)
{
	TSharedPtr<FJsonObject> Response = MakeShareable(new FJsonObject());
	Response->SetStringField(TEXT("status"), TEXT("success"));

	if (ResultData.IsValid())
	{
		Response->SetObjectField(TEXT("result"), ResultData);
	}

	return Response;
}

TSharedPtr<FJsonObject> FMCPJsonHelpers::CreateErrorResponse(const FString& ErrorMessage, int32 ErrorCode)
{
	TSharedPtr<FJsonObject> Response = MakeShareable(new FJsonObject());
	Response->SetStringField(TEXT("status"), TEXT("error"));
	Response->SetStringField(TEXT("error"), ErrorMessage);

	if (ErrorCode != -1)
	{
		Response->SetNumberField(TEXT("error_code"), ErrorCode);
	}

	UE_LOG(LogUnrealMCP, Warning, TEXT("Error response: %s"), *ErrorMessage);

	return Response;
}

FString FMCPJsonHelpers::SerializeJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	if (!JsonObject.IsValid())
	{
		return TEXT("{}");
	}

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);

	return OutputString;
}
