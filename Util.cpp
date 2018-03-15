#include "stdafx.h"
#include "Util.h"

static const struct {
	COLORREF color;
	const char *command;
} COMMAND2COLOR[] = {
	{RGB(0xFF, 0x00, 0x00), "red"},
	{RGB(0xFF, 0x80, 0x80), "pink"},
	{RGB(0xFF, 0xC0, 0x00), "orange"},
	{RGB(0xFF, 0xFF, 0x00), "yellow"},
	{RGB(0x00, 0xFF, 0x00), "green"},
	{RGB(0x00, 0xFF, 0xFF), "cyan"},
	{RGB(0x00, 0x00, 0xFF), "blue"},
	{RGB(0xC0, 0x00, 0xFF), "purple"},
	{RGB(0x00, 0x00, 0x00), "black"},
	{RGB(0xCC, 0xCC, 0x99), "white2"},
	{RGB(0xCC, 0xCC, 0x99), "niconicowhite"},
	{RGB(0xCC, 0x00, 0x33), "red2"},
	{RGB(0xCC, 0x00, 0x33), "truered"},
	{RGB(0xFF, 0x33, 0xCC), "pink2"},
	{RGB(0xFF, 0x66, 0x00), "orange2"},
	{RGB(0xFF, 0x66, 0x00), "passionorange"},
	{RGB(0x99, 0x99, 0x00), "yellow2"},
	{RGB(0x99, 0x99, 0x00), "madyellow"},
	{RGB(0x00, 0xCC, 0x66), "green2"},
	{RGB(0x00, 0xCC, 0x66), "elementalgreen"},
	{RGB(0x00, 0xCC, 0xCC), "cyan2"},
	{RGB(0x33, 0x99, 0xFF), "blue2"},
	{RGB(0x33, 0x99, 0xFF), "marineblue"},
	{RGB(0x66, 0x33, 0xCC), "purple2"},
	{RGB(0x66, 0x33, 0xCC), "nobleviolet"},
	{RGB(0x66, 0x66, 0x66), "black2"},
};

// 必要なバッファを確保してGetPrivateProfileSection()を呼ぶ
std::vector<TCHAR> GetPrivateProfileSectionBuffer(LPCTSTR lpAppName, LPCTSTR lpFileName)
{
	std::vector<TCHAR> buf(4096);
	for (;;) {
		DWORD len = GetPrivateProfileSection(lpAppName, buf.data(), static_cast<DWORD>(buf.size()), lpFileName);
		if (len < buf.size() - 2) {
			buf.resize(len + 1);
			break;
		}
		buf.resize(buf.size() * 2);
	}
	return buf;
}

// GetPrivateProfileSection()で取得したバッファから、キーに対応する文字列を取得する
void GetBufferedProfileString(LPCTSTR lpBuff, LPCTSTR lpKeyName, LPCTSTR lpDefault, LPTSTR lpReturnedString, DWORD nSize)
{
	size_t nKeyLen = _tcslen(lpKeyName);
	while (*lpBuff) {
		size_t nLen = _tcslen(lpBuff);
		if (!_tcsnicmp(lpBuff, lpKeyName, nKeyLen) && lpBuff[nKeyLen] == TEXT('=')) {
			if ((lpBuff[nKeyLen + 1] == TEXT('\'') || lpBuff[nKeyLen + 1] == TEXT('"')) &&
			    nLen >= nKeyLen + 3 && lpBuff[nKeyLen + 1] == lpBuff[nLen - 1]) {
				_tcsncpy_s(lpReturnedString, nSize, lpBuff + nKeyLen + 2, min(nLen - nKeyLen - 3, static_cast<size_t>(nSize - 1)));
			} else {
				_tcsncpy_s(lpReturnedString, nSize, lpBuff + nKeyLen + 1, _TRUNCATE);
			}
			return;
		}
		lpBuff += nLen + 1;
	}
	_tcsncpy_s(lpReturnedString, nSize, lpDefault, _TRUNCATE);
}

