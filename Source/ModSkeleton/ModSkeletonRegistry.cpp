// Copyright 2017 Smogworks
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http ://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "ModSkeletonRegistry.h"
#include "ModSkeleton.h"

#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"

#include "Runtime/PakFile/Public/IPlatformFilePak.h"

#include "AssetRegistryModule.h"

#include "ModSkeletonPluginInterface.h"

UModSkeletonRegistry::UModSkeletonRegistry()
{
	FModSkeletonHookDescription InitHook;
	InitHook.AlwaysInvoke = true;
	InitHook.HookName = "ModSkeletonInit";
	InitHook.HookDescription = "ModSkeleton Bootstrap Entrypoint. This will be invoked on every Mod as they are loaded.";

	InstallHook(InitHook);
}

void UModSkeletonRegistry::ScanForModPlugins()
{
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	// Uncomment this and the "PakPlatform->IterateDirectoryRecursively" below to dump out pak contents on load
	struct StructDumpVisitor : public IPlatformFile::FDirectoryVisitor
	{
		virtual bool Visit(const TCHAR* FilenameOrDirectory, bool bIsDirectory)
		{
			if (bIsDirectory)
			{
				UE_LOG(ModSkeletonLog, Log, TEXT(" - DumpVisitor Directory: %s"), FilenameOrDirectory);
			}
			else
			{
				UE_LOG(ModSkeletonLog, Log, TEXT(" - DumpVisitor File: %s"), FilenameOrDirectory);
			}
			return true;
		}
	};
	StructDumpVisitor DumpVisitor;

	IPlatformFile& InnerPlatform = FPlatformFileManager::Get().GetPlatformFile();
	FPakPlatformFile* PakPlatform = new FPakPlatformFile();
	PakPlatform->Initialize(&InnerPlatform, TEXT(""));
	FPlatformFileManager::Get().SetPlatformFile(*PakPlatform);
	//FPlatformFileManager::Get().InitializeNewAsyncIO();

	IFileManager& FileManager = IFileManager::Get();
	FString ModPath = FPaths::ProjectDir() + TEXT("Mods");
	FPaths::NormalizeDirectoryName(ModPath);
	FString BinSearch = ModPath + "/*.pak";

	// First, search for all Pak files which includes AssetRegistry .bin file in it
	TArray<FString> Files;
	FileManager.FindFiles(Files, *BinSearch, true, false);
	UE_LOG(ModSkeletonLog, Log, TEXT("Searching for Pak files: %s"), *BinSearch);

	for (int32 i = 0; i < Files.Num(); ++i)
	{
		FString PakFilename = ModPath + TEXT("/") + Files[i];
		FPaths::MakeStandardFilename(PakFilename);
		UE_LOG(ModSkeletonLog, Log, TEXT(" - Pak file: %s"), *PakFilename);

		FString PathPart;
		FString FilenamePart;
		FString ExtensionPart;
		FPaths::Split(PakFilename, PathPart, FilenamePart, ExtensionPart);
		
		//FString PakFilename = PathPart + "/" + FilenamePart + ".pak";
		//FPaths::MakeStandardFilename(PakFilename);

		if (LoadedPaks.Contains(PakFilename))
		{
			continue;
		}
		
		UE_LOG(ModSkeletonLog, Log, TEXT("Attempting PakLoad: %s"), *PakFilename);

		// Mount the .pak content file
		FPakFile PakFile(&InnerPlatform, *PakFilename, false);
		if (!PakFile.IsValid())
		{
			UE_LOG(ModSkeletonLog, Error, TEXT("Invalid pak file: %s"), *PakFilename);
			GEngine->AddOnScreenDebugMessage(-1, 10.f, FColor::Red, FString::Printf(TEXT("Invalid pak file: %s"), *PakFilename));
			continue;
		}

		FString MountPoint = PakFile.GetMountPoint();
		FPaths::MakeStandardFilename(MountPoint);
		UE_LOG(ModSkeletonLog, Log, TEXT(" - Plugin contents mountpoint is: %s"), *MountPoint);
		UE_LOG(ModSkeletonLog, Log, TEXT("If the mountpoint doesn't include the plugin name, the MOD Pak contains Engine contents. Remove the reference to Engine contents and rebuild Pak."));

		PakFile.SetMountPoint(*MountPoint);
		if (!PakPlatform->Mount(*PakFilename, 0, *MountPoint))
		{
			UE_LOG(ModSkeletonLog, Error, TEXT("Failed to mount pak file: %s"), *PakFilename);
			GEngine->AddOnScreenDebugMessage(-1, 10.f, FColor::Red, FString::Printf(TEXT("Failed to mount pak file: %s"), *PakFilename));
			continue;
		}

		LoadedPaks.Add(PakFilename, true);

		// Mount Plugin contents
		FString PluginContentsDirectory = FPaths::Combine(MountPoint, TEXT("Content/"));
		FString PluginMountPoint = TEXT("/") + FilenamePart + TEXT("/");
		FPaths::MakeStandardFilename(PluginContentsDirectory);
		UE_LOG(ModSkeletonLog, Log, TEXT(" - Plugin contents mounting at: %s"), *PluginContentsDirectory);
		FPackageName::RegisterMountPoint(PluginMountPoint, PluginContentsDirectory);

		// Load the asset registry .bin file into the in-memory AssetRegistry
		FString BinFilename = MountPoint + TEXT("AssetRegistry.bin");
		FArrayReader SerializedAssetData;
		if (FFileHelper::LoadFileToArray(SerializedAssetData, *BinFilename))
		{
			AssetRegistry.Serialize(SerializedAssetData);
			UE_LOG(ModSkeletonLog, Log, TEXT(" - AssetRegistry Loaded (%d bytes): %s"), SerializedAssetData.Num(), *BinFilename);
			//GEngine->AddOnScreenDebugMessage(-1, 10.f, FColor::Emerald, FString::Printf(TEXT(" - AssetRegistry Loaded (%d bytes): %s"), SerializedAssetData.Num(), *BinFilename));
		}

		//UE_LOG(ModSkeletonLog, Log, TEXT("Dump %s"), *PluginContentsDirectory);
		//PakPlatform->IterateDirectoryRecursively(*PluginContentsDirectory, DumpVisitor);
	}

	// now that the content assets have been added, and the asset registry has been updated
	// we need to search the in-memory AssetRegistry to find any MOD_SKELETON init interfaces

	TArray<FAssetData> AssetData;
	AssetRegistry.GetAllAssets(AssetData);

	UE_LOG(ModSkeletonLog, Log, TEXT("Searching for ModSkeleton Mod Assets:"));

	for (int32 i = 0; i < AssetData.Num(); ++i)
	{
		FString name = AssetData[i].AssetName.ToString();
		if (name.StartsWith("MOD_SKELETON", ESearchCase::CaseSensitive))
		{
			UE_LOG(ModSkeletonLog, Log, TEXT(" - Asset: %s %s %s %s"), *name, *AssetData[i].PackagePath.ToString(), *AssetData[i].ObjectPath.ToString(), *AssetData[i].AssetClass.ToString());

			if (LoadedPlugins.Contains(AssetData[i].ObjectPath))
			{
				continue;
			}

			// TODO - this is loading Blueprint Interfaces
			// make this work with C++ interfaces as well!

			UClass* AssetClass = LoadObject<UClass>(nullptr, *(TEXT("Class'") + AssetData[i].ObjectPath.ToString() + TEXT("_C'")));
			if (AssetClass != nullptr)
			{
				UObject *RealObj = NewObject<UObject>(this, AssetClass);
				if (RealObj->GetClass()->ImplementsInterface(UModSkeletonPluginInterface::StaticClass()))
				{
					// Invoke the ModSkeletonInit hook - this is invoked exactly once for every mod right at load.

					TArray< UBPVariant* > HookIO;
					IModSkeletonPluginInterface::Execute_ModSkeletonHook(RealObj, TEXT("ModSkeletonInit"), HookIO);

					LoadedPlugins.Add(AssetData[i].ObjectPath, RealObj);
				}
			}
		}
	}
}

