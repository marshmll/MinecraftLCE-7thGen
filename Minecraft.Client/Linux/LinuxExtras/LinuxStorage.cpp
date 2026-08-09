// LinuxStorage.cpp - C4JStorage implementation for local single-player saves
// on Linux, against Minecraft.Client/Linux/4JLibs/inc/4J_Storage.h (Phase 2's
// Linux-adapted copy of the header, extended with the split-save Subfile API).
//
// Scope, per the plan: only the local save/load subset gets a real
// implementation (Init/SaveSaveData/LoadSaveData/GetSavesInfo/DeleteSaveData/
// DoesSaveExist/GetSaveData+AllocateSaveData+SetSaveImages/GetMountedPath,
// plus the Subfile split-save API that ConsoleSaveFileSplit.cpp calls
// unconditionally). Everything DLC/marketplace/TMS/message-box related is
// Xbox Live/PSN-coupled with no Linux desktop equivalent and no online
// service is being stood up for this port - those are stubbed below to
// return "unavailable"/idle statuses, each documented at its definition
// rather than silently no-op'd.

#include "LinuxTypes.h"
#include "LinuxStubs.h"
// extraX64.h (a Minecraft.World public include dir) is where the desktop
// platforms' Xbox-Live-compat stub types used by 4J_Storage.h's DLC/
// marketplace surface (XCONTENT_DATA, XMARKETPLACE_CONTENTOFFER_INFO,
// XUSER_INDEX_ANY, ...) actually live - same header Minecraft.World's
// stdafx.h pulls in for every platform.
#include "extraX64.h"
#include "../4JLibs/inc/4J_Storage.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <ctime>

namespace
{
	// WCHAR (wchar_t, 4 bytes on Linux/UCS-4) <-> UTF-8, done by hand rather
	// than wcstombs/mbstowcs to avoid depending on a UTF-8 locale being set.
	std::string WStringToUtf8(const wchar_t *ws)
	{
		std::string out;
		if (!ws) return out;
		for (; *ws; ++ws)
		{
			unsigned int cp = (unsigned int)*ws;
			if (cp < 0x80) out += (char)cp;
			else if (cp < 0x800)
			{
				out += (char)(0xC0 | (cp >> 6));
				out += (char)(0x80 | (cp & 0x3F));
			}
			else if (cp < 0x10000)
			{
				out += (char)(0xE0 | (cp >> 12));
				out += (char)(0x80 | ((cp >> 6) & 0x3F));
				out += (char)(0x80 | (cp & 0x3F));
			}
			else
			{
				out += (char)(0xF0 | (cp >> 18));
				out += (char)(0x80 | ((cp >> 12) & 0x3F));
				out += (char)(0x80 | ((cp >> 6) & 0x3F));
				out += (char)(0x80 | (cp & 0x3F));
			}
		}
		return out;
	}

	std::string GetSaveBaseDir()
	{
		const char *xdgData = getenv("XDG_DATA_HOME");
		std::string base;
		if (xdgData && xdgData[0])
			base = std::string(xdgData) + "/minecraft-lce/saves";
		else
		{
			const char *home = getenv("HOME");
			base = std::string(home ? home : ".") + "/.local/share/minecraft-lce/saves";
		}

		// mkdir -p (base has at most 3 missing path components in practice).
		std::string partial;
		for (size_t i = 0; i < base.size(); i++)
		{
			partial += base[i];
			if (base[i] == '/' && !partial.empty())
				mkdir(partial.c_str(), 0755);
		}
		mkdir(base.c_str(), 0755);
		return base;
	}

	struct SubfileEntry
	{
		unsigned int id;
		std::vector<unsigned char> data;
	};

	struct StorageState
	{
		std::string baseDir;
		std::string groupID;
		std::string savePackName;
		unsigned int saveVersion = 0;
		std::wstring defaultSaveName;
		std::wstring saveTitle;
		std::string uniqueFilename;
		int uniqueNumber = 1;
		bool saveDisabled = false;