// GetPrivateProfileSection()で取得したバッファから、キーに対応する数値を取得する
int GetBufferedProfileInt(LPCTSTR lpBuff, LPCTSTR lpKeyName, int nDefault)
{
	TCHAR sz[16];
	GetBufferedProfileString(lpBuff, lpKeyName, TEXT(""), sz, _countof(sz));
	LPTSTR endp;
	int nRet = _tcstol(sz, &endp, 10);
	return endp == sz ? nDefault : nRet;
}

BOOL WritePrivateProfileInt(LPCTSTR lpAppName, LPCTSTR lpKeyName, int value, LPCTSTR lpFileName)
{
	TCHAR sz[16];
	_stprintf_s(sz, TEXT("%d"), value);
	return WritePrivateProfileString(lpAppName, lpKeyName, sz, lpFileName);
}

DWORD GetLongModuleFileName(HMODULE hModule, LPTSTR lpFileName, DWORD nSize)
{
	TCHAR longOrShortName[MAX_PATH];
	DWORD nRet = GetModuleFileName(hModule, longOrShortName, MAX_PATH);
	if (nRet && nRet < MAX_PATH) {
		nRet = GetLongPathName(longOrShortName, lpFileName, nSize);
		if (nRet < nSize) return nRet;
	}
	return 0;
}

size_t FindHttpBody(const char *str)
{
	const char *p = strstr(str, "\r\n\r\n");
	return p ? p + 4 - str : strlen(str);
}

bool HasToken(const char *str, const char *substr)
{
	size_t len = strlen(substr);
	if (!strncmp(str, substr, len) && (!str[len] || str[len]==' ')) {
		return true;
	}
	for (; *str; ++str) {
		if (*str==' ' && !strncmp(str+1, substr, len) && (!str[1+len] || str[1+len]==' ')) {
			return true;
		}
	}
	return false;
}

void DecodeEntityReference(TCHAR *str)
{
	static const struct {
		TCHAR ent;
		LPCTSTR ref;
	} ENT_REF[] = {
		{TEXT('<'), TEXT("lt;")},
		{TEXT('>'), TEXT("gt;")},
		{TEXT('&'), TEXT("amp;")},
		{TEXT('"'), TEXT("quot;")},
		{TEXT('\''), TEXT("apos;")},
		{TEXT('\n'), TEXT("#10;")},
		{TEXT('\r'), TEXT("#13;")},
	};
	TCHAR *p = str;
	for (; *str; ++p) {
		if ((*p = *str++) == TEXT('&')) {
			for (int i = 0; i < _countof(ENT_REF); ++i) {
				size_t len = _tcslen(ENT_REF[i].ref);
				if (!_tcsncmp(str, ENT_REF[i].ref, len)) {
					str += len;
					*p = ENT_REF[i].ent;
					break;
				}
			}
		}
	}
	*p = TEXT('\0');
}

void EncodeEntityReference(const char *src, char *dest, size_t destSize)
{
	// 切り捨てを防ぐには'&'に対して5倍のバッファを見積もる
	dest[0] = '\0';
	for (; *src; ++src) {
		char s[2] = {*src};
		const char *p = *s=='<' ? "&lt;" : *s=='>' ? "&gt;" : *s=='&' ? "&amp;" : s;
		if (strlen(p) >= destSize) {
			break;
		}
		strcpy_s(dest, destSize, p);
		dest += strlen(p);
		destSize -= strlen(p);
	}
}

COLORREF GetColor(const char *command)
{
	static const std::regex re("(?:^| )#([0-9A-Fa-f]{6})(?: |$)");
	std::cmatch m;
	if (std::regex_search(command, m, re)) {
		int color = strtol(m[1].first, nullptr, 16);
		return RGB((color>>16)&0xFF, (color>>8)&0xFF, color&0xFF);
	}
	for (int i = 0; i < _countof(COMMAND2COLOR); ++i) {
		if (HasToken(command, COMMAND2COLOR[i].command)) {
			return COMMAND2COLOR[i].color;
		}
	}
	return RGB(0xFF, 0xFF, 0xFF);
}

bool GetChatDate(unsigned int *tm, const char *tag)
{
	// TODO: dateは秒精度しかないので独自に属性値つけるかvposを解釈するとよりよいかも
	static const std::regex re("^<chat[^>]*? date=\"(\\d+)\"");
	std::cmatch m;
	if (std::regex_search(tag, m, re)) {
		*tm = strtoul(m[1].first, nullptr, 10);
		return true;
	}
	return false;
}