void UModSkeletonRegistry::ListModPlugins(TArray< UObject* >& OutPluginList)
{
	LoadedPlugins.GenerateValueArray(OutPluginList);
}

bool UModSkeletonRegistry::InstallHook(FModSkeletonHookDescription HookDescription)
{
	if (RegisteredHooks.Contains(HookDescription.HookName))
	{
		return false;
	}
	RegisteredHooks.Add(HookDescription.HookName, HookDescription);
	return true;
}

TArray< FModSkeletonHookDescription > UModSkeletonRegistry::ListHooks()
{
	TArray< FModSkeletonHookDescription > OutArray;
	RegisteredHooks.GenerateValueArray(OutArray);
	return OutArray;
}

FModSkeletonHookDescription UModSkeletonRegistry::GetHookDescription(FString HookName)
{
	if (RegisteredHooks.Contains(HookName))
	{
		return RegisteredHooks[HookName];
	}
	return FModSkeletonHookDescription();
}

void UModSkeletonRegistry::ConnectHook(FString HookName, int32 Priority, UObject *ModSkeletonPluginInterface)
{
	if (!ModSkeletonPluginInterface->GetClass()->ImplementsInterface(UModSkeletonPluginInterface::StaticClass())) {
		return;
	}

	FModSkeletonConnectHook NewHook;
	NewHook.HookName = HookName;
	NewHook.Priority = Priority;
	NewHook.ModSkeletonPluginInterface = ModSkeletonPluginInterface;

	ConnectedHooks.HeapPush(NewHook, FModSkeletonConnectHookPredicate());
}

TArray< UBPVariant* > UModSkeletonRegistry::InvokeHook(FString HookName, const TArray< UBPVariant * >& HookIO)
{
	if (!RegisteredHooks.Contains(HookName))
	{
		UE_LOG(ModSkeletonLog, Warning, TEXT("Ignoring Unregistered HookName: %s"), *HookName);
		return HookIO;
	}
	UE_LOG(ModSkeletonLog, Log, TEXT("Invoke HookName: %s"), *HookName);

	// We want to pass the RESULTS from the previous invocation in as the PARAMETERS to the next
	TArray< UBPVariant* > CurHookIO = HookIO;

	FModSkeletonHookDescription HookDescription = RegisteredHooks[HookName];
	if (HookDescription.AlwaysInvoke) {
		for (auto PlugRef : LoadedPlugins)
		{
			CurHookIO = IModSkeletonPluginInterface::Execute_ModSkeletonHook(PlugRef.Value, HookName, CurHookIO);
		}
	}
	else
	{
		TArray<FModSkeletonConnectHook> ConnectedHooksClone = ConnectedHooks;
		FModSkeletonConnectHook Next;
		while (ConnectedHooksClone.Num() > 0)
		{
			ConnectedHooksClone.HeapPop(Next, FModSkeletonConnectHookPredicate());
			if (Next.HookName != HookName) continue;
			CurHookIO = IModSkeletonPluginInterface::Execute_ModSkeletonHook(Next.ModSkeletonPluginInterface, HookName, CurHookIO);
		}
	}
	return CurHookIO;
}