		std::vector<unsigned char> saveData; // AllocateSaveData's buffer
		std::vector<unsigned char> thumbnailData;
		std::vector<unsigned char> imageData;
		std::vector<unsigned char> textData;

		std::vector<SubfileEntry> subfiles;

		C4JStorage::ESaveGameState saveState = C4JStorage::ESaveGame_Idle;

		SAVE_DETAILS lastSavesInfo = {};
		std::vector<SAVE_INFO> savesInfoStorage; // backing storage for lastSavesInfo.SaveInfoA
	};

	StorageState g_storage;

	std::string CurrentSavePath()
	{
		std::string name = g_storage.uniqueFilename.empty() ? "world" : g_storage.uniqueFilename;
		return g_storage.baseDir + "/" + name + ".dat";
	}
}

C4JStorage::C4JStorage()
	: m_pStringTable(nullptr)
{
}

void C4JStorage::Tick(void)
{
	// Local single-player saves complete synchronously (see SaveSaveData/
	// SaveSubfiles below) so there is no async state machine to advance here,
	// unlike the console platforms this class was originally written for.
}

// --- Messages / online-service surface: no Xbox LIVE/PSN equivalent exists
// on desktop Linux and this port stands up no replacement service, so these
// consistently report "unavailable" rather than silently succeeding. ---

C4JStorage::EMessageResult C4JStorage::RequestMessageBox(UINT uiTitle, UINT uiText, UINT *uiOptionA, UINT uiOptionC, DWORD dwPad,
	int (*Func)(LPVOID, int, const C4JStorage::EMessageResult), LPVOID lpParam, C4JStringTable *pStringTable, WCHAR *pwchFormatString, DWORD dwFocusButton)
{
	(void)uiTitle; (void)uiText; (void)uiOptionA; (void)uiOptionC; (void)dwPad; (void)pStringTable; (void)pwchFormatString; (void)dwFocusButton;
	if (Func) Func(lpParam, 0, EMessage_Undefined);
	return EMessage_Undefined;
}

C4JStorage::EMessageResult C4JStorage::GetMessageBoxResult()
{
	return EMessage_Undefined;
}

bool C4JStorage::SetSaveDevice(int (*Func)(LPVOID, const bool), LPVOID lpParam, bool bForceResetOfSaveDevice)
{
	// There is exactly one "save device" on desktop Linux: the local
	// filesystem. Always succeeds.
	(void)bForceResetOfSaveDevice;
	if (Func) Func(lpParam, true);
	return true;
}

// --- Save/load (real implementation) ---

void C4JStorage::Init(unsigned int uiSaveVersion, LPCWSTR pwchDefaultSaveName, char *pszSavePackName, int iMinimumSaveSize,
	int (*Func)(LPVOID, const ESavingMessage, int), LPVOID lpParam, LPCSTR szGroupID)
{
	(void)iMinimumSaveSize; (void)Func; (void)lpParam;
	g_storage.baseDir = GetSaveBaseDir();
	g_storage.saveVersion = uiSaveVersion;
	g_storage.defaultSaveName = pwchDefaultSaveName ? pwchDefaultSaveName : L"";
	g_storage.savePackName = pszSavePackName ? pszSavePackName : "";
	g_storage.groupID = szGroupID ? szGroupID : "";
	g_storage.saveState = ESaveGame_Idle;
}

void C4JStorage::ResetSaveData()
{
	g_storage.saveData.clear();
	g_storage.uniqueFilename.clear();
}

void C4JStorage::SetDefaultSaveNameForKeyboardDisplay(LPCWSTR pwchDefaultSaveName)
{
	g_storage.defaultSaveName = pwchDefaultSaveName ? pwchDefaultSaveName : L"";
}

