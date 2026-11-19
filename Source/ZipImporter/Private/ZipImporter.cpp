
#include "ZipImporter.h"

#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "ContentBrowserModule.h"
#include "DesktopPlatformModule.h"
#include "EditorReimportHandler.h"
#include "FileHelpers.h"
#include "libzip/zip.h"
#include "Misc/ScopeExit.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "IDesktopPlatform.h"
#include "Input/DragAndDrop.h"
#include "Misc/FileHelper.h"
#include "Misc/MessageDialog.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"


namespace ZipImporter
{
	static const TCHAR* ArchivePathKey = TEXT("ZipImporter.ArchivePath");
	static const TCHAR* ArchiveFileKey = TEXT("ZipImporter.ArchiveFile");
	static const TCHAR* ArchiveChainKey = TEXT("ZipImporter.ArchiveChain");

	static constexpr int32 MaxArchiveDepth = 8;
	static constexpr int32 MaxArchiveFiles = 10000;
	static constexpr int64 MaxSingleFileSize = 512ll * 1024ll * 1024ll;

	static constexpr int64 MaxTotalSize = 4ll * 1024 * 1024 * 1024;

	struct FImportSource
	{
		FString DiskPath;
		FString DestinationPath;
		TArray<FString> ArchiveChain;
	};

	struct FImportContext
	{
		FString ArchivePath;
		FString StagingDirectory;
		TArray<FImportSource> Sources;
		TArray<FString> FailedPaths;
		TSet<FString> VisitedArchives;
		int64 ExtractedBytes = 0;
		int32 ProcessedFiles = 0;
		int32 ImportedObjects = 0;
	};

	bool IsSafeRelativePath(const FString& InputPath, FString& OutRelativePath)
	{
		OutRelativePath = InputPath;
		OutRelativePath.ReplaceInline(TEXT("\\"), TEXT("/"));

		if (OutRelativePath.IsEmpty() ||
			OutRelativePath.StartsWith(TEXT("/")) ||
			OutRelativePath.Contains(TEXT(":")) ||
			OutRelativePath.Contains(TEXT("|")) ||
			OutRelativePath.Contains(TEXT("\n")) ||
			OutRelativePath.Contains(TEXT("\r")))
		{
			return false;
		}

		TArray<FString> Segments;
		OutRelativePath.ParseIntoArray(Segments, TEXT("/"), false);

		if (Segments.IsEmpty())
		{
			return false;
		}

		for (const FString& Segment : Segments)
		{
			if (Segment.IsEmpty() || Segment == TEXT("..") || Segment == TEXT("."))
			{
				return false;
			}
		}

		return true;
	}

	bool GetContentRelativePath(const FString& Directory, FString& OutRelativePath)
	{
		FString ContentDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir());
		FString SelectedDirectory = FPaths::ConvertRelativePathToFull(Directory);

		FPaths::NormalizeDirectoryName(ContentDirectory);
		FPaths::NormalizeDirectoryName(SelectedDirectory);

		if (SelectedDirectory.Equals(ContentDirectory, ESearchCase::IgnoreCase))
		{
			OutRelativePath.Reset();
			return true;
		}

		ContentDirectory /= TEXT("");

		if (!SelectedDirectory.StartsWith(ContentDirectory, ESearchCase::IgnoreCase))
		{
			return false;
		}

		OutRelativePath = SelectedDirectory.RightChop(ContentDirectory.Len());
		OutRelativePath.ReplaceInline(TEXT("\\"), TEXT("/"));

