#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"

/**
 * Helper class for JSON parsing and validation in MCP
 */
class UNREALMCP_API FMCPJsonHelpers
{
public:
	/**
	 * Parse JSON string into JsonObject with error handling
	 * @param JsonString The JSON string to parse
	 * @param OutJsonObject The parsed JSON object (if successful)
	 * @param OutErrorMessage Error message if parsing fails
	 * @return true if parsing succeeded
	 */
	static bool ParseJson(const FString& JsonString, TSharedPtr<FJsonObject>& OutJsonObject, FString& OutErrorMessage);

	/**
	 * Validate that a JSON object has required fields
	 * @param JsonObject The JSON object to validate
	 * @param RequiredFields Array of required field names
	 * @param OutErrorMessage Error message if validation fails
	 * @return true if all required fields are present
	 */
	static bool ValidateRequiredFields(const TSharedPtr<FJsonObject>& JsonObject, const TArray<FString>& RequiredFields, FString& OutErrorMessage);

	/**
	 * Safely get a string field from JSON with default value
	 * @param JsonObject The JSON object
	 * @param FieldName The field name
	 * @param DefaultValue Default value if field doesn't exist
	 * @return The field value or default value
	 */
	static FString GetStringField(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, const FString& DefaultValue = TEXT(""));

	/**
	 * Safely get an integer field from JSON with default value
	 * @param JsonObject The JSON object
	 * @param FieldName The field name
	 * @param DefaultValue Default value if field doesn't exist
	 * @return The field value or default value
	 */
	static int32 GetIntField(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, int32 DefaultValue = 0);

	/**
	 * Safely get a float field from JSON with default value
	 * @param JsonObject The JSON object
	 * @param FieldName The field name
	 * @param DefaultValue Default value if field doesn't exist
	 * @return The field value or default value
	 */
	static float GetFloatField(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, float DefaultValue = 0.0f);

	/**
	 * Safely get a boolean field from JSON with default value
	 * @param JsonObject The JSON object
	 * @param FieldName The field name
	 * @param DefaultValue Default value if field doesn't exist
	 * @return The field value or default value
	 */
	static bool GetBoolField(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, bool DefaultValue = false);

	/**
	 * Create a success response JSON object
	 * @param ResultData Optional result data to include
	 * @return JSON object with status "success"
	 */
	static TSharedPtr<FJsonObject> CreateSuccessResponse(const TSharedPtr<FJsonObject>& ResultData = nullptr);

	/**
	 * Create an error response JSON object
	 * @param ErrorMessage The error message
	 * @param ErrorCode Optional error code
	 * @return JSON object with status "error"
	 */
	static TSharedPtr<FJsonObject> CreateErrorResponse(const FString& ErrorMessage, int32 ErrorCode = -1);

	/**
	 * Serialize JSON object to string
	 * @param JsonObject The JSON object to serialize
	 * @return JSON string
	 */
	static FString SerializeJson(const TSharedPtr<FJsonObject>& JsonObject);
};