void C4JStorage::SetSaveTitle(LPCWSTR pwchDefaultSaveName)
{
	g_storage.saveTitle = pwchDefaultSaveName ? pwchDefaultSaveName : L"";
}

bool C4JStorage::GetSaveUniqueNumber(INT *piVal)
{
	if (piVal) *piVal = g_storage.uniqueNumber++;
	return true;
}

bool C4JStorage::GetSaveUniqueFilename(char *pszName)
{
	if (!pszName) return false;
	strncpy(pszName, g_storage.uniqueFilename.c_str(), MAX_SAVEFILENAME_LENGTH - 1);
	pszName[MAX_SAVEFILENAME_LENGTH - 1] = '\0';
	return !g_storage.uniqueFilename.empty();
}

void C4JStorage::SetSaveUniqueFilename(char *szFilename)
{
	g_storage.uniqueFilename = szFilename ? szFilename : "";
}

void C4JStorage::SetState(ESaveGameControlState eControlState, int (*Func)(LPVOID, const bool), LPVOID lpParam)
{
	(void)eControlState;
	if (Func) Func(lpParam, true);
}

void C4JStorage::SetSaveDisabled(bool bDisable)
{
	g_storage.saveDisabled = bDisable;
}

bool C4JStorage::GetSaveDisabled(void)
{
	return g_storage.saveDisabled;
}

unsigned int C4JStorage::GetSaveSize()
{
	return (unsigned int)g_storage.saveData.size();
}

void C4JStorage::GetSaveData(void *pvData, unsigned int *puiBytes)
{
	if (!pvData || !puiBytes) return;
	unsigned int toCopy = (unsigned int)g_storage.saveData.size();
	if (toCopy > *puiBytes) toCopy = *puiBytes;
	if (toCopy > 0) memcpy(pvData, g_storage.saveData.data(), toCopy);
	*puiBytes = toCopy;
}

PVOID C4JStorage::AllocateSaveData(unsigned int uiBytes)
{
	g_storage.saveData.assign(uiBytes, 0);
	return g_storage.saveData.empty() ? nullptr : g_storage.saveData.data();
}

void C4JStorage::SetSaveImages(PBYTE pbThumbnail, DWORD dwThumbnailBytes, PBYTE pbImage, DWORD dwImageBytes, PBYTE pbTextData, DWORD dwTextDataBytes)
{
	g_storage.thumbnailData.assign(pbThumbnail, pbThumbnail + dwThumbnailBytes);
	g_storage.imageData.assign(pbImage, pbImage + dwImageBytes);
	g_storage.textData.assign(pbTextData, pbTextData + dwTextDataBytes);
}

C4JStorage::ESaveGameState C4JStorage::SaveSaveData(int (*Func)(LPVOID, const bool), LPVOID lpParam)
{
	if (g_storage.saveDisabled)
	{
		if (Func) Func(lpParam, false);
		return ESaveGame_Idle;
	}

	if (g_storage.uniqueFilename.empty())
		g_storage.uniqueFilename = "world";

	bool ok = false;
	{
		std::ofstream out(CurrentSavePath(), std::ios::binary | std::ios::trunc);
		if (out)
		{
			out.write((const char *)g_storage.saveData.data(), (std::streamsize)g_storage.saveData.size());
			ok = out.good();
		}
	}
	if (ok && !g_storage.thumbnailData.empty())
	{
		std::ofstream thumb(g_storage.baseDir + "/" + g_storage.uniqueFilename + ".thumb", std::ios::binary | std::ios::trunc);
		if (thumb) thumb.write((const char *)g_storage.thumbnailData.data(), (std::streamsize)g_storage.thumbnailData.size());
	}

	g_storage.saveState = ESaveGame_Idle;
	if (Func) Func(lpParam, ok);
	return ok ? ESaveGame_Idle : ESaveGame_Idle;
}