		return true;
	}

	FString MakePackagePath(const FString& RelativePath)
	{
		FString Result = TEXT("/Game");

		if (!RelativePath.IsEmpty())
		{
			Result /= RelativePath;
			Result.ReplaceInline(TEXT("\\"), TEXT("/"));
		}

		return Result;
	}

	bool IsZipFile(const FString& FilePath)
	{
		return FPaths::GetExtension(FilePath).Equals(TEXT("zip"), ESearchCase::IgnoreCase);
	}

	void ShowMessage(const FString& Message)
	{
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Message));
	}

	FString SerializeChain(const TArray<FString>& Chain)
	{
		return FString::Join(Chain, TEXT("\n"));
	}

	bool DeserializeChain(const FString& SerializedChain, TArray<FString>& OutChain)
	{
		OutChain.Reset();

		if (SerializedChain.IsEmpty())
		{
			return false;
		}

		SerializedChain.ParseIntoArray(OutChain, TEXT("\n"), false);

		if (OutChain.IsEmpty())
		{
			return false;
		}

		for (FString& Entry : OutChain)
		{
			FString SafePath;

			if (!IsSafeRelativePath(Entry, SafePath))
			{
				OutChain.Reset();
				return false;
			}

			Entry = SafePath;
		}

		return true;
	}

	void SetReimportMetadata(
		UObject* Object,
		const FString& ArchivePath,
		const TArray<FString>& ArchiveChain)
	{
		if (!IsValid(Object) || ArchiveChain.IsEmpty())
		{
			return;
		}

		UPackage* Package = Object->GetOutermost();

		if (!IsValid(Package))
		{
			return;
		}

		FMetaData& Metadata = Package->GetMetaData();
		Metadata.SetValue(Object, ArchivePathKey, *ArchivePath);
		Metadata.SetValue(Object, ArchiveFileKey, *ArchiveChain.Last());
		Metadata.SetValue(Object, ArchiveChainKey, *SerializeChain(ArchiveChain));

		Object->MarkPackageDirty();
	}

	bool GetReimportMetadata(
		UObject* Object,
		FString& OutArchivePath,
		TArray<FString>& OutArchiveChain)
	{
		if (!IsValid(Object))
		{
			return false;
		}

		UPackage* Package = Object->GetOutermost();

		if (!IsValid(Package))
		{
			return false;
		}

		FMetaData& Metadata = Package->GetMetaData();

		const FString& ArchivePath = Metadata.GetValue(Object, ArchivePathKey);
		const FString& SerializedChain = Metadata.GetValue(Object, ArchiveChainKey);
		const FString& LegacyArchiveFile = Metadata.GetValue(Object, ArchiveFileKey);

		if (ArchivePath.IsEmpty())
		{
			return false;
		}

		OutArchivePath = ArchivePath;

		if (DeserializeChain(SerializedChain, OutArchiveChain))
		{
			return true;
		}

		if (!LegacyArchiveFile.IsEmpty())
		{
			FString SafePath;

			if (IsSafeRelativePath(LegacyArchiveFile, SafePath))
			{
				OutArchiveChain = { SafePath };
				return true;
			}
		}

		return false;
	}

    bool ReadEntry(zip_t* Archive, zip_uint64_t Index, int64 Remaining, TArray<uint8>& Data)
    {
        zip_stat_t Stat;
        if (zip_stat_index(Archive, Index, 0, &Stat) != 0 || !(Stat.valid & ZIP_STAT_SIZE)
            || Stat.size > static_cast<uint64>(FMath::Min(MaxSingleFileSize, Remaining)))
        {
            return false;
        }
        zip_file_t* Entry = zip_fopen_index(Archive, Index, 0);
        if (!Entry)
        {
            return false;
        }
        Data.SetNumUninitialized(static_cast<int32>(Stat.size));
        const bool bRead = Data.IsEmpty() || zip_fread(Entry, Data.GetData(), Stat.size) == static_cast<zip_int64_t>(Stat.size);
        uint8 Extra;
        const bool bAtEnd = bRead && zip_fread(Entry, &Extra, 1) == 0;
        return zip_fclose(Entry) == 0 && bAtEnd;
    }

    bool ReadArchiveEntry(const FString& ArchivePath, const FString& EntryPath, int64 Remaining,
        TArray<uint8>& OutData, FString& OutError)
    {
        FString SafePath;
        if (!IsSafeRelativePath(EntryPath, SafePath))
        {
            OutError = TEXT("Invalid ZIP entry path.");
            return false;
        }
        zip_t* Archive = zip_open(TCHAR_TO_UTF8(*ArchivePath), ZIP_RDONLY, nullptr);
        if (Archive)
        {
            ON_SCOPE_EXIT { zip_discard(Archive); };
            const zip_int64_t Count = zip_get_num_entries(Archive, 0);
            if (Count >= 0 && Count <= MaxArchiveFiles)
            {
                for (zip_int64_t Index = 0; Index < Count; ++Index)
                {
                    const char* Name = zip_get_name(Archive, Index, 0);
                    FString Normalized;
                    if (Name && IsSafeRelativePath(UTF8_TO_TCHAR(Name), Normalized) && Normalized == SafePath)
                    {
                        if (ReadEntry(Archive, Index, Remaining, OutData))
                        {
                            return true;
                        }
                        break;
                    }
                }
            }
        }
        OutError = FString::Printf(TEXT("Cannot read ZIP entry within extraction limits: %s"), *EntryPath);
        return false;
    }

	bool WriteFile(const FString& OutputPath, const TArray<uint8>& Data, FString& OutError)
	{
		const FString OutputDirectory = FPaths::GetPath(OutputPath);

		if (!IFileManager::Get().MakeDirectory(*OutputDirectory, true))
		{
			OutError = FString::Printf(
				TEXT("Could not create directory:\n%s"),
				*OutputDirectory);
			return false;
		}

		if (!FFileHelper::SaveArrayToFile(Data, *OutputPath))
		{
			OutError = FString::Printf(
				TEXT("Could not write file:\n%s"),
				*OutputPath);
			return false;
		}

		return true;
	}

	bool ExtractArchiveChain(
		const FString& ArchivePath,
		const TArray<FString>& ArchiveChain,
		FString& OutExtractedPath,
		FString& OutError)
	{
		if (!FPaths::FileExists(ArchivePath))
		{
			OutError = FString::Printf(
				TEXT("The source ZIP archive no longer exists:\n%s"),
				*ArchivePath);
			return false;
		}

		if (ArchiveChain.IsEmpty() || ArchiveChain.Num() > MaxArchiveDepth + 1)
		{
			OutError = TEXT("The stored archive path chain is invalid or too deep.");
			return false;
		}

		FString CurrentArchivePath = FPaths::ConvertRelativePathToFull(ArchivePath);
		TArray<uint8> FileData;
        const FString NestedDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("ZipImporter/Reimport/Nested"), FGuid::NewGuid().ToString());
        ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*NestedDirectory, false, true); };
		int64 Remaining = MaxTotalSize;

		for (int32 Index = 0; Index < ArchiveChain.Num(); ++Index)
		{
			const FString& EntryPath = ArchiveChain[Index];

			if (!ReadArchiveEntry(CurrentArchivePath, EntryPath, Remaining, FileData, OutError))
			{
				OutError = FString::Printf(
					TEXT("Failed to resolve source chain at step %d of %d.\n\n%s"),
					Index + 1,
					ArchiveChain.Num(),
					*OutError);
				return false;
			}

			Remaining -= FileData.Num();
			if (Index < ArchiveChain.Num() - 1)
			{
				if (!IsZipFile(EntryPath))
				{
					OutError = FString::Printf(
						TEXT("An intermediate source entry is not a ZIP archive:\n%s"),
						*EntryPath);
					return false;
				}

                CurrentArchivePath = FPaths::Combine(NestedDirectory, FString::FromInt(Index) + TEXT(".zip"));

				if (!WriteFile(CurrentArchivePath, FileData, OutError))
				{
					return false;
				}
			}
		}

		const FString TempDirectory = FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("ZipImporter"),
			TEXT("Reimport"),
			FString::Printf(
				TEXT("%08x"),
				GetTypeHash(ArchivePath + TEXT("|") + SerializeChain(ArchiveChain))));

		OutExtractedPath = FPaths::Combine(
			TempDirectory,
			FPaths::GetCleanFilename(ArchiveChain.Last()));

		return WriteFile(OutExtractedPath, FileData, OutError);
	}

	bool SaveDirtyPackages(const TArray<UPackage*>& Packages)
	{
		TArray<UPackage*> UniquePackages;
		TSet<UPackage*> SeenPackages;

		for (UPackage* Package : Packages)
		{
			if (IsValid(Package) && !SeenPackages.Contains(Package))
			{
				SeenPackages.Add(Package);
				UniquePackages.Add(Package);
			}
		}

		return UniquePackages.IsEmpty() ||
			UEditorLoadingAndSavingUtils::SavePackages(UniquePackages, true);
	}

	bool ProcessArchiveRecursive(
		const FString& ArchivePath,
		const FString& DestinationPackagePath,
		const TArray<FString>& ParentChain,
		const int32 Depth,
		FImportContext& Context)
	{
		if (Depth > MaxArchiveDepth)
		{
			Context.FailedPaths.Add(FString::Printf(
				TEXT("Maximum ZIP nesting depth exceeded:\n%s"),
				*ArchivePath));
			return false;
		}

		const FString CanonicalArchivePath = FPaths::ConvertRelativePathToFull(ArchivePath);
		const FString ArchiveKey = CanonicalArchivePath.ToLower();

		if (Context.VisitedArchives.Contains(ArchiveKey))
		{
			Context.FailedPaths.Add(FString::Printf(
				TEXT("Archive recursion detected:\n%s"),
				*ArchivePath));
			return false;
		}

		Context.VisitedArchives.Add(ArchiveKey);

        zip_t* Archive = zip_open(TCHAR_TO_UTF8(*CanonicalArchivePath), ZIP_RDONLY, nullptr);
        if (!Archive)
        {
            Context.FailedPaths.Add(FString::Printf(TEXT("Could not open archive: %s"), *CanonicalArchivePath));
            return false;
        }
        ON_SCOPE_EXIT { zip_discard(Archive); };
        const zip_int64_t Count = zip_get_num_entries(Archive, 0);
        if (Count < 0 || Count > MaxArchiveFiles - Context.ProcessedFiles)
        {
            Context.FailedPaths.Add(TEXT("Archive entry count exceeds the import limit."));
            return false;
        }
        Context.ProcessedFiles += static_cast<int32>(Count);
        for (zip_int64_t Index = 0; Index < Count; ++Index)
        {
            const char* Name = zip_get_name(Archive, Index, 0);
            if (!Name)
            {
                Context.FailedPaths.Add(TEXT("Could not read ZIP entry name."));
                return false;
            }
            const FString ArchiveFile(UTF8_TO_TCHAR(Name));
			if (ArchiveFile.EndsWith(TEXT("/")) || ArchiveFile.EndsWith(TEXT("\\")))
			{
				continue;
			}

			FString RelativeFilePath;

			if (!IsSafeRelativePath(ArchiveFile, RelativeFilePath))
			{
				Context.FailedPaths.Add(FString::Printf(
					TEXT("Unsafe archive path:\n%s"),
					*ArchiveFile));
				continue;
			}

			TArray<uint8> FileData;

            if (!ReadEntry(Archive, Index, MaxTotalSize - Context.ExtractedBytes, FileData))
            {
                Context.FailedPaths.Add(FString::Printf(TEXT("ZIP entry is invalid or exceeds extraction limits: %s"), *RelativeFilePath));
                return false;
            }
            Context.ExtractedBytes += FileData.Num();

			TArray<FString> CurrentChain = ParentChain;
			CurrentChain.Add(RelativeFilePath);

			if (IsZipFile(RelativeFilePath))
			{
				if (Depth >= MaxArchiveDepth)
				{
					Context.FailedPaths.Add(FString::Printf(
						TEXT("Maximum ZIP nesting depth exceeded:\n%s"),
						*SerializeChain(CurrentChain)));
					return false;
				}

				const FString NestedArchivePath = FPaths::Combine(
					Context.StagingDirectory,
					TEXT("Nested"),
					FString::Printf(
						TEXT("%08x"),
						GetTypeHash(SerializeChain(CurrentChain))),
					FPaths::GetCleanFilename(RelativeFilePath));

				FString WriteError;

				if (!WriteFile(NestedArchivePath, FileData, WriteError))
				{
					Context.FailedPaths.Add(WriteError);
					continue;
				}

				FString NestedDestination = DestinationPackagePath;
				const FString RelativeDirectory = FPaths::GetPath(RelativeFilePath);

				if (!RelativeDirectory.IsEmpty())
				{
					NestedDestination /= RelativeDirectory;
				}

				NestedDestination /= FPaths::GetBaseFilename(RelativeFilePath);
				NestedDestination.ReplaceInline(TEXT("\\"), TEXT("/"));

				FileData.Empty();
				const bool bProcessed = ProcessArchiveRecursive(
					NestedArchivePath,
					NestedDestination,
					CurrentChain,
					Depth + 1,
					Context);
				IFileManager::Get().Delete(*NestedArchivePath);
				if (!bProcessed)
				{
					return false;
				}

				continue;
			}

			const FString SerializedChain = SerializeChain(CurrentChain);

			const FString OutputPath = FPaths::Combine(
				Context.StagingDirectory,
				TEXT("Extracted"),
				FString::Printf(TEXT("%08x"), GetTypeHash(SerializedChain)),
				FPaths::GetCleanFilename(RelativeFilePath));

			FString WriteError;

			if (!WriteFile(OutputPath, FileData, WriteError))
			{
				Context.FailedPaths.Add(WriteError);
				continue;
			}

			FString TaskDestination = DestinationPackagePath;
			const FString RelativeDirectory = FPaths::GetPath(RelativeFilePath);

			if (!RelativeDirectory.IsEmpty())
			{
				TaskDestination /= RelativeDirectory;
				TaskDestination.ReplaceInline(TEXT("\\"), TEXT("/"));
			}

			FImportSource& Source = Context.Sources.AddDefaulted_GetRef();
			Source.DiskPath = OutputPath;
			Source.DestinationPath = TaskDestination;
			Source.ArchiveChain = MoveTemp(CurrentChain);
		}

		Context.VisitedArchives.Remove(ArchiveKey);
		return true;
	}
}

