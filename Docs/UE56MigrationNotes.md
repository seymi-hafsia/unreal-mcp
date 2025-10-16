# UnrealMCP UE 5.6 Migration Notes

This document summarizes the key API updates applied to the UnrealMCP editor module to restore Rocket build compatibility with Unreal Engine 5.6.

## Asset Registry

* Switched legacy tag access helpers (`GetTagValues`, `GetTagNames`) to the modern `FAssetDataTagMapSharedView` iteration pattern exposed through `FAssetData::TagsAndValues()`.
* Replaced `IAssetRegistry::GetAssetByObjectPath` usages that relied on out parameters with the UE 5.6 version returning an `FAssetData` value.
* Migrated dependency lookups to `FAssetRegistryDependencyOptions`, avoiding the deprecated `FDependencyQuery::Flags` field and matching the hard/soft/searchable/manage flag separation in 5.6.

## LevelSequence / MovieScene

* Updated possessable creation to use the UE 5.6 `UMovieScene::AddPossessable` return value (`FGuid`) and validated bindings via `UMovieSceneSequence::LocateBoundObjects`.
* Replaced boolean-returning expectations on `UMovieScene::RemoveBinding` and `ULevelSequence::BindPossessableObject` with explicit binding checks.
* Introduced the `UE::MovieScene::FResolveParams` overload of `LocateBoundObjects` in all call sites to silence deprecation warnings and support the new resolution workflow.
* Swapped deprecated `UMovieSceneSequenceExtensions::SetDisplayName` with `SetBindingDisplayName` for per-binding labels.

## Data Layers

* Forwarded all runtime state queries and mutations through `UDataLayerManager` (retrieved from the world’s `UDataLayerSubsystem`), replacing removed `SetDataLayerRuntimeStateByName`/`GetDataLayerRuntimeStateByName` subsystem helpers.
* Centralized manager access to avoid null subsystem lookups and to enable batched state verification during streaming waits.

## Include & IWYU Adjustments

* Normalized engine header paths relocated in UE 5.6 (e.g., `AssetImportTask.h`, `WidgetBlueprint.h`, `String/LexToString.h`).
* Removed private container headers (`ScriptArray`, `ScriptSet`) in favor of public `UObject/Script.h` and updated miscellaneous include orderings to maintain IWYU compliance.