void C4JStorage::CopySaveDataToNewSave(PBYTE pbThumbnail, DWORD cbThumbnail, WCHAR *wchNewName, int (*Func)(LPVOID, bool), LPVOID lpParam)
{
	(void)pbThumbnail; (void)cbThumbnail;
	std::string oldPath = CurrentSavePath();
	g_storage.uniqueFilename = WStringToUtf8(wchNewName);
	bool ok = false;
	{
		std::ifstream in(oldPath, std::ios::binary);
		std::ofstream out(CurrentSavePath(), std::ios::binary | std::ios::trunc);
		if (in && out)
		{
			out << in.rdbuf();
			ok = out.good();
		}
	}
	if (Func) Func(lpParam, ok);
}

void C4JStorage::SetSaveDeviceSelected(unsigned int uiPad, bool bSelected)
{
	(void)uiPad; (void)bSelected;
}

bool C4JStorage::GetSaveDeviceSelected(unsigned int iPad)
{
	(void)iPad;
	return true; // the local filesystem "device" is always selected
}

C4JStorage::ESaveGameState C4JStorage::DoesSaveExist(bool *pbExists)
{
	struct stat st;
	bool exists = (stat(CurrentSavePath().c_str(), &st) == 0);
	if (pbExists) *pbExists = exists;
	return ESaveGame_Idle;
}

bool C4JStorage::EnoughSpaceForAMinSaveGame()
{
	return true; // no meaningful "device capacity" concept for a local disk save
}

void C4JStorage::SetSaveMessageVPosition(float fY)
{
	(void)fY; // no "Saving..." HUD overlay exists yet - nothing to position
}

C4JStorage::ESaveGameState C4JStorage::GetSavesInfo(int iPad, int (*Func)(LPVOID, SAVE_DETAILS *, const bool), LPVOID lpParam, const char *pszSavePackName)
{
	(void)iPad; (void)pszSavePackName;

	g_storage.savesInfoStorage.clear();
	DIR *dir = opendir(g_storage.baseDir.c_str());
	if (dir)
	{
		struct dirent *entry;
		while ((entry = readdir(dir)) != nullptr)
		{
			std::string name = entry->d_name;
			const std::string ext = ".dat";
			if (name.size() <= ext.size() || name.compare(name.size() - ext.size(), ext.size(), ext) != 0)
				continue;

			SAVE_INFO info = {};
			std::string stem = name.substr(0, name.size() - ext.size());
			strncpy(info.UTF8SaveFilename, stem.c_str(), MAX_SAVEFILENAME_LENGTH - 1);
			strncpy(info.UTF8SaveTitle, stem.c_str(), MAX_DISPLAYNAME_LENGTH - 1);

			struct stat st;
			std::string fullPath = g_storage.baseDir + "/" + name;
			if (stat(fullPath.c_str(), &st) == 0)
			{
				info.metaData.modifiedTime = st.st_mtime;
				info.metaData.dataSize = (unsigned int)st.st_size;
			}
			info.metaData.thumbnailSize = 0;
			info.thumbnailData = nullptr;
			g_storage.savesInfoStorage.push_back(info);
		}
		closedir(dir);
	}

	g_storage.lastSavesInfo.iSaveC = (int)g_storage.savesInfoStorage.size();
	g_storage.lastSavesInfo.SaveInfoA = g_storage.savesInfoStorage.empty() ? nullptr : g_storage.savesInfoStorage.data();

	if (Func) Func(lpParam, &g_storage.lastSavesInfo, true);
	return g_storage.savesInfoStorage.empty() ? ESaveGame_Idle : ESaveGame_Idle;
}

PSAVE_DETAILS C4JStorage::ReturnSavesInfo()
{
	return &g_storage.lastSavesInfo;
}

void C4JStorage::ClearSavesInfo()
{
	g_storage.savesInfoStorage.clear();
	g_storage.lastSavesInfo.iSaveC = 0;
	g_storage.lastSavesInfo.SaveInfoA = nullptr;
}