class FZipArchiveReimportHandler final : public FReimportHandler
{
public:
	bool CanReimport(UObject* Object, TArray<FString>& OutFilenames) override
	{
		FString ArchivePath;
		TArray<FString> ArchiveChain;

		if (!ZipImporter::GetReimportMetadata(Object, ArchivePath, ArchiveChain))
		{
			return false;
		}

		OutFilenames.Add(ArchivePath);
		return true;
	}

	void SetReimportPaths(UObject* Object, const TArray<FString>& NewReimportPaths) override
	{
		if (!IsValid(Object) || NewReimportPaths.IsEmpty())
		{
			return;
		}

		FString ArchivePath;
		TArray<FString> ArchiveChain;

		if (!ZipImporter::GetReimportMetadata(Object, ArchivePath, ArchiveChain))
		{
			return;
		}

		const FString NewArchivePath =
			FPaths::ConvertRelativePathToFull(NewReimportPaths[0]);

		ZipImporter::SetReimportMetadata(Object, NewArchivePath, ArchiveChain);
	}

	int32 GetPriority() const override
	{
		return 1000;
	}

	EReimportResult::Type Reimport(UObject* Object) override
	{
		return Reimport(Object, 0);
	}

	EReimportResult::Type Reimport(UObject* Object, int32 SourceFileIndex) override
	{
		FString ArchivePath;
		TArray<FString> ArchiveChain;

		if (!ZipImporter::GetReimportMetadata(Object, ArchivePath, ArchiveChain))
		{
			return EReimportResult::Failed;
		}

		FString ExtractedPath;
		FString Error;

		if (!ZipImporter::ExtractArchiveChain(
			ArchivePath,
			ArchiveChain,
			ExtractedPath,
			Error))
		{
			ZipImporter::ShowMessage(Error);
			return EReimportResult::Failed;
		}

		UPackage* ExistingPackage = Object->GetOutermost();

		if (!IsValid(ExistingPackage))
		{
			return EReimportResult::Failed;
		}

		UAssetImportTask* Task = NewObject<UAssetImportTask>();
		Task->Filename = ExtractedPath;
		Task->DestinationPath = FPackageName::GetLongPackagePath(ExistingPackage->GetName());
		Task->DestinationName = Object->GetName();
		Task->bAutomated = !IsForceShowDialog();
		Task->bReplaceExisting = true;
		Task->bSave = false;

		TArray<UAssetImportTask*> Tasks;
		Tasks.Add(Task);

		FAssetToolsModule& AssetToolsModule =
			FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));

		AssetToolsModule.Get().ImportAssetTasks(Tasks);

		const TArray<UObject*>& ImportedObjects = Task->GetObjects();

		if (ImportedObjects.IsEmpty())
		{
			ZipImporter::ShowMessage(
				FString::Printf(
					TEXT("Reimport failed.\n\nAsset:\n%s\n\nSource chain:\n%s"),
					*Object->GetPathName(),
					*ZipImporter::SerializeChain(ArchiveChain)));

			return EReimportResult::Failed;
		}

		TArray<UPackage*> PackagesToSave;
		bool bFoundImportedObject = false;

		for (UObject* ImportedObject : ImportedObjects)
		{
			if (!IsValid(ImportedObject))
			{
				continue;
			}

			ZipImporter::SetReimportMetadata(
				ImportedObject,
				ArchivePath,
				ArchiveChain);

			PackagesToSave.Add(ImportedObject->GetOutermost());
			bFoundImportedObject = true;
		}

		if (!bFoundImportedObject)
		{
			return EReimportResult::Failed;
		}

		if (!ZipImporter::SaveDirtyPackages(PackagesToSave))
		{
			ZipImporter::ShowMessage(
				TEXT("The source file was imported, but saving the updated asset packages failed."));
			return EReimportResult::Failed;
		}

		UE_LOG(
			LogTemp,
			Log,
			TEXT("ZIP reimport succeeded: %s"),
			*Object->GetPathName());

		return EReimportResult::Succeeded;
	}
};

