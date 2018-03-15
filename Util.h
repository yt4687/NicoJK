#pragma once

#define FILETIME_MILLISECOND 10000LL

std::vector<TCHAR> GetPrivateProfileSectionBuffer(LPCTSTR lpAppName, LPCTSTR lpFileName);
void GetBufferedProfileString(LPCTSTR lpBuff, LPCTSTR lpKeyName, LPCTSTR lpDefault, LPTSTR lpReturnedString, DWORD nSize);
int GetBufferedProfileInt(LPCTSTR lpBuff, LPCTSTR lpKeyName, int nDefault);
BOOL WritePrivateProfileInt(LPCTSTR lpAppName, LPCTSTR lpKeyName, int value, LPCTSTR lpFileName);
DWORD GetLongModuleFileName(HMODULE hModule, LPTSTR lpFileName, DWORD nSize);
size_t FindHttpBody(const char *str);
bool HasToken(const char *str, const char *substr);
void DecodeEntityReference(TCHAR *str);
void EncodeEntityReference(const char *src, char *dest, size_t destSize);
COLORREF GetColor(const char *command);
bool GetChatDate(unsigned int *tm, const char *tag);
void UnixTimeToFileTime(unsigned int tm, FILETIME *pft);
unsigned int FileTimeToUnixTime(const FILETIME &ft);
FILETIME &operator+=(FILETIME &ft, LONGLONG offset);
LONGLONG operator-(const FILETIME &ft1, const FILETIME &ft2);
bool AribToFileTime(const BYTE *pData, FILETIME *pft);
BOOL FileOpenDialog(HWND hwndOwner, LPCTSTR lpstrFilter, LPTSTR lpstrFile, DWORD nMaxFile);
bool ImportLogfile(LPCTSTR srcPath, LPCTSTR destPath, unsigned int tmNew);
bool GetProcessOutput(LPCTSTR commandLine, LPCTSTR currentDir, char *buf, size_t bufSize, int timeout = INT_MAX);
std::string UnprotectDpapiToString(const char *src);

// HTTPヘッダフィールドを連結付加する
template<size_t strSize>
void AppendHttpHeader(char (&str)[strSize], const char *field, const char *value, const char *trail)
{
	// valueが空文字列なら何もしない
	if (value[0]) {
		strcat_s(str, field);
		strcat_s(str, value);
		strcat_s(str, trail);
	}
}

// FindFirstFile()の結果を列挙する
template<class P>
void EnumFindFile(LPCTSTR pattern, P enumProc)
{
	WIN32_FIND_DATA findData;
	HANDLE hFind = FindFirstFile(pattern, &findData);
	if (hFind != INVALID_HANDLE_VALUE) {
		do {
			enumProc(findData);
		} while (FindNextFile(hFind, &findData));
		FindClose(hFind);
	}
}

class recursive_mutex_
{
public:
	recursive_mutex_() { InitializeCriticalSection(&cs_); }
	~recursive_mutex_() { DeleteCriticalSection(&cs_); }
	void lock() { EnterCriticalSection(&cs_); }
	void unlock() { LeaveCriticalSection(&cs_); }
private:
	recursive_mutex_(const recursive_mutex_&);
	recursive_mutex_ &operator=(const recursive_mutex_&);
	CRITICAL_SECTION cs_;
};

class CBlockLock
{
public:
	CBlockLock(recursive_mutex_ *mtx) : mtx_(mtx) { mtx_->lock(); }
	~CBlockLock() { mtx_->unlock(); }
private:
	CBlockLock(const CBlockLock&);
	CBlockLock &operator=(const CBlockLock&);
	recursive_mutex_ *mtx_;
};