C4JStorage::ESaveGameState C4JStorage::LoadSaveDataThumbnail(PSAVE_INFO pSaveInfo, int (*Func)(LPVOID, PBYTE, DWORD), LPVOID lpParam)
{
	if (!pSaveInfo) { if (Func) Func(lpParam, nullptr, 0); return ESaveGame_Idle; }
	std::string path = g_storage.baseDir + "/" + std::string(pSaveInfo->UTF8SaveFilename) + ".thumb";
	std::ifstream in(path, std::ios::binary);
	std::vector<unsigned char> data;
	if (in)
	{
		in.seekg(0, std::ios::end);
		size_t size = (size_t)in.tellg();
		in.seekg(0);
		data.resize(size);
		if (size > 0) in.read((char *)data.data(), (std::streamsize)size);
	}
	if (Func) Func(lpParam, data.empty() ? nullptr : data.data(), (DWORD)data.size());
	return ESaveGame_Idle;
}

void C4JStorage::GetSaveCacheFileInfo(DWORD dwFile, XCONTENT_DATA &xContentData)
{
	(void)dwFile; (void)xContentData; // TMS content-cache concept, not applicable locally
}

void C4JStorage::GetSaveCacheFileInfo(DWORD dwFile, PBYTE *ppbImageData, DWORD *pdwImageBytes)
{
	(void)dwFile;
	if (ppbImageData) *ppbImageData = nullptr;
	if (pdwImageBytes) *pdwImageBytes = 0;
}

C4JStorage::ESaveGameState C4JStorage::LoadSaveData(PSAVE_INFO pSaveInfo, int (*Func)(LPVOID, const bool, const bool), LPVOID lpParam)
{
	std::string filename = pSaveInfo ? pSaveInfo->UTF8SaveFilename : g_storage.uniqueFilename;
	if (pSaveInfo) g_storage.uniqueFilename = filename;

	std::ifstream in(g_storage.baseDir + "/" + filename + ".dat", std::ios::binary);
	bool ok = false;
	if (in)
	{
		in.seekg(0, std::ios::end);
		size_t size = (size_t)in.tellg();
		in.seekg(0);
		g_storage.saveData.resize(size);
		if (size > 0) in.read((char *)g_storage.saveData.data(), (std::streamsize)size);
		ok = in.good() || in.eof();
	}
	if (Func) Func(lpParam, ok, false); // (success, changedDevice) - "changedDevice" never applies locally
	return ESaveGame_Idle;
}

C4JStorage::ESaveGameState C4JStorage::DeleteSaveData(PSAVE_INFO pSaveInfo, int (*Func)(LPVOID, const bool), LPVOID lpParam)
{
	bool ok = false;
	if (pSaveInfo)
	{
		std::string base = g_storage.baseDir + "/" + std::string(pSaveInfo->UTF8SaveFilename);
		ok = (remove((base + ".dat").c_str()) == 0);
		remove((base + ".thumb").c_str());
	}
	if (Func) Func(lpParam, ok);
	return ESaveGame_Idle;
}

// --- DLC / marketplace / TMS: Xbox LIVE/PSN-specific, no Linux desktop
// equivalent and no replacement online service for this port. Report
// "unavailable"/idle/empty rather than pretending to succeed. ---

void C4JStorage::RegisterMarketplaceCountsCallback(int (*Func)(LPVOID, C4JStorage::DLC_TMS_DETAILS *, int), LPVOID lpParam)
{
	(void)Func; (void)lpParam;
}

void C4JStorage::SetDLCPackageRoot(char *pszDLCRoot)
{
	(void)pszDLCRoot;
}

C4JStorage::EDLCStatus C4JStorage::GetDLCOffers(int iPad, int (*Func)(LPVOID, int, DWORD, int), LPVOID lpParam, DWORD dwOfferTypesBitmask)
{
	(void)iPad; (void)dwOfferTypesBitmask;
	if (Func) Func(lpParam, 0, 0, EDLC_NoOffers);
	return EDLC_NoOffers;
}