void FZipImporterModule::StartupModule()
{
	ReimportHandler = new FZipArchiveReimportHandler();

	if (FModuleManager::Get().IsModuleLoaded(TEXT("ContentBrowser")))
	{
		FContentBrowserModule& ContentBrowserModule =
			FModuleManager::GetModuleChecked<FContentBrowserModule>(
				TEXT("ContentBrowser"));

		ContentBrowserModule.GetAssetViewDragAndDropExtenders().Add(
			FAssetViewDragAndDropExtender(
				FAssetViewDragAndDropExtender::FOnDropDelegate::CreateRaw(
					this,
					&FZipImporterModule::OnAssetViewDrop),
				FAssetViewDragAndDropExtender::FOnDragOverDelegate::CreateRaw(
					this,
					&FZipImporterModule::OnAssetViewDragOver)));
	}
}

void FZipImporterModule::ShutdownModule()
{
    if (FContentBrowserModule* Browser = FModuleManager::GetModulePtr<FContentBrowserModule>(TEXT("ContentBrowser")))
    {
        Browser->GetAssetViewDragAndDropExtenders().RemoveAll([this](const FAssetViewDragAndDropExtender& Extender)
        {
            return Extender.OnDropDelegate.IsBoundToObject(this);
        });
    }
	delete ReimportHandler;
	ReimportHandler = nullptr;
}