void UnixTimeToFileTime(unsigned int tm, FILETIME *pft)
{
	LONGLONG ll = static_cast<LONGLONG>(tm) * 10000000 + 116444736000000000;
	pft->dwLowDateTime = static_cast<DWORD>(ll);
	pft->dwHighDateTime = static_cast<DWORD>(ll >> 32);
}

unsigned int FileTimeToUnixTime(const FILETIME &ft)
{
	LONGLONG ll = (static_cast<LONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
	return static_cast<unsigned int>((ll - 116444736000000000) / 10000000);
}

FILETIME &operator+=(FILETIME &ft, LONGLONG offset)
{
	LONGLONG ll = (static_cast<LONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
	ll += offset;
	ft.dwLowDateTime = static_cast<DWORD>(ll);
	ft.dwHighDateTime = static_cast<DWORD>(ll >> 32);
	return ft;
}

LONGLONG operator-(const FILETIME &ft1, const FILETIME &ft2)
{
	LONGLONG ll1 = (static_cast<LONGLONG>(ft1.dwHighDateTime) << 32) | ft1.dwLowDateTime;
	LONGLONG ll2 = (static_cast<LONGLONG>(ft2.dwHighDateTime) << 32) | ft2.dwLowDateTime;
	return ll1 - ll2;
}

bool AribToFileTime(const BYTE *pData, FILETIME *pft)
{
	if (pData[0]==0xFF && pData[1]==0xFF && pData[2]==0xFF && pData[3]==0xFF && pData[4]==0xFF) {
		// 不指定
		return false;
	}
	// 1858-11-17
	pft->dwLowDateTime = 2303934464;
	pft->dwHighDateTime = 18947191;
	// MJD形式の日付
	*pft += (pData[0] << 8 | pData[1]) * FILETIME_MILLISECOND * 86400000;
	// BCD形式の時刻
	*pft += ((pData[2] >> 4) * 10 + (pData[2] & 0x0F)) * FILETIME_MILLISECOND * 3600000;
	*pft += ((pData[3] >> 4) * 10 + (pData[3] & 0x0F)) * FILETIME_MILLISECOND * 60000;
	*pft += ((pData[4] >> 4) * 10 + (pData[4] & 0x0F)) * FILETIME_MILLISECOND * 1000;
	return true;
}

// ファイルを開くダイアログ
BOOL FileOpenDialog(HWND hwndOwner, LPCTSTR lpstrFilter, LPTSTR lpstrFile, DWORD nMaxFile)
{
	OPENFILENAME ofn = {};
	ofn.lStructSize = sizeof(OPENFILENAME);
	ofn.hwndOwner = hwndOwner;
	ofn.lpstrFilter = lpstrFilter;
	ofn.lpstrTitle = TEXT("ファイルを開く");
	ofn.Flags = OFN_HIDEREADONLY | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
	ofn.lpstrFile = lpstrFile;
	ofn.nMaxFile = nMaxFile;
	lpstrFile[0] = TEXT('\0');
	return GetOpenFileName(&ofn);
}

// ローカル形式をタイムシフトする
static bool TxtToLocalFormat(LPCTSTR srcPath, LPCTSTR destPath, unsigned int tmNew)
{
	std::unique_ptr<FILE, decltype(&fclose)> fpDest(nullptr, fclose);
	FILE *fp;
	size_t len = _tcslen(srcPath);
	if (len >= 5 && !_tcschr(TEXT("/\\"), srcPath[len - 5]) && !_tcsicmp(&srcPath[len - 4], TEXT(".txt")) &&
	    !_tfopen_s(&fp, srcPath, TEXT("rN"))) {
		std::unique_ptr<FILE, decltype(&fclose)> fpSrc(fp, fclose);
		const std::regex re("^<chat[^>]*? date=\"(\\d+)\"");
		std::cmatch m;
		char buf[4096];
		unsigned int tmOld = 0;
		while (fgets(buf, _countof(buf), fpSrc.get())) {
			if (std::regex_search(buf, m, re)) {
				// chatタグが1行以上見つかれば書き込みを始める
				if (!fpDest) {
					if (_tfopen_s(&fp, destPath, TEXT("wN"))) {
						break;
					}
					fpDest.reset(fp);
				}
				fwrite(buf, sizeof(char), m[1].first - buf, fpDest.get());
				unsigned int tm = strtoul(m[1].first, nullptr, 10);
				if (!tmOld) {
					tmOld = tm;
				}
				fprintf(fpDest.get(), "%u", !tmNew ? tm : tm - tmOld + tmNew);
				fputs(m[1].second, fpDest.get());
			}
		}
	}
	return fpDest != nullptr;
}

static void WriteChatTag(FILE *fpDest, const std::cmatch &m, unsigned int *ptmOld, unsigned int tmNew)
{
	fwrite(m[0].first, sizeof(char), m[1].first - m[0].first, fpDest);
	unsigned int tm = strtoul(m[1].first, nullptr, 10);
	if (!*ptmOld) {
		*ptmOld = tm;
	}
	fprintf(fpDest, "%u", !tmNew ? tm : tm - *ptmOld + tmNew);
	const char *p = m[1].second;
	int len = 0;
	for (; p + len < m[0].second; ++len) {
		// 改行文字は数値文字参照に置換
		if (p[len] == '\n' || p[len] == '\r') {
			fwrite(p, sizeof(char), len, fpDest);
			fprintf(fpDest, "&#%d;", p[len]);
			p += len + 1;
			len = -1;
		}
	}
	fwrite(p, sizeof(char), len, fpDest);
	fputs("\n", fpDest);
}

// JikkyoRec.jklをローカル形式に変換する
static bool JklToLocalFormat(LPCTSTR srcPath, LPCTSTR destPath, unsigned int tmNew)
{
	std::unique_ptr<FILE, decltype(&fclose)> fpDest(nullptr, fclose);
	FILE *fp;
	size_t len = _tcslen(srcPath);
	if (len >= 5 && !_tcschr(TEXT("/\\"), srcPath[len - 5]) && !_tcsicmp(&srcPath[len - 4], TEXT(".jkl")) &&
	    !_tfopen_s(&fp, srcPath, TEXT("rbN"))) {
		std::unique_ptr<FILE, decltype(&fclose)> fpSrc(fp, fclose);
		char buf[4096];
		if (fread(buf, sizeof(char), 10, fpSrc.get()) == 10 && !memcmp(buf, "<JikkyoRec", 10) && !_tfopen_s(&fp, destPath, TEXT("wN"))) {
			fpDest.reset(fp);
			// 空行まで読み飛ばす
			int c;
			for (int d = '\0'; (c = fgetc(fpSrc.get())) != EOF && !(d=='\n' && (c=='\n' || c=='\r')); d = c);

			const std::regex re("<chat[^>]*? date=\"(\\d+)\"[^]*?</chat>");
			std::cmatch m;
			int bufLen = 0;
			unsigned int tmOld = 0;
			while ((c = fgetc(fpSrc.get())) != EOF) {
				if (bufLen >= _countof(buf)) {
					bufLen = 0;
					continue;
				}
				buf[bufLen++] = static_cast<char>(c);
				if (c == '\0') {
					if (std::regex_search(buf, m, re)) {
						WriteChatTag(fpDest.get(), m, &tmOld, tmNew);
					}
					bufLen = 0;
				}
			}
		}
	}
	return fpDest != nullptr;
}

// ニコニコ実況コメントビューア.xmlをローカル形式に変換する
static bool XmlToLocalFormat(LPCTSTR srcPath, LPCTSTR destPath, unsigned int tmNew)
{
	std::unique_ptr<FILE, decltype(&fclose)> fpDest(nullptr, fclose);
	FILE *fp;
	size_t len = _tcslen(srcPath);
	if (len >= 5 && !_tcschr(TEXT("/\\"), srcPath[len - 5]) && !_tcsicmp(&srcPath[len - 4], TEXT(".xml")) &&
	    !_tfopen_s(&fp, srcPath, TEXT("rN"))) {
		std::unique_ptr<FILE, decltype(&fclose)> fpSrc(fp, fclose);
		char buf[4096];
		if (fgets(buf, _countof(buf), fpSrc.get()) && strstr(buf, "<?xml") && !_tfopen_s(&fp, destPath, TEXT("wN"))) {
			fpDest.reset(fp);
			const std::regex re("<chat[^>]*? date=\"(\\d+)\"[^]*?</chat>");
			std::cmatch m;
			char tag[8192];
			tag[0] = '\0';
			unsigned int tmOld = 0;
			while (fgets(buf, _countof(buf), fpSrc.get())) {
				if (strlen(buf) >= _countof(tag) - strlen(tag)) {
					tag[0] = '\0';
					continue;
				}
				strcat_s(tag, buf);
				if (std::regex_search(tag, m, re)) {
					WriteChatTag(fpDest.get(), m, &tmOld, tmNew);
					tag[0] = '\0';
				}
			}
		}
	}
	return fpDest != nullptr;
}

bool ImportLogfile(LPCTSTR srcPath, LPCTSTR destPath, unsigned int tmNew)
{
	return JklToLocalFormat(srcPath, destPath, tmNew) ||
	       XmlToLocalFormat(srcPath, destPath, tmNew) ||
	       TxtToLocalFormat(srcPath, destPath, tmNew);
}

// 指定プロセスを実行して標準出力の文字列を得る
bool GetProcessOutput(LPCTSTR commandLine, LPCTSTR currentDir, char *buf, size_t bufSize, int timeout)
{
	bool bRet = false;
	SECURITY_ATTRIBUTES sa;
	sa.nLength = sizeof(sa);
	sa.lpSecurityDescriptor = nullptr;
	sa.bInheritHandle = TRUE;
	HANDLE hReadPipe, hWritePipe;
	if (CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
		{
			STARTUPINFO si = {};
			si.cb = sizeof(si);
			si.dwFlags = STARTF_USESTDHANDLES;
			si.hStdOutput = hWritePipe;
			PROCESS_INFORMATION pi;
			std::vector<TCHAR> commandLineBuf(commandLine, commandLine + _tcslen(commandLine) + 1);
			if (CreateProcess(nullptr, commandLineBuf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, currentDir, &si, &pi)) {
				size_t bufCount = 0;
				bool bBreak = false;
				bRet = true;
				while (!bBreak) {
					timeout -= 100;
					if (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0) {
						bBreak = true;
					} else if (timeout <= 0) {
						bBreak = true;
						bRet = false;
					}
					DWORD avail;
					if (PeekNamedPipe(hReadPipe, nullptr, 0, nullptr, &avail, nullptr) && avail != 0) {
						if (bufCount + avail >= bufSize) {
							bBreak = true;
							bRet = false;
						} else {
							DWORD read;
							if (ReadFile(hReadPipe, &buf[bufCount], avail, &read, nullptr)) {
								bufCount += read;
							}
						}
					}
				}
				buf[bufCount] = '\0';
				CloseHandle(pi.hThread);
				CloseHandle(pi.hProcess);
			}
		}
		CloseHandle(hWritePipe);
		CloseHandle(hReadPipe);
	}
	return bRet;
}

// DPAPIでプロテクトされた文字列を復号する
std::string UnprotectDpapiToString(const char *src)
{
	std::vector<BYTE> blob;
	for (int i = 0;; ++i) {
		char c = src[i];
		if ('0' <= c && c <= '9') c -= '0';
		else if ('A' <= c && c <= 'F') c -= 'A' - 10;
		else if ('a' <= c && c <= 'f') c -= 'a' - 10;
		else break;

		if (i % 2) blob[i / 2] += c;
		else blob.push_back(static_cast<BYTE>(c * 16));
	}
	DATA_BLOB in, out = {};
	in.cbData = static_cast<DWORD>(blob.size());
	in.pbData = blob.data();
	if (!in.cbData || !CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out) || !out.pbData) {
		return "";
	}
	std::string ret(reinterpret_cast<char*>(out.pbData), out.cbData);
	SecureZeroMemory(out.pbData, out.cbData);
	LocalFree(out.pbData);
	return ret;
}
