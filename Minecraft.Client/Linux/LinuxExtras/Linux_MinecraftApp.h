#pragma once

// Linux equivalent of Windows64/Windows64_App.h - same class name
// (CConsoleMinecraftApp) as every other platform's *_App.h, since nothing
// outside platform-specific headers references it by name (only the shared
// CMinecraftApp base / the `app` global are used from portable code).

class CConsoleMinecraftApp : public CMinecraftApp
{
public:
	CConsoleMinecraftApp();

	virtual void SetRichPresenceContext(int iPad, int contextId);

	virtual void StoreLaunchData();
	virtual void ExitGame();
	virtual void FatalLoadError();

	virtual void CaptureSaveThumbnail();
	virtual void GetSaveThumbnail(PBYTE*,DWORD*);
	virtual void ReleaseSaveThumbnail();
	virtual void GetScreenshot(int iPad,PBYTE *pbData,DWORD *pdwSize);

	virtual int LoadLocalTMSFile(WCHAR *wchTMSFile);
	virtual int LoadLocalTMSFile(WCHAR *wchTMSFile, eFileExtensionType eExt);

	virtual void FreeLocalTMSFiles(eTMSFileType eType);
	virtual int GetLocalTMSFileIndex(WCHAR *wchTMSFile,bool bFilenameIncludesExtension,eFileExtensionType eEXT=eFileExtensionType_PNG);

	// BANNED LEVEL LIST
	virtual void ReadBannedList(int iPad, eTMSAction action=(eTMSAction)0, bool bCallback=false) {}

	C4JStringTable *GetStringTable()																									{ return NULL;}

	// Direct non-UI single-player bootstrap (Phase 7b uses this) - mirrors
	// Windows64_App.cpp's own dev-shortcut of the same name exactly.
	virtual void TemporaryCreateGameStart();
};

extern CConsoleMinecraftApp app;