bool FZipImporterModule::CanHandleZipDrop(
	const FAssetViewDragAndDropExtender::FPayload& Payload) const
{
	if (!Payload.DragDropOp.IsValid() ||
		!Payload.DragDropOp->IsOfType<FExternalDragOperation>())
	{
		return false;
	}

	const TSharedPtr<FExternalDragOperation> ExternalOperation =
		StaticCastSharedPtr<FExternalDragOperation>(Payload.DragDropOp);

	if (!ExternalOperation.IsValid() || !ExternalOperation->HasFiles())
	{
		return false;
	}

	if (Payload.PackagePaths.IsEmpty())
	{
		return false;
	}

	const FString DestinationPath = Payload.PackagePaths[0].ToString();

	if (DestinationPath != TEXT("/Game") && !DestinationPath.StartsWith(TEXT("/Game/")))
	{
		return false;
	}

	for (const FString& FilePath : ExternalOperation->GetFiles())
	{
		if (!ZipImporter::IsZipFile(FilePath))
		{
			return false;
		}
	}

	return true;
}

bool FZipImporterModule::OnAssetViewDragOver(
	const FAssetViewDragAndDropExtender::FPayload& Payload)
{
	return CanHandleZipDrop(Payload);
}

bool FZipImporterModule::OnAssetViewDrop(
	const FAssetViewDragAndDropExtender::FPayload& Payload)
{
	if (!CanHandleZipDrop(Payload))
	{
		return false;
	}

	const TSharedPtr<FExternalDragOperation> ExternalOperation =
		StaticCastSharedPtr<FExternalDragOperation>(Payload.DragDropOp);

	const FString DestinationPackagePath = Payload.PackagePaths[0].ToString();

	bool bHandled = false;

	for (const FString& FilePath : ExternalOperation->GetFiles())
	{
		if (!ZipImporter::IsZipFile(FilePath))
		{
			continue;
		}

		ImportArchiveToPackagePath(FilePath, DestinationPackagePath);
		bHandled = true;
	}

	return bHandled;
}