DWORD C4JStorage::CancelGetDLCOffers() { return 0; }
void C4JStorage::ClearDLCOffers() {}

XMARKETPLACE_CONTENTOFFER_INFO &C4JStorage::GetOffer(DWORD dw)
{
	(void)dw;
	static XMARKETPLACE_CONTENTOFFER_INFO dummy = {};
	return dummy;
}

int C4JStorage::GetOfferCount() { return 0; }

DWORD C4JStorage::InstallOffer(int iOfferIDC, __uint64 *ullOfferIDA, int (*Func)(LPVOID, int, int), LPVOID lpParam, bool bTrial)
{
	(void)iOfferIDC; (void)ullOfferIDA; (void)bTrial;
	if (Func) Func(lpParam, 0, 0);
	return 0;
}

DWORD C4JStorage::GetAvailableDLCCount(int iPad) { (void)iPad; return 0; }

C4JStorage::EDLCStatus C4JStorage::GetInstalledDLC(int iPad, int (*Func)(LPVOID, int, int), LPVOID lpParam)
{
	(void)iPad;
	if (Func) Func(lpParam, 0, 0);
	return EDLC_NoInstalledDLC;
}

XCONTENT_DATA &C4JStorage::GetDLC(DWORD dw)
{
	(void)dw;
	static XCONTENT_DATA dummy = {};
	return dummy;
}

DWORD C4JStorage::MountInstalledDLC(int iPad, DWORD dwDLC, int (*Func)(LPVOID, int, DWORD, DWORD), LPVOID lpParam, LPCSTR szMountDrive)
{
	(void)iPad; (void)dwDLC; (void)szMountDrive;
	if (Func) Func(lpParam, 0, 0, 0);
	return 0;
}

DWORD C4JStorage::UnmountInstalledDLC(LPCSTR szMountDrive) { (void)szMountDrive; return 0; }

void C4JStorage::GetMountedDLCFileList(const char *szMountDrive, std::vector<std::string> &fileList)
{
	(void)szMountDrive;
	fileList.clear();
}

std::string C4JStorage::GetMountedPath(std::string szMount)
{
	// No DLC/content mounting on Linux - paths are used as-is, matching how
	// File.cpp/File.cpp's callers treat GetMountedPath as a passthrough on
	// every other platform when nothing is actually mounted.
	return szMount;
}

C4JStorage::ETMSStatus C4JStorage::ReadTMSFile(int iQuadrant, eGlobalStorage eStorageFacility, C4JStorage::eTMS_FileType eFileType,
	WCHAR *pwchFilename, BYTE **ppBuffer, DWORD *pdwBufferSize, int (*Func)(LPVOID, WCHAR *, int, bool, int), LPVOID lpParam, int iAction)
{
	(void)iQuadrant; (void)eStorageFacility; (void)eFileType; (void)iAction;
	if (ppBuffer) *ppBuffer = nullptr;
	if (pdwBufferSize) *pdwBufferSize = 0;
	if (Func) Func(lpParam, pwchFilename, 0, false, 0);
	return ETMSStatus_Fail;
}

bool C4JStorage::WriteTMSFile(int iQuadrant, eGlobalStorage eStorageFacility, WCHAR *pwchFilename, BYTE *pBuffer, DWORD dwBufferSize)
{
	(void)iQuadrant; (void)eStorageFacility; (void)pwchFilename; (void)pBuffer; (void)dwBufferSize;
	return false;
}

bool C4JStorage::DeleteTMSFile(int iQuadrant, eGlobalStorage eStorageFacility, WCHAR *pwchFilename)
{
	(void)iQuadrant; (void)eStorageFacility; (void)pwchFilename;
	return false;
}