UObject* FZipImporterModule::ImportArchiveToPackagePath(
    const FString& ArchivePath,
    const FString& DestinationPackagePath)
{
    if (!ZipImporter::IsZipFile(ArchivePath))
    {
        return nullptr;
    }

    const FString ArchiveAbsolutePath =
        FPaths::ConvertRelativePathToFull(ArchivePath);

    const FString ArchiveName =
        FPaths::GetBaseFilename(ArchiveAbsolutePath);

    const FString StagingDirectory = FPaths::Combine(
        FPaths::ProjectSavedDir(),
        TEXT("ZipImporter"),
        FString::Printf(
            TEXT("%s_%08x"),
            *ArchiveName,
            GetTypeHash(ArchiveAbsolutePath)));

    if (!FPaths::FileExists(ArchiveAbsolutePath))
    {
        ZipImporter::ShowMessage(
            FString::Printf(
                TEXT("Archive does not exist:\n%s"),
                *ArchiveAbsolutePath));

        return nullptr;
    }

    ZipImporter::FImportContext Context;
    Context.ArchivePath = ArchiveAbsolutePath;
    Context.StagingDirectory = FPaths::Combine(StagingDirectory, TEXT("Preparing"), FGuid::NewGuid().ToString());
    ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Context.StagingDirectory, false, true); };

    if (!IFileManager::Get().MakeDirectory(*StagingDirectory, true))
    {
        ZipImporter::ShowMessage(
            FString::Printf(
                TEXT("Could not create staging directory:\n%s"),
                *StagingDirectory));

        return nullptr;
    }

    const bool bPrepared = ZipImporter::ProcessArchiveRecursive(
        ArchiveAbsolutePath,
        DestinationPackagePath,
        TArray<FString>(),
        0,
        Context);

    for (const FString& Problem : Context.FailedPaths)
    {
        UE_LOG(LogTemp, Warning, TEXT("%s"), *Problem);
    }
    if (!bPrepared || Context.Sources.IsEmpty())
    {
        FString Message = TEXT("No files were available for import.");

        if (!Context.FailedPaths.IsEmpty())
        {
            Message += TEXT("\n\n");
            Message += Context.FailedPaths[0] + TEXT("\nSee Output Log for details.");
        }

        ZipImporter::ShowMessage(Message);
        return nullptr;
    }

    TArray<FString> ExistingPackages;
    for (const ZipImporter::FImportSource& Source : Context.Sources)
    {
        const FString AssetName = ObjectTools::SanitizeObjectName(FPaths::GetBaseFilename(Source.DiskPath));
        const FString PackageName = Source.DestinationPath / AssetName;
        if (FPackageName::IsValidLongPackageName(PackageName)
            && (FindPackage(nullptr, *PackageName) || FPackageName::DoesPackageExist(PackageName)))
        {
            ExistingPackages.AddUnique(PackageName);
        }
    }
    if (!ExistingPackages.IsEmpty())
    {
        ExistingPackages.Sort();
        for (const FString& Package : ExistingPackages)
        {
            UE_LOG(LogTemp, Display, TEXT("ZIP import destination already exists: %s"), *Package);
        }
        const int32 Count = ExistingPackages.Num();
        ExistingPackages.SetNum(FMath::Min(Count, 12));
        const FText Prompt = FText::Format(
            NSLOCTEXT("ZipImporter", "ExistingDestinations", "{0} existing destination package(s) found. First matches:\n{1}\n\nThe complete list is in the Output Log. Continue with import?"),
            FText::AsNumber(Count), FText::FromString(FString::Join(ExistingPackages, TEXT("\n"))));
        if (FMessageDialog::Open(EAppMsgType::YesNo, Prompt) != EAppReturnType::Yes)
        {
            return nullptr;
        }
    }

    TArray<UAssetImportTask*> ImportTasks;
    TMap<UAssetImportTask*, TArray<FString>> TaskArchiveChains;

    ImportTasks.Reserve(Context.Sources.Num());

    for (const ZipImporter::FImportSource& Source : Context.Sources)
    {
        const FString RelativeSource = Source.DiskPath.Mid(Context.StagingDirectory.Len() + 1);
        const FString FactorySource = FPaths::Combine(StagingDirectory, RelativeSource);
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(FactorySource), true);
        if (!IFileManager::Get().Move(*FactorySource, *Source.DiskPath, true, false))
        {
            ZipImporter::ShowMessage(FString::Printf(TEXT("Could not prepare import source: %s"), *FactorySource));
            return nullptr;
        }
        UAssetImportTask* Task = NewObject<UAssetImportTask>();
        Task->Filename = FactorySource;
        Task->DestinationPath = Source.DestinationPath;
        Task->bAutomated = false;
        Task->bReplaceExisting = false;
        Task->bSave = false;

        ImportTasks.Add(Task);
        TaskArchiveChains.Add(Task, Source.ArchiveChain);
    }

    FAssetToolsModule& AssetToolsModule =
        FModuleManager::LoadModuleChecked<FAssetToolsModule>(
            TEXT("AssetTools"));

    AssetToolsModule.Get().ImportAssetTasks(ImportTasks);

    TArray<UPackage*> PackagesToSave;
    int32 FailedImportTasks = 0;
    UObject* PrimaryImportedObject = nullptr;

    for (UAssetImportTask* Task : ImportTasks)
    {
        if (!Task)
        {
            ++FailedImportTasks;
            continue;
        }

        const TArray<FString>* ArchiveChain =
            TaskArchiveChains.Find(Task);

        if (!ArchiveChain)
        {
            ++FailedImportTasks;
            continue;
        }

        const TArray<UObject*>& ImportedObjects = Task->GetObjects();

        if (ImportedObjects.IsEmpty())
        {
            UE_LOG(LogTemp, Warning, TEXT("ZIP asset import failed: %s -> %s"), *ZipImporter::SerializeChain(*ArchiveChain), *Task->DestinationPath);
            ++FailedImportTasks;
            continue;
        }

        for (UObject* ImportedObject : ImportedObjects)
        {
            if (!IsValid(ImportedObject))
            {
                continue;
            }

            if (!PrimaryImportedObject)
            {
                PrimaryImportedObject = ImportedObject;
            }

            ZipImporter::SetReimportMetadata(
                ImportedObject,
                ArchiveAbsolutePath,
                *ArchiveChain);

            PackagesToSave.Add(ImportedObject->GetOutermost());
            ++Context.ImportedObjects;
        }
    }

    const bool bSaved =
        ZipImporter::SaveDirtyPackages(PackagesToSave);

    UE_LOG(
        LogTemp,
        Log,
        TEXT("ZIP import finished. Archive: %s. Files: %d. Import tasks: %d. Imported objects: %d. Failed tasks: %d. Extraction errors: %d."),
        *ArchiveName,
        Context.Sources.Num(),
        ImportTasks.Num(),
        Context.ImportedObjects,
        FailedImportTasks,
        Context.FailedPaths.Num());

    if (!bSaved ||
        FailedImportTasks > 0 ||
        !Context.FailedPaths.IsEmpty())
    {
        FString ResultMessage = FString::Printf(
            TEXT("ZIP import completed with problems.\n\nArchive: %s\nFiles prepared: %d\nImport tasks: %d\nImported objects: %d\nFailed tasks: %d\nExtraction errors: %d"),
            *ArchiveName,
            Context.Sources.Num(),
            ImportTasks.Num(),
            Context.ImportedObjects,
            FailedImportTasks,
            Context.FailedPaths.Num());

        if (!bSaved)
        {
            ResultMessage +=
                TEXT("\n\nSome imported asset packages could not be saved.");
        }

        const int32 MaxErrorsToShow =
            FMath::Min(Context.FailedPaths.Num(), 20);

        for (int32 Index = 0; Index < MaxErrorsToShow; ++Index)
        {
            ResultMessage += TEXT("\n\n");
            ResultMessage += Context.FailedPaths[Index];
        }

        if (Context.FailedPaths.Num() > MaxErrorsToShow)
        {
            ResultMessage += FString::Printf(
                TEXT("\n\nAnd %d more errors. See the Output Log."),
                Context.FailedPaths.Num() - MaxErrorsToShow);
        }

        ZipImporter::ShowMessage(ResultMessage);
    }

    return PrimaryImportedObject;
}

IMPLEMENT_MODULE(FZipImporterModule, ZipImporter)