void C4JStorage::StoreTMSPathName(WCHAR *pwchName)
{
	(void)pwchName;
}

C4JStorage::ETMSStatus C4JStorage::TMSPP_ReadFile(int iPad, C4JStorage::eGlobalStorage eStorageFacility, C4JStorage::eTMS_FILETYPEVAL eFileTypeVal,
	LPCSTR szFilename, int (*Func)(LPVOID, int, int, PTMSPP_FILEDATA, LPCSTR), LPVOID lpParam, int iUserData)
{
	(void)iPad; (void)eStorageFacility; (void)eFileTypeVal; (void)iUserData;
	if (Func) Func(lpParam, 0, 0, nullptr, szFilename);
	return ETMSStatus_Fail;
}

// --- Split-save Subfile API (ConsoleSaveFileSplit.cpp calls these
// unconditionally - see the header comment where this class extended
// Windows64's copy in Phase 2). Backed by g_storage.subfiles, flushed to a
// single "<save>.dat" file laid out as [count][id,size,data]*. ---

C4JStorage::ESaveGameState C4JStorage::GetSaveState()
{
	return g_storage.saveState;
}

unsigned int C4JStorage::GetSubfileCount()
{
	return (unsigned int)g_storage.subfiles.size();
}

void C4JStorage::ResetSubfiles()
{
	g_storage.subfiles.clear();
}

void C4JStorage::GetSubfileDetails(int idx, unsigned int *subfileId, unsigned char **data, unsigned int *sizeOut)
{
	if (idx < 0 || idx >= (int)g_storage.subfiles.size()) return;
	SubfileEntry &e = g_storage.subfiles[idx];
	if (subfileId) *subfileId = e.id;
	if (data) *data = e.data.empty() ? nullptr : e.data.data();
	if (sizeOut) *sizeOut = (unsigned int)e.data.size();
}

void C4JStorage::UpdateSubfile(int idx, unsigned char *data, unsigned int size)
{
	if (idx < 0 || idx >= (int)g_storage.subfiles.size()) return;
	g_storage.subfiles[idx].data.assign(data, data + size);
}

int C4JStorage::AddSubfile(unsigned int subfileId)
{
	SubfileEntry e;
	e.id = subfileId;
	g_storage.subfiles.push_back(e);
	return (int)g_storage.subfiles.size() - 1;
}

C4JStorage::ESaveGameState C4JStorage::SaveSubfiles(int (*Func)(LPVOID, const bool), LPVOID lpParam)
{
	if (g_storage.uniqueFilename.empty())
		g_storage.uniqueFilename = "world";

	bool ok = false;
	{
		std::ofstream out(CurrentSavePath(), std::ios::binary | std::ios::trunc);
		if (out)
		{
			unsigned int count = (unsigned int)g_storage.subfiles.size();
			out.write((const char *)&count, sizeof(count));
			for (auto &e : g_storage.subfiles)
			{
				unsigned int size = (unsigned int)e.data.size();
				out.write((const char *)&e.id, sizeof(e.id));
				out.write((const char *)&size, sizeof(size));
				if (size > 0) out.write((const char *)e.data.data(), size);
			}
			ok = out.good();
		}
	}
	if (Func) Func(lpParam, ok);
	return ESaveGame_Idle;
}

unsigned int C4JStorage::CRC(unsigned char *buf, int len)
{
	// Standard CRC-32 (poly 0xEDB88320) - callers only use this for local
	// integrity checks, not cross-platform save compatibility, so any
	// correct CRC-32 implementation works.
	unsigned int crc = 0xFFFFFFFFu;
	for (int i = 0; i < len; i++)
	{
		crc ^= buf[i];
		for (int b = 0; b < 8; b++)
			crc = (crc >> 1) ^ (0xEDB88320u & (unsigned int)(-(int)(crc & 1)));
	}
	return ~crc;
}

C4JStorage StorageManager;
