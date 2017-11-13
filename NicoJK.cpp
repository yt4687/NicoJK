/*
	NicoJK
		TVTest ニコニコ実況プラグイン
*/

#include "stdafx.h"
#include "Util.h"
#include "AsyncSocket.h"
#include "TextFileReader.h"
#include "CommentWindow.h"
#define TVTEST_PLUGIN_CLASS_IMPLEMENT
#include "TVTestPlugin.h"
#include "resource.h"
#include "NetworkServiceIDTable.h"
#include "JKIDNameTable.h"
#include "NicoJK.h"

#ifdef _DEBUG
#include <stdarg.h>
inline void dprintf_real( const _TCHAR * fmt, ... )
{
  _TCHAR buf[1024];
  va_list ap;
  va_start(ap, fmt);
  _vsntprintf_s(buf, 1024, fmt, ap);
  va_end(ap);
  OutputDebugString(buf);
}
#  define dprintf dprintf_real
#else
#  define dprintf __noop
#endif

// 通信用
#define WMS_FORCE (WM_APP + 101)
#define WMS_JK (WM_APP + 102)
#define WMS_POST (WM_APP + 103)

#define WM_RESET_STREAM (WM_APP + 105)
#define WM_UPDATE_LIST (WM_APP + 106)
#define WM_SET_ZORDER (WM_APP + 107)
#define WM_POST_COMMENT (WM_APP + 108)

#define JK_HOST_NAME "jk.nicovideo.jp"

enum {
	TIMER_UPDATE = 1,
	TIMER_JK_WATCHDOG,
	TIMER_FORWARD,
	TIMER_SETUP_CURJK,
	TIMER_OPEN_DROPFILE,
	TIMER_DONE_MOVE,
	TIMER_DONE_SIZE,
	TIMER_DONE_POSCHANGE,
	TIMER_UPDATE_LIST,
};

enum {
	COMMAND_HIDE_FORCE,
	COMMAND_HIDE_COMMENT,
	COMMAND_FORWARD_A,
};

void CNicoJK::RPL_ELEM::SetEnabled(bool b)
{
	if (!pattern.empty()) {
		if (b && TEXT('A') <= pattern[0] && pattern[0] <= TEXT('Z')) {
			pattern[0] = pattern[0] - TEXT('A') + TEXT('a');
		} else if (!b && TEXT('a') <= pattern[0] && pattern[0] <= TEXT('z')) {
			pattern[0] = pattern[0] - TEXT('a') + TEXT('A');
		}
	}
}

bool CNicoJK::RPL_ELEM::SetPattern(LPCTSTR patt)
{
	// 入力パターンはsedコマンド等の形式をまねたもの
	// ただし今のところ's/{regex}/{replace}/g'のみ対応(拡張可能)
	// 先頭文字が大文字の場合はそのパターンが無効状態であることを示す
	static const std::regex reBrace("[Ss](.)(.+?)\\1(.*?)\\1g");
	std::vector<char> utf8(WideCharToMultiByte(CP_UTF8, 0, patt, -1, nullptr, 0, nullptr, nullptr));
	if (utf8.empty() || WideCharToMultiByte(CP_UTF8, 0, patt, -1, utf8.data(), static_cast<int>(utf8.size()), nullptr, nullptr) == 0) {
		return false;
	}
	std::cmatch m;
	if (!std::regex_match(utf8.data(), m, reBrace)) {
		return false;
	}
	try {
		re.assign(m[2].first, m[2].length());
	} catch (std::regex_error&) {
		return false;
	}
	pattern = patt;
	fmt.assign(m[3].first, m[3].length());
	return true;
}

CNicoJK::CNicoJK()
	: bDragAcceptFiles_(false)
	, hPanel_(nullptr)
	, hForce_(nullptr)
	, hForceFont_(nullptr)
	, bDisplayLogList_(false)
	, logListDisplayedSize_(0)
	, bPendingTimerUpdateList_(false)
	, lastUpdateListTick_(0)
	, lastCalcWidth_(0)
	, forwardTick_(0)
	, hSyncThread_(nullptr)
	, bQuitSyncThread_(false)
	, bPendingTimerForward_(false)
	, bHalfSkip_(false)
	, bFlipFlop_(false)
	, forwardOffset_(0)
	, forwardOffsetDelta_(0)
	, currentJKToGet_(-1)
	, currentJK_(-1)
	, jkLeaveThreadCheck_(0)
	, bConnectedToCommentServer_(false)
	, commentServerResponseTick_(0)
	, bGetflvIsPremium_(false)
	, lastChatNo_(0)
	, lastPostTick_(0)
	, bRecording_(false)
	, bUsingLogfileDriver_(false)
	, bSetStreamCallback_(false)
	, bResyncComment_(false)
	, currentLogfileJK_(-1)
	, hLogfile_(INVALID_HANDLE_VALUE)
	, hLogfileLock_(INVALID_HANDLE_VALUE)
	, currentReadLogfileJK_(-1)
	, tmReadLogText_(0)
	, readLogfileTick_(0)
	, pcr_(0)
	, pcrTick_(0)
	, pcrPid_(-1)
	, bSpecFile_(false)
	, dropFileTimeout_(0)
{
	szIniFileName_[0] = TEXT('\0');
	cookie_[0] = '\0';
	jkLeaveThreadID_[0] = '\0';
	commentServerResponse_[0] = '\0';
	getflvUserID_[0] = '\0';
	lastPostComm_[0] = TEXT('\0');
	readLogText_[0] = '\0';
	tmpSpecFileName_[0] = TEXT('\0');
	dropFileName_[0] = TEXT('\0');
	SETTINGS s = {};
	s_ = s;
	// TOTを取得できていないことを表す
	ftTot_[0].dwHighDateTime = 0xFFFFFFFF;
	pcrPids_[0] = -1;
}

bool CNicoJK::GetPluginInfo(TVTest::PluginInfo *pInfo)
{
	// プラグインの情報を返す
	pInfo->Type           = TVTest::PLUGIN_TYPE_NORMAL;
	pInfo->Flags          = 0;
	pInfo->pszPluginName  = L"NicoJK";
	pInfo->pszCopyright   = L"Public Domain";
	pInfo->pszDescription = L"ニコニコ実況をSDKで表示";
	return true;
}

bool CNicoJK::Initialize()
{
	// ウィンドウクラスを登録
	WNDCLASSEX wcPanel = {};
	wcPanel.cbSize = sizeof(wcPanel);
	wcPanel.style = 0;
	wcPanel.lpfnWndProc = PanelWindowProc;
	wcPanel.hInstance = g_hinstDLL;
	wcPanel.lpszClassName = TEXT("ru.jk.panel");
	if (RegisterClassEx(&wcPanel) == 0) {
		return false;
	}
	WNDCLASSEX wc = {};
	wc.cbSize = sizeof(wc);
	wc.style = CS_VREDRAW | CS_HREDRAW;
	wc.lpfnWndProc = ForceWindowProc;
	wc.hInstance = g_hinstDLL;
	wc.hbrBackground = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
	wc.lpszClassName = TEXT("ru.jk.force");
	if (RegisterClassEx(&wc) == 0) {
		return false;
	}
	// 初期化処理
	if (!GetLongModuleFileName(g_hinstDLL, szIniFileName_, _countof(szIniFileName_)) ||
	    !PathRenameExtension(szIniFileName_, TEXT(".ini"))) {
		szIniFileName_[0] = TEXT('\0');
	}
	tmpSpecFileName_[0] = TEXT('\0');
	TCHAR path[MAX_PATH + 32];
	if (GetLongModuleFileName(g_hinstDLL, path, MAX_PATH)) {
		PathRemoveExtension(path);
		wsprintf(&path[lstrlen(path)], TEXT("_%u.tmp"), GetCurrentProcessId());
		if (lstrlen(path) < _countof(tmpSpecFileName_)) {
			lstrcpy(tmpSpecFileName_, path);
		}
	}
	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 0), &wsaData) != 0) {
		return false;
	}
	// OsdCompositorは他プラグインと共用することがあるので、有効にするならFinalize()まで破棄しない
	bool bEnableOsdCompositor = GetPrivateProfileInt(TEXT("Setting"), TEXT("enableOsdCompositor"), 0, szIniFileName_) != 0;
	// フィルタグラフを取得できないバージョンではAPIフックを使う
	bool bSetHookOsdCompositor = m_pApp->GetVersion() < TVTest::MakeVersion(0, 9, 0);
	if (!commentWindow_.Initialize(g_hinstDLL, &bEnableOsdCompositor, bSetHookOsdCompositor)) {
		WSACleanup();
		return false;
	}
	if (bEnableOsdCompositor) {
		m_pApp->AddLog(L"OsdCompositorを初期化しました。");
	}
	int dpi = m_pApp->GetDPIFromWindow(m_pApp->GetAppWindow());
	int iconWidth = 16 * dpi / 96;
	int iconHeight = 16 * dpi / 96;
	m_pApp->GetStyleValuePixels(L"side-bar.item.icon.width", dpi, &iconWidth);
	m_pApp->GetStyleValuePixels(L"side-bar.item.icon.height", dpi, &iconHeight);
	bool bSmallIcon = iconWidth <= 16 && iconHeight <= 16;
	// アイコンを登録
	m_pApp->RegisterPluginIconFromResource(g_hinstDLL, MAKEINTRESOURCE(IDB_ICON));

	// パネル項目を登録
	s_.bUsePanel = GetPrivateProfileInt(TEXT("Setting"), TEXT("usePanel"), 1, szIniFileName_) != 0;
	if (s_.bUsePanel) {
		TVTest::PanelItemInfo pi;
		pi.Size = sizeof(pi);
		pi.Flags = 0;
		pi.Style = TVTest::PANEL_ITEM_STYLE_NEEDFOCUS;
		pi.ID = 1;
		pi.pszIDText = L"NicoJK";
		pi.pszTitle = L"NicoJK";
		pi.hbmIcon = static_cast<HBITMAP>(LoadImage(g_hinstDLL, MAKEINTRESOURCE(IDB_ICON), IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION));
		s_.bUsePanel = m_pApp->RegisterPanelItem(&pi);
		DeleteObject(pi.hbmIcon);
	}
	// コマンドを登録
	TVTest::PluginCommandInfo ci;
	ci.Size = sizeof(ci);
	ci.Flags = TVTest::PLUGIN_COMMAND_FLAG_ICONIZE;
	ci.State = TVTest::PLUGIN_COMMAND_STATE_DISABLED;

	ci.ID = COMMAND_HIDE_FORCE;
	ci.pszText = L"HideForce";
	ci.pszDescription = ci.pszName = L"勢いウィンドウの表示切替";
	ci.hbmIcon = static_cast<HBITMAP>(LoadImage(g_hinstDLL, MAKEINTRESOURCE(bSmallIcon ? IDB_FORCE16 : IDB_FORCE), IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION));
	if (!s_.bUsePanel && !m_pApp->RegisterPluginCommand(&ci)) {
		m_pApp->RegisterCommand(ci.ID, ci.pszText, ci.pszName);
	}
	DeleteObject(ci.hbmIcon);

	ci.ID = COMMAND_HIDE_COMMENT;
	ci.pszText = L"HideComment";
	ci.pszDescription = ci.pszName = L"実況コメントの表示切替";
	ci.hbmIcon = static_cast<HBITMAP>(LoadImage(g_hinstDLL, MAKEINTRESOURCE(bSmallIcon ? IDB_COMMENT16 : IDB_COMMENT), IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION));
	if (!m_pApp->RegisterPluginCommand(&ci)) {
		m_pApp->RegisterCommand(ci.ID, ci.pszText, ci.pszName);
	}
	DeleteObject(ci.hbmIcon);

	memset(s_.forwardList, 0, sizeof(s_.forwardList));
	for (int i = 0; i < _countof(s_.forwardList); ++i) {
		TCHAR key[16], name[32];
		wsprintf(key, TEXT("Forward%c"), TEXT('A') + i);
		wsprintf(name, TEXT("実況コメントの前進:%c"), TEXT('A') + i);
		if ((s_.forwardList[i] = GetPrivateProfileInt(TEXT("Setting"), key, INT_MAX, szIniFileName_)) == INT_MAX) {
			break;
		}
		m_pApp->RegisterCommand(COMMAND_FORWARD_A + i, key, name);
	}
	// イベントコールバック関数を登録
	m_pApp->SetEventCallback(EventCallback, this);
	return true;
}

bool CNicoJK::Finalize()
{
	// 終了処理
	TogglePlugin(false);
	// パネルウィンドウを破棄
	if (hPanel_) {
		DestroyWindow(hPanel_);
	}
	// 本体や他プラグインとの干渉を防ぐため、一旦有効にしたD&Dは最後まで維持する
	if (bDragAcceptFiles_) {
		DragAcceptFiles(m_pApp->GetAppWindow(), FALSE);
		bDragAcceptFiles_ = false;
	}
	commentWindow_.Finalize();
	WSACleanup();
	return true;
}

bool CNicoJK::TogglePlugin(bool bEnabled)
{
	if (bEnabled) {
		if ((!s_.bUsePanel || hPanel_) && !hForce_) {
			LoadFromIni();
			// ネットワーク未接続でもログフォルダにあるチャンネルを勢い窓に表示できるようにするため
			forceList_.clear();
			if (s_.logfileFolder[0]) {
				TCHAR pattern[_countof(s_.logfileFolder) + 64];
				wsprintf(pattern, TEXT("%s\\jk*"), s_.logfileFolder);
				EnumFindFile(pattern, [this](const WIN32_FIND_DATA &fd) {
					FORCE_ELEM e;
					if (!StrCmpNI(fd.cFileName, TEXT("jk"), 2) && (e.jkID = StrToInt(&fd.cFileName[2])) > 0) {
						// とりあえず組み込みのチャンネル名を設定しておく
						JKID_NAME_ELEM f;
						f.jkID = e.jkID;
						f.name = TEXT("");
						const JKID_NAME_ELEM *p = std::lower_bound(
							DEFAULT_JKID_NAME_TABLE, &DEFAULT_JKID_NAME_TABLE[_countof(DEFAULT_JKID_NAME_TABLE)], f,
							[](const JKID_NAME_ELEM &a, const JKID_NAME_ELEM &b) { return a.jkID < b.jkID; });
						if (p && p->jkID == f.jkID) {
							f.name = p->name;
						}
						lstrcpyn(e.name, f.name, _countof(e.name) - 1);
						// 今後チャンネル移動などあるかもしれないので確実ではないことを示す"?"
						lstrcat(e.name, TEXT("?"));
						e.force = 0;
						forceList_.insert(std::lower_bound(forceList_.begin(), forceList_.end(), e,
							[](const FORCE_ELEM &a, const FORCE_ELEM &b) { return a.jkID < b.jkID; }), e);
					}
				});
			}
			// 必要ならサーバに渡すCookieを取得
			cookie_[0] = '\0';
			TCHAR currDir[MAX_PATH];
			if (s_.execGetCookie == TEXT("cmd /c echo ;")) {
				lstrcpyA(cookie_, ";");
			} else if (!s_.execGetCookie.empty() && GetLongModuleFileName(nullptr, currDir, _countof(currDir)) && PathRemoveFileSpec(currDir)) {
				if (!GetProcessOutput(s_.execGetCookie.c_str(), currDir, cookie_, _countof(cookie_), 10000)) {
					cookie_[0] = '\0';
					m_pApp->AddLog(L"execGetCookieの実行に失敗しました。", TVTest::LOG_TYPE_ERROR);
				} else {
					// 改行->';'
					StrTrimA(cookie_, " \t\n\r");
					std::string strCookie;
					for (char *p = cookie_; *p; ) {
						char *q = p + StrCSpnA(p, "=\r\n");
						char *r = q + StrCSpnA(q, "\r\n");
						strCookie.append(p, q);
						if (q[0] == '=' && (q[1] == 'X' || q[1] == 'x') && q[2] == '\'') {
							// 値がX'で始まるときはDPAPIでプロテクトされたBLOBとみなす
							strCookie += '=';
							strCookie += UnprotectDpapiToString(&q[3]);
						} else {
							strCookie.append(q, r);
						}
						strCookie += ';';
						if (*(p = r + StrCSpnA(r, "\n")) != '\0') ++p;
					}
					lstrcpynA(cookie_, strCookie.c_str(), _countof(cookie_));
				}
			}
			// 破棄のタイミングがややこしいので勢い窓のフォントはここで作る
			if (!hForceFont_) {
				LOGFONT lf = {};
				HDC hdc = GetDC(nullptr);
				lf.lfHeight = -(s_.forceFontSize * GetDeviceCaps(hdc, LOGPIXELSY) / 72);
				ReleaseDC(nullptr, hdc);
				lf.lfCharSet = SHIFTJIS_CHARSET;
				lstrcpy(lf.lfFaceName, s_.forceFontName);
				hForceFont_ = CreateFontIndirect(&lf);
			}

			// 勢い窓作成
			if (hPanel_) {
				hForce_ = CreateWindowEx(0, TEXT("ru.jk.force"), TEXT("NicoJK - ニコニコ実況勢い"),
				                         WS_CHILD, 0, 0, 320, 240, hPanel_, nullptr, g_hinstDLL, this);
			} else {
				hForce_ = CreateWindowEx(WS_EX_WINDOWEDGE | WS_EX_TOOLWINDOW, TEXT("ru.jk.force"), TEXT("NicoJK - ニコニコ実況勢い"),
				                         WS_CAPTION | WS_POPUP | WS_THICKFRAME | WS_SYSMENU,
				                         CW_USEDEFAULT, CW_USEDEFAULT, 320, 240, nullptr, nullptr, g_hinstDLL, this);
			}
			if (hForce_) {
				// ウィンドウコールバック関数を登録
				m_pApp->SetWindowMessageCallback(WindowMsgCallback, this);
				// ストリームコールバック関数を登録(指定ファイル再生機能のために常に登録)
				ToggleStreamCallback(true);
				// DWMの更新タイミングでTIMER_FORWARDを呼ぶスレッドを開始(Vista以降)
				if (s_.timerInterval < 0) {
					OSVERSIONINFO vi;
					vi.dwOSVersionInfoSize = sizeof(vi);
					BOOL bCompEnabled;
					// ここで"dwmapi.dll"を遅延読み込みしていることに注意(つまりXPではDwm*()を踏んではいけない)
					if (GetVersionEx(&vi) && vi.dwMajorVersion >= 6 && SUCCEEDED(DwmIsCompositionEnabled(&bCompEnabled)) && bCompEnabled) {
						bQuitSyncThread_ = false;
						hSyncThread_ = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, SyncThread, this, 0, nullptr));
						if (hSyncThread_) {
							SetThreadPriority(hSyncThread_, THREAD_PRIORITY_ABOVE_NORMAL);
						}
					}
					if (!hSyncThread_) {
						m_pApp->AddLog(L"Aeroが無効のため設定timerIntervalのリフレッシュ同期機能はオフになります。");
						SetTimer(hForce_, TIMER_FORWARD, 166667 / -s_.timerInterval, nullptr);
					}
				}
				if (s_.dropLogfileMode != 0) {
					DragAcceptFiles(m_pApp->GetAppWindow(), TRUE);
					bDragAcceptFiles_ = true;
				}
			}
		}
		return hForce_ != nullptr;
	} else {
		if (hForce_) {
			DestroyWindow(hForce_);
		}
		if (hForceFont_) {
			DeleteFont(hForceFont_);
			hForceFont_ = nullptr;
		}
		return true;
	}
}

unsigned int __stdcall CNicoJK::SyncThread(void *pParam)
{
	CNicoJK *pThis = static_cast<CNicoJK*>(pParam);
	DWORD count = 0;
	int timeout = 0;
	while (!pThis->bQuitSyncThread_) {
		if (FAILED(DwmFlush())) {
			// ビジーに陥らないように
			Sleep(500);
		}
		if (count >= 10000) {
			// 捌き切れない量のメッセージを送らない
			if (pThis->bPendingTimerForward_ && --timeout >= 0) {
				continue;
			}
			count -= 10000;
			timeout = 30;
			pThis->bPendingTimerForward_ = true;
			SendNotifyMessage(pThis->hForce_, WM_TIMER, TIMER_FORWARD, 0);
		}
		count += pThis->bHalfSkip_ ? -pThis->s_.timerInterval / 2 : -pThis->s_.timerInterval;
	}
	return 0;
}

void CNicoJK::ToggleStreamCallback(bool bSet)
{
	if (bSet) {
		if (!bSetStreamCallback_) {
			bSetStreamCallback_ = true;
			pcrPid_ = -1;
			pcrPids_[0] = -1;
			m_pApp->SetStreamCallback(0, StreamCallback, this);
		}
	} else {
		if (bSetStreamCallback_) {
			m_pApp->SetStreamCallback(TVTest::STREAM_CALLBACK_REMOVE, StreamCallback);
			bSetStreamCallback_ = false;
		}
	}
}

void CNicoJK::LoadFromIni()
{
	// iniはセクション単位で読むと非常に速い。起動時は処理が混み合うのでとくに有利
	std::vector<TCHAR> buf = GetPrivateProfileSectionBuffer(TEXT("Setting"), szIniFileName_);
	s_.hideForceWindow		= GetBufferedProfileInt(buf.data(), TEXT("hideForceWindow"), 0);
	s_.forceFontSize		= GetBufferedProfileInt(buf.data(), TEXT("forceFontSize"), 10);
	GetBufferedProfileString(buf.data(), TEXT("forceFontName"), TEXT("Meiryo UI"), s_.forceFontName, _countof(s_.forceFontName));
	s_.timerInterval		= GetBufferedProfileInt(buf.data(), TEXT("timerInterval"), -5000);
	s_.halfSkipThreshold	= GetBufferedProfileInt(buf.data(), TEXT("halfSkipThreshold"), 9999);
	s_.commentLineMargin	= GetBufferedProfileInt(buf.data(), TEXT("commentLineMargin"), 125);
	s_.commentFontOutline	= GetBufferedProfileInt(buf.data(), TEXT("commentFontOutline"), 0);
	s_.commentSize			= GetBufferedProfileInt(buf.data(), TEXT("commentSize"), 100);
	s_.commentSizeMin		= GetBufferedProfileInt(buf.data(), TEXT("commentSizeMin"), 16);
	s_.commentSizeMax		= GetBufferedProfileInt(buf.data(), TEXT("commentSizeMax"), 9999);
	GetBufferedProfileString(buf.data(), TEXT("commentFontName"), TEXT("ＭＳ Ｐゴシック"), s_.commentFontName, _countof(s_.commentFontName));
	GetBufferedProfileString(buf.data(), TEXT("commentFontNameMulti"), TEXT("ＭＳ Ｐゴシック"), s_.commentFontNameMulti, _countof(s_.commentFontNameMulti));
	s_.bCommentFontBold		= GetBufferedProfileInt(buf.data(), TEXT("commentFontBold"), 1) != 0;
	s_.bCommentFontAntiAlias = GetBufferedProfileInt(buf.data(), TEXT("commentFontAntiAlias"), 1) != 0;
	s_.commentDuration		= GetBufferedProfileInt(buf.data(), TEXT("commentDuration"), CCommentWindow::DISPLAY_DURATION);
	s_.commentDrawLineCount = GetBufferedProfileInt(buf.data(), TEXT("commentDrawLineCount"), CCommentWindow::DEFAULT_LINE_DRAW_COUNT);
	s_.logfileMode			= GetBufferedProfileInt(buf.data(), TEXT("logfileMode"), 0);
	TCHAR val[SETTING_VALUE_MAX];
	GetBufferedProfileString(buf.data(), TEXT("logfileDrivers"),
	                         TEXT("BonDriver_UDP.dll:BonDriver_TCP.dll:BonDriver_File.dll:BonDriver_RecTask.dll:BonDriver_Pipe.dll"),
	                         val, _countof(val));
	s_.logfileDrivers = val;
	GetBufferedProfileString(buf.data(), TEXT("nonTunerDrivers"),
	                         TEXT("BonDriver_UDP.dll:BonDriver_TCP.dll:BonDriver_File.dll:BonDriver_RecTask.dll:BonDriver_Pipe.dll"),
	                         val, _countof(val));
	s_.nonTunerDrivers = val;
	GetBufferedProfileString(buf.data(), TEXT("execGetCookie"), TEXT("cmd /c echo ;"), val, _countof(val));
	s_.execGetCookie = val;
	GetBufferedProfileString(buf.data(), TEXT("mailDecorations"),
	                         TEXT("[cyan big]:[shita]:[green shita small]:[orange]::"),
	                         val, _countof(val));
	s_.mailDecorations = val;
	s_.bAnonymity			= GetBufferedProfileInt(buf.data(), TEXT("anonymity"), 1) != 0;
	s_.bUseOsdCompositor	= GetBufferedProfileInt(buf.data(), TEXT("useOsdCompositor"), 0) != 0;
	s_.bUseTexture			= GetBufferedProfileInt(buf.data(), TEXT("useTexture"), 1) != 0;
	s_.bUseDrawingThread	= GetBufferedProfileInt(buf.data(), TEXT("useDrawingThread"), 1) != 0;
	s_.bSetChannel			= GetBufferedProfileInt(buf.data(), TEXT("setChannel"), 1) != 0;
	s_.bShowRadio			= GetBufferedProfileInt(buf.data(), TEXT("showRadio"), 0) != 0;
	s_.bDoHalfClose			= GetBufferedProfileInt(buf.data(), TEXT("doHalfClose"), 0) != 0;
	s_.maxAutoReplace		= GetBufferedProfileInt(buf.data(), TEXT("maxAutoReplace"), 20);
	GetBufferedProfileString(buf.data(), TEXT("abone"), TEXT("### NG ### &"), val, _countof(val));
	s_.abone = val;
	s_.dropLogfileMode		= GetBufferedProfileInt(buf.data(), TEXT("dropLogfileMode"), 0);
	s_.defaultPlaybackDelay	= GetBufferedProfileInt(buf.data(), TEXT("defaultPlaybackDelay"), 500);
	// 実況ログフォルダのパスを作成
	TCHAR path[MAX_PATH], dir[MAX_PATH];
	GetBufferedProfileString(buf.data(), TEXT("logfileFolder"), TEXT("Plugins\\NicoJK"), path, _countof(path));
	if (path[0] && PathIsRelative(path)) {
		if (!GetLongModuleFileName(nullptr, dir, _countof(dir)) || !PathRemoveFileSpec(dir) || !PathCombine(s_.logfileFolder, dir, path)) {
			s_.logfileFolder[0] = TEXT('\0');
		}
	} else {
		lstrcpy(s_.logfileFolder, path);
	}
	if (!PathIsDirectory(s_.logfileFolder)) {
		s_.logfileFolder[0] = TEXT('\0');
	}

	buf = GetPrivateProfileSectionBuffer(TEXT("Window"), szIniFileName_);
	s_.rcForce.left			= GetBufferedProfileInt(buf.data(), TEXT("ForceX"), 0);
	s_.rcForce.top			= GetBufferedProfileInt(buf.data(), TEXT("ForceY"), 0);
	s_.rcForce.right		= GetBufferedProfileInt(buf.data(), TEXT("ForceWidth"), 0) + s_.rcForce.left;
	s_.rcForce.bottom		= GetBufferedProfileInt(buf.data(), TEXT("ForceHeight"), 0) + s_.rcForce.top;
	s_.forceOpacity			= GetBufferedProfileInt(buf.data(), TEXT("ForceOpacity"), 255);
	s_.commentOpacity		= GetBufferedProfileInt(buf.data(), TEXT("CommentOpacity"), 255);
	s_.headerMask			= GetBufferedProfileInt(buf.data(), TEXT("HeaderMask"), 0);
	s_.bSetRelative			= GetBufferedProfileInt(buf.data(), TEXT("SetRelative"), 0) != 0;

	ntsIDList_.clear();
	ntsIDList_.reserve(_countof(DEFAULT_NTSID_TABLE));
	for (int i = 0; i < _countof(DEFAULT_NTSID_TABLE); ++i) {
		NETWORK_SERVICE_ID_ELEM e = {DEFAULT_NTSID_TABLE[i]&~0xFFF0, DEFAULT_NTSID_TABLE[i]>>4&0xFFF};
		ntsIDList_.push_back(e);
	}
	// 設定ファイルのネットワーク/サービスID-実況ID対照表を、ソートを維持しながらマージ
	buf = GetPrivateProfileSectionBuffer(TEXT("Channels"), szIniFileName_);
	for (LPCTSTR p = buf.data(); *p; p += lstrlen(p) + 1) {
		NETWORK_SERVICE_ID_ELEM e;
		bool bPrior = _stscanf_s(p, TEXT("0x%x=+%d"), &e.ntsID, &e.jkID) == 2;
		if (bPrior) {
			e.jkID |= NETWORK_SERVICE_ID_ELEM::JKID_PRIOR;
		}
		if (bPrior || _stscanf_s(p, TEXT("0x%x=%d"), &e.ntsID, &e.jkID) == 2) {
			// 設定ファイルの定義では上位と下位をひっくり返しているので補正
			e.ntsID = (e.ntsID<<16) | (e.ntsID>>16);
			std::vector<NETWORK_SERVICE_ID_ELEM>::iterator it =
				std::lower_bound(ntsIDList_.begin(), ntsIDList_.end(), e,
					[](const NETWORK_SERVICE_ID_ELEM &a, const NETWORK_SERVICE_ID_ELEM &b) { return a.ntsID < b.ntsID; });
			if (it != ntsIDList_.end() && it->ntsID == e.ntsID) {
				*it = e;
			} else {
				ntsIDList_.insert(it, e);
			}
		}
	}

	rplList_.clear();
	LoadRplListFromIni(TEXT("AutoReplace"), &rplList_);
	LoadRplListFromIni(TEXT("CustomReplace"), &rplList_);
}

void CNicoJK::SaveToIni()
{
	WritePrivateProfileInt(TEXT("Window"), TEXT("ForceX"), s_.rcForce.left, szIniFileName_);
	WritePrivateProfileInt(TEXT("Window"), TEXT("ForceY"), s_.rcForce.top, szIniFileName_);
	WritePrivateProfileInt(TEXT("Window"), TEXT("ForceWidth"), s_.rcForce.right - s_.rcForce.left, szIniFileName_);
	WritePrivateProfileInt(TEXT("Window"), TEXT("ForceHeight"), s_.rcForce.bottom - s_.rcForce.top, szIniFileName_);
	WritePrivateProfileInt(TEXT("Window"), TEXT("ForceOpacity"), s_.forceOpacity, szIniFileName_);
	WritePrivateProfileInt(TEXT("Window"), TEXT("CommentOpacity"), s_.commentOpacity, szIniFileName_);
	WritePrivateProfileInt(TEXT("Window"), TEXT("HeaderMask"), s_.headerMask, szIniFileName_);
	WritePrivateProfileInt(TEXT("Window"), TEXT("SetRelative"), s_.bSetRelative, szIniFileName_);
}

void CNicoJK::LoadRplListFromIni(LPCTSTR section, std::vector<RPL_ELEM> *pRplList)
{
	std::vector<TCHAR> buf = GetPrivateProfileSectionBuffer(section, szIniFileName_);
	size_t lastSize = pRplList->size();
	for (LPCTSTR p = buf.data(); *p; p += lstrlen(p) + 1) {
		RPL_ELEM e;
		if (!StrCmpNI(p, TEXT("Pattern"), 7) && StrToIntEx(&p[7], STIF_DEFAULT, &e.key)) {
			e.section = section;
			TCHAR key[32];
			wsprintf(key, TEXT("Comment%d"), e.key);
			TCHAR val[SETTING_VALUE_MAX];
			GetBufferedProfileString(buf.data(), key, TEXT(""), val, _countof(val));
			e.comment = val;
			wsprintf(key, TEXT("Pattern%d"), e.key);
			GetBufferedProfileString(buf.data(), key, TEXT(""), val, _countof(val));
			if (!e.SetPattern(val)) {
				TCHAR text[64];
				wsprintf(text, TEXT("%sの正規表現が異常です。"), key);
				m_pApp->AddLog(text, TVTest::LOG_TYPE_ERROR);
			} else {
				pRplList->push_back(e);
			}
		}
	}
	std::sort(pRplList->begin() + lastSize, pRplList->end(), [](const RPL_ELEM &a, const RPL_ELEM &b) { return a.key < b.key; });
}

void CNicoJK::SaveRplListToIni(LPCTSTR section, const std::vector<RPL_ELEM> &rplList, bool bClearSection)
{
	if (bClearSection) {
		WritePrivateProfileString(section, nullptr, nullptr, szIniFileName_);
	}
	for (auto it = rplList.cbegin(); it != rplList.end(); ++it) {
		if (it->section == section) {
			TCHAR key[32];
			wsprintf(key, TEXT("Pattern%d"), it->key);
			WritePrivateProfileString(section, key, it->pattern.c_str(), szIniFileName_);
		}
	}
}

HWND CNicoJK::GetFullscreenWindow()
{
	TVTest::HostInfo hostInfo;
	if (m_pApp->GetFullscreen() && m_pApp->GetHostInfo(&hostInfo)) {
		wchar_t className[64];
		lstrcpynW(className, hostInfo.pszAppName, 48);
		lstrcatW(className, L" Fullscreen");

		HWND hwnd = nullptr;
		while ((hwnd = FindWindowExW(nullptr, hwnd, className, nullptr)) != nullptr) {
			DWORD pid;
			GetWindowThreadProcessId(hwnd, &pid);
			if (pid == GetCurrentProcessId()) {
				return hwnd;
			}
		}
	}
	return nullptr;
}

static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam)
{
	void **params = reinterpret_cast<void**>(lParam);
	TCHAR className[64];
	if (GetClassName(hwnd, className, _countof(className)) && !lstrcmp(className, static_cast<LPCTSTR>(params[1]))) {
		// 見つかった
		*static_cast<HWND*>(params[0]) = hwnd;
		return FALSE;
	}
	return TRUE;
}

// TVTestのVideo Containerウィンドウを探す
HWND CNicoJK::FindVideoContainer()
{
	HWND hwndFound = nullptr;
	TVTest::HostInfo hostInfo;
	if (m_pApp->GetHostInfo(&hostInfo)) {
		TCHAR searchName[64];
		lstrcpyn(searchName, hostInfo.pszAppName, 32);
		lstrcat(searchName, L" Video Container");

		void *params[2] = { &hwndFound, searchName };
		HWND hwndFull = GetFullscreenWindow();
		EnumChildWindows(hwndFull ? hwndFull : m_pApp->GetAppWindow(), EnumWindowsProc, reinterpret_cast<LPARAM>(params));
	}
	return hwndFound;
}

// 再生中のストリームのネットワーク/サービスIDを取得する
DWORD CNicoJK::GetCurrentNetworkServiceID()
{
	TVTest::ServiceInfo si;
	int index = m_pApp->GetService();
	if (index >= 0 && m_pApp->GetServiceInfo(index, &si)) {
		TVTest::ChannelInfo ci;
		if (m_pApp->GetCurrentChannelInfo(&ci) && ci.NetworkID) {
			if (0x7880 <= ci.NetworkID && ci.NetworkID <= 0x7FEF) {
				// 地上波のサービス種別とサービス番号はマスクする
				return (static_cast<DWORD>(si.ServiceID&~0x0187) << 16) | 0x000F;
			}
			return (static_cast<DWORD>(si.ServiceID) << 16) | ci.NetworkID;
		}
		// チャンネルスキャンしていないとGetCurrentChannelInfo()もネットワークIDの取得に失敗するよう
		if (si.ServiceID >= 0x0400) {
			// 地上波っぽいのでマスクする
			return (static_cast<DWORD>(si.ServiceID&~0x0187) << 16) | 0;
		}
		return (static_cast<DWORD>(si.ServiceID) << 16) | 0;
	}
	return 0;
}

// 指定チャンネルのネットワーク/サービスIDを取得する
bool CNicoJK::GetChannelNetworkServiceID(int tuningSpace, int channelIndex, DWORD *pNtsID)
{
	TVTest::ChannelInfo ci;
	if (m_pApp->GetChannelInfo(tuningSpace, channelIndex, &ci)) {
		if (ci.NetworkID && ci.ServiceID) {
			if (0x7880 <= ci.NetworkID && ci.NetworkID <= 0x7FEF) {
				// 地上波のサービス種別とサービス番号はマスクする
				*pNtsID = (static_cast<DWORD>(ci.ServiceID&~0x0187) << 16) | 0x000F;
				return true;
			}
			*pNtsID = (static_cast<DWORD>(ci.ServiceID) << 16) | ci.NetworkID;
			return true;
		}
		*pNtsID = 0;
		return true;
	}
	return false;
}

// 再生中のストリームのTOT時刻(取得からの経過時間で補正済み)をUTCで取得する
bool CNicoJK::GetCurrentTot(FILETIME *pft)
{
	CBlockLock lock(&streamLock_);
	DWORD tick = GetTickCount();
	if (ftTot_[0].dwHighDateTime == 0xFFFFFFFF) {
		// TOTを取得できていない
		return false;
	} else if (tick - pcrTick_ >= 2000) {
		// 2秒以上PCRを取得できていない→ポーズ中?
		*pft = ftTot_[0];
		*pft += -s_.defaultPlaybackDelay * FILETIME_MILLISECOND;
		return true;
	} else if (ftTot_[1].dwHighDateTime == 0xFFFFFFFF) {
		// 再生速度は分からない
		*pft = ftTot_[0];
		*pft += ((int)(tick - totTick_[0]) - s_.defaultPlaybackDelay) * FILETIME_MILLISECOND;
		return true;
	} else {
		DWORD delta = totTick_[0] - totTick_[1];
		// 再生速度(10%～1000%)
		LONGLONG speed = !delta ? FILETIME_MILLISECOND : (ftTot_[0] - ftTot_[1]) / delta;
		speed = min(max(speed, FILETIME_MILLISECOND / 10), FILETIME_MILLISECOND * 10);
		*pft = ftTot_[0];
		*pft += ((int)(tick - totTick_[0]) - s_.defaultPlaybackDelay) * speed;
		return true;
	}
}

// 現在のBonDriverが':'区切りのリストに含まれるかどうか調べる
bool CNicoJK::IsMatchDriverName(LPCTSTR drivers)
{
	TCHAR path[MAX_PATH];
	m_pApp->GetDriverName(path, _countof(path));
	LPCTSTR name = PathFindFileName(path);
	int len = lstrlen(name);
	if (len > 0) {
		for (LPCTSTR p = drivers; (p = StrStrI(p, name)) != nullptr; p += len) {
			if ((p == drivers || p[-1] == TEXT(':')) && (p[len] == TEXT('\0') || p[len] == TEXT(':'))) {
				return true;
			}
		}
	}
	return false;
}

// 指定した実況IDのログファイルに書き込む
// jkIDが負値のときはログファイルを閉じる
void CNicoJK::WriteToLogfile(int jkID, const char *text)
{
	if (!s_.logfileFolder[0] || s_.logfileMode == 0 || s_.logfileMode == 1 && !bRecording_) {
		// ログを記録しない
		jkID = -1;
	}
	if (currentLogfileJK_ >= 0 && currentLogfileJK_ != jkID) {
		// 閉じる
		CloseHandle(hLogfile_);
		CloseHandle(hLogfileLock_);
		// ロックファイルを削除
		TCHAR lockPath[_countof(s_.logfileFolder) + 32];
		wsprintf(lockPath, TEXT("%s\\jk%d\\lockfile"), s_.logfileFolder, currentLogfileJK_);
		DeleteFile(lockPath);
		currentLogfileJK_ = -1;
		OutputMessageLog(TEXT("ログファイルの書き込みを終了しました。"));
	}
	if (currentLogfileJK_ < 0 && jkID >= 0) {
		unsigned int tm;
		TCHAR dir[_countof(s_.logfileFolder) + 32];
		wsprintf(dir, TEXT("%s\\jk%d"), s_.logfileFolder, jkID);
		if (GetChatDate(&tm, text) && (PathFileExists(dir) || CreateDirectory(dir, nullptr))) {
			// ロックファイルを開く
			TCHAR lockPath[_countof(dir) + 32];
			wsprintf(lockPath, TEXT("%s\\lockfile"), dir);
			hLogfileLock_ = CreateFile(lockPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (hLogfileLock_ != INVALID_HANDLE_VALUE) {
				// 開く
				TCHAR path[_countof(dir) + 32];
				wsprintf(path, TEXT("%s\\%010u.txt"), dir, tm);
				hLogfile_ = CreateFile(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
				if (hLogfile_ != INVALID_HANDLE_VALUE) {
					// ヘッダを書き込む(別に無くてもいい)
					FILETIME ftUtc, ft;
					UnixTimeToFileTime(tm, &ftUtc);
					FileTimeToLocalFileTime(&ftUtc, &ft);
					SYSTEMTIME st;
					FileTimeToSystemTime(&ft, &st);
					char header[128];
					int len = wsprintfA(header, "<!-- NicoJK logfile from %04d-%02d-%02dT%02d:%02d:%02d -->\r\n",
					                    st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
					DWORD written;
					WriteFile(hLogfile_, header, len, &written, nullptr);
					currentLogfileJK_ = jkID;

					TCHAR debug[_countof(path) + 64];
					wsprintf(debug, TEXT("ログファイル\"%s\"の書き込みを開始しました。"),
					         StrRChr(path, StrRChr(path, nullptr, TEXT('\\')), TEXT('\\')) + 1);
					OutputMessageLog(debug);
				} else {
					CloseHandle(hLogfileLock_);
					DeleteFile(lockPath);
				}
			}
		}
	}
	// 開いてたら書き込む
	if (currentLogfileJK_ >= 0) {
		DWORD written;
		WriteFile(hLogfile_, text, lstrlenA(text), &written, nullptr);
		WriteFile(hLogfile_, "\r\n", 2, &written, nullptr);
	}
}

#define DWORD_MSB(x) ((x) & 0x80000000)

// 指定した実況IDの指定時刻のログ1行を読み込む
// jkIDが負値のときはログファイルを閉じる
// jkID==0は指定ファイル再生(tmpSpecFileName_)を表す特殊な実況IDとする
bool CNicoJK::ReadFromLogfile(int jkID, char *text, int textMax, unsigned int tmToRead)
{
	if (jkID != 0 && (!s_.logfileFolder[0] || !bUsingLogfileDriver_)) {
		// ログを読まない
		jkID = -1;
	}
	DWORD tick = GetTickCount();
	if (currentReadLogfileJK_ >= 0 && currentReadLogfileJK_ != jkID) {
		// 閉じる
		readLogfile_.Close();
		readLogfileTick_ = tick;
		currentReadLogfileJK_ = -1;
		OutputMessageLog(TEXT("ログファイルの読み込みを終了しました。"));
	}
	if (!DWORD_MSB(tick - readLogfileTick_) && currentReadLogfileJK_ < 0 && jkID >= 0) {
		// ファイルチェックを大量に繰りかえすのを防ぐ
		readLogfileTick_ = tick + READ_LOG_FOLDER_INTERVAL;
		TCHAR path[_countof(s_.logfileFolder) + 64];
		path[0] = TEXT('\0');
		if (jkID == 0) {
			// 指定ファイル再生
			lstrcpyn(path, tmpSpecFileName_, _countof(path));
		} else {
			// jkIDのログファイル一覧を得る
			TCHAR pattern[_countof(s_.logfileFolder) + 64];
			wsprintf(pattern, TEXT("%s\\jk%d\\??????????.txt"), s_.logfileFolder, jkID);
			// tmToRead以前でもっとも新しいログファイルを探す
			TCHAR target[64];
			wsprintf(target, TEXT("%010u.txt"), tmToRead + (READ_LOG_FOLDER_INTERVAL / 1000 + 2));
			TCHAR latest[16] = {};
			EnumFindFile(pattern, [&target, &latest](const WIN32_FIND_DATA &fd) {
				if (lstrcmpi(fd.cFileName, target) < 0 && (!latest[0] || lstrcmpi(fd.cFileName, latest) > 0) && lstrlen(fd.cFileName) == 14) {
					lstrcpy(latest, fd.cFileName);
				}
			});
			if (latest[0]) {
				// 見つかった
				wsprintf(path, TEXT("%s\\jk%d\\%s"), s_.logfileFolder, jkID, latest);
			}
		}
		if (path[0]) {
			if (readLogfile_.Open(path, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN)) {
				char last[CHAT_TAG_MAX];
				unsigned int tmLast;
				// 最終行がtmToReadより過去なら読む価値無し
				if (!readLogfile_.ReadLastLine(last, _countof(last)) || !GetChatDate(&tmLast, last) || tmLast < tmToRead) {
					// 閉じる
					readLogfile_.Close();
				} else {
					// まず2分探索
					for (int scale = 2; ; scale *= 2) {
						char middle[CHAT_TAG_MAX];
						int sign = 0;
						for (;;) {
							if (!readLogfile_.ReadLine(middle, _countof(middle))) {
								break;
							}
							unsigned int tmMiddle;
							if (GetChatDate(&tmMiddle, middle)) {
								sign = tmMiddle + 10 > tmToRead ? -1 : 1;
								break;
							}
						}
						// 行の時刻が得られないか最初の行がすでに未来ならリセット
						if (sign == 0 || sign < 0 && scale == 2) {
							readLogfile_.ResetPointer();
							break;
						}
						int moveSize = readLogfile_.Seek(sign * scale);
						dprintf(TEXT("CNicoJK::ReadFromLogfile() moveSize=%d\n"), moveSize); // DEBUG
						// 移動量が小さくなれば打ち切り
						if (-32 * 1024 < moveSize && moveSize < 32 * 1024) {
							// tmToReadよりも確実に過去になる位置まで戻す
							readLogfile_.Seek(-scale);
							// シーク直後の中途半端な1行を読み飛ばす
							readLogfile_.ReadLine(middle, 1);
							break;
						}
						readLogfile_.ReadLine(middle, 1);
					}
					// tmToReadより過去の行を読み飛ばす
					for (;;) {
						if (!readLogfile_.ReadLine(readLogText_, _countof(readLogText_))) {
							// 閉じる
							readLogfile_.Close();
							break;
						} else if (GetChatDate(&tmReadLogText_, readLogText_) && tmReadLogText_ > tmToRead/*>=はダメ*/) {
							currentReadLogfileJK_ = jkID;

							TCHAR debug[_countof(path) + 64];
							wsprintf(debug, TEXT("ログファイル\"jk%d\\%s\"の読み込みを開始しました。"), jkID, PathFindFileName(path));
							OutputMessageLog(debug);
							break;
						}
					}
				}
			}
		}
	}
	bool bRet = false;
	// 開いてたら読み込む
	if (currentReadLogfileJK_ >= 0) {
		if (readLogText_[0] && tmReadLogText_ <= tmToRead) {
			lstrcpynA(text, readLogText_, textMax);
			readLogText_[0] = '\0';
			bRet = true;
		}
		if (!readLogText_[0]) {
			for (;;) {
				if (!readLogfile_.ReadLine(readLogText_, _countof(readLogText_))) {
					// 閉じる
					readLogfile_.Close();
					readLogfileTick_ = tick;
					currentReadLogfileJK_ = -1;
					OutputMessageLog(TEXT("ログファイルの読み込みを終了しました。"));
					break;
				} else if (GetChatDate(&tmReadLogText_, readLogText_)) {
					break;
				}
			}
		}
	}
	return bRet;
}

static int GetWindowHeight(HWND hwnd)
{
	RECT rc;
	return hwnd && GetWindowRect(hwnd, &rc) ? rc.bottom - rc.top : 0;
}

// イベントコールバック関数
// 何かイベントが起きると呼ばれる
LRESULT CALLBACK CNicoJK::EventCallback(UINT Event, LPARAM lParam1, LPARAM lParam2, void *pClientData)
{
	CNicoJK *pThis = static_cast<CNicoJK*>(pClientData);
	switch (Event) {
	case TVTest::EVENT_PLUGINENABLE:
		// プラグインの有効状態が変化した
		{
			pThis->TogglePlugin(lParam1 != 0);
			// パネルの有効状態は(hForce_の有無にかかわらず)常にTVTestのプラグイン状態フラグに合わせる
			TVTest::PanelItemSetInfo psi;
			psi.Size = sizeof(psi);
			psi.Mask = TVTest::PANEL_ITEM_SET_INFO_MASK_STATE;
			psi.ID = 1;
			psi.StateMask = TVTest::PANEL_ITEM_STATE_ENABLED | (pThis->hPanel_ ? TVTest::PANEL_ITEM_STATE_ACTIVE : 0);
			psi.State = lParam1 ? psi.StateMask : 0;
			pThis->m_pApp->SetPanelItem(&psi);
		}
		return TRUE;
	case TVTest::EVENT_PANELITEM_NOTIFY:
		// パネル項目の通知
		{
			TVTest::PanelItemEventInfo *pei = reinterpret_cast<TVTest::PanelItemEventInfo*>(lParam1);
			switch (pei->Event) {
			case TVTest::PANEL_ITEM_EVENT_CREATE:
				{
					TVTest::PanelItemCreateEventInfo *pcei = reinterpret_cast<TVTest::PanelItemCreateEventInfo*>(lParam1);
					pThis->hPanel_ = CreateWindowEx(0, TEXT("ru.jk.panel"), nullptr, WS_CHILD | WS_VISIBLE,
					                                pcei->ItemRect.left, pcei->ItemRect.top,
					                                pcei->ItemRect.right - pcei->ItemRect.left, pcei->ItemRect.bottom - pcei->ItemRect.top,
					                                pcei->hwndParent, nullptr, g_hinstDLL, pThis);
					if (pThis->hPanel_) {
						pcei->hwndItem = pThis->hPanel_;
						// このイベントはTVTest::EVENT_PLUGINENABLEよりも遅れるため
						pThis->TogglePlugin(pThis->m_pApp->IsPluginEnabled());
						return TRUE;
					}
				}
				return FALSE;
			}
		}
		break;
	case TVTest::EVENT_COLORCHANGE:
		// 色の設定が変化した
		if (pThis->hPanel_ && pThis->hForce_) {
			DeleteBrush(SetClassLongPtr(pThis->hForce_, GCLP_HBRBACKGROUND,
				reinterpret_cast<LONG_PTR>(CreateSolidBrush(pThis->m_pApp->GetColor(L"ControlPanelMargin")))));
			InvalidateRect(pThis->hForce_, nullptr, TRUE);
		}
		break;
	case TVTest::EVENT_RECORDSTATUSCHANGE:
		// 録画状態が変化した
		pThis->bRecording_ = lParam1 != TVTest::RECORD_STATUS_NOTRECORDING;
		break;
	case TVTest::EVENT_FULLSCREENCHANGE:
		// 全画面表示状態が変化した
		if (pThis->hForce_) {
			// オーナーが変わるのでコメントウィンドウを作りなおす
			pThis->commentWindow_.Destroy();
			if (pThis->commentWindow_.GetOpacity() != 0 && pThis->m_pApp->GetPreview()) {
				HWND hwnd = pThis->FindVideoContainer();
				pThis->commentWindow_.Create(hwnd);
				pThis->bHalfSkip_ = GetWindowHeight(hwnd) >= pThis->s_.halfSkipThreshold;
			}
			// 全画面遷移時は隠れたほうが使い勝手がいいので呼ばない
			if (!lParam1) {
				SendMessage(pThis->hForce_, WM_SET_ZORDER, 0, 0);
			}
		}
		break;
	case TVTest::EVENT_PREVIEWCHANGE:
		// プレビュー表示状態が変化した
		if (pThis->hForce_) {
			if (pThis->commentWindow_.GetOpacity() != 0 && lParam1 != 0) {
				HWND hwnd = pThis->FindVideoContainer();
				pThis->commentWindow_.Create(hwnd);
				pThis->bHalfSkip_ = GetWindowHeight(hwnd) >= pThis->s_.halfSkipThreshold;
				pThis->ProcessChatTag("<!--<chat date=\"0\" mail=\"cyan ue\" user_id=\"-\">(NicoJK ON)</chat>-->");
			} else {
				pThis->commentWindow_.Destroy();
			}
		}
		break;
	case TVTest::EVENT_DRIVERCHANGE:
		// ドライバが変更された
		if (pThis->hForce_) {
			pThis->bUsingLogfileDriver_ = pThis->IsMatchDriverName(pThis->s_.logfileDrivers.c_str());
		}
		// FALL THROUGH!
	case TVTest::EVENT_CHANNELCHANGE:
		// チャンネルが変更された
		if (pThis->hForce_) {
			PostMessage(pThis->hForce_, WM_RESET_STREAM, 0, 0);
		}
		// FALL THROUGH!
	case TVTest::EVENT_SERVICECHANGE:
		// サービスが変更された
		if (pThis->hForce_) {
			// 重複やザッピング対策のためタイマで呼ぶ
			SetTimer(pThis->hForce_, TIMER_SETUP_CURJK, SETUP_CURJK_DELAY, nullptr);
		}
		break;
	case TVTest::EVENT_SERVICEUPDATE:
		// サービスの構成が変化した(再生ファイルを切り替えたときなど)
		if (pThis->hForce_) {
			// ユーザの自発的なチャンネル変更(EVENT_CHANNELCHANGE)を捉えるのが原則だが
			// 非チューナ系のBonDriverだとこれでは不十分なため
			if (pThis->IsMatchDriverName(pThis->s_.nonTunerDrivers.c_str())) {
				SetTimer(pThis->hForce_, TIMER_SETUP_CURJK, SETUP_CURJK_DELAY, nullptr);
			}
		}
		break;
	case TVTest::EVENT_COMMAND:
		// コマンドが選択された
		if (pThis->hForce_) {
			switch (lParam1) {
			case COMMAND_HIDE_FORCE:
				if (IsWindowVisible(pThis->hForce_)) {
					ShowWindow(pThis->hForce_, SW_HIDE);
				} else {
					ShowWindow(pThis->hForce_, SW_SHOWNA);
				}
				SendMessage(pThis->hForce_, WM_UPDATE_LIST, TRUE, 0);
				SendMessage(pThis->hForce_, WM_SET_ZORDER, 0, 0);
				PostMessage(pThis->hForce_, WM_TIMER, TIMER_UPDATE, 0);
				break;
			case COMMAND_HIDE_COMMENT:
					if (pThis->commentWindow_.GetOpacity() == 0 && pThis->m_pApp->GetPreview()) {
						pThis->commentWindow_.ClearChat();
						HWND hwnd = pThis->FindVideoContainer();
						pThis->commentWindow_.Create(hwnd);
						pThis->bHalfSkip_ = GetWindowHeight(hwnd) >= pThis->s_.halfSkipThreshold;
						pThis->commentWindow_.AddChat(TEXT("(Comment ON)"), RGB(0x00,0xFF,0xFF), CCommentWindow::CHAT_POS_UE);
						// 非表示前の不透明度を復元する
						BYTE newOpacity = static_cast<BYTE>(pThis->s_.commentOpacity>>8);
						pThis->commentWindow_.SetOpacity(newOpacity == 0 ? 255 : newOpacity);
						pThis->m_pApp->SetPluginCommandState(COMMAND_HIDE_COMMENT, TVTest::COMMAND_ICON_STATE_CHECKED);
					} else {
						pThis->commentWindow_.Destroy();
						// 8-15bitに非表示前の不透明度を記憶しておく
						pThis->s_.commentOpacity = (pThis->s_.commentOpacity&~0xFF00) | (pThis->commentWindow_.GetOpacity()<<8);
						pThis->commentWindow_.SetOpacity(0);
						pThis->m_pApp->SetPluginCommandState(COMMAND_HIDE_COMMENT, 0);
					}
					SendDlgItemMessage(pThis->hForce_, IDC_SLIDER_OPACITY, TBM_SETPOS, TRUE, (pThis->commentWindow_.GetOpacity() * 10 + 254) / 255);
				break;
			default:
				if (COMMAND_FORWARD_A <= lParam1 && lParam1 < COMMAND_FORWARD_A + _countof(pThis->s_.forwardList)) {
					int forward = pThis->s_.forwardList[lParam1 - COMMAND_FORWARD_A];
					if (forward == 0) {
						pThis->forwardOffsetDelta_ = -pThis->forwardOffset_;
					} else {
						pThis->forwardOffsetDelta_ += forward;
					}
				}
				break;
			}
		}
		break;
	case TVTest::EVENT_FILTERGRAPH_INITIALIZED:
		// フィルタグラフの初期化終了
		pThis->commentWindow_.OnFilterGraphInitialized(reinterpret_cast<const TVTest::FilterGraphInfo*>(lParam1)->pGraphBuilder);
		break;
	case TVTest::EVENT_FILTERGRAPH_FINALIZE:
		// フィルタグラフの終了処理開始
		pThis->commentWindow_.OnFilterGraphFinalize(reinterpret_cast<const TVTest::FilterGraphInfo*>(lParam1)->pGraphBuilder);
		break;
	}
	return 0;
}

BOOL CALLBACK CNicoJK::WindowMsgCallback(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT *pResult, void *pUserData)
{
	CNicoJK *pThis = static_cast<CNicoJK*>(pUserData);
	switch (uMsg) {
	case WM_ACTIVATE:
		if (LOWORD(wParam) != WA_INACTIVE) {
			SendMessage(pThis->hForce_, WM_SET_ZORDER, 0, 0);
		}
		break;
	case WM_WINDOWPOSCHANGED:
		// WM_ACTIVATEされないZオーダーの変化を捉える。フルスクリーンでもなぜか送られてくるので注意
		SetTimer(pThis->hForce_, TIMER_DONE_POSCHANGE, 1000, nullptr);
		break;
	case WM_MOVE:
		pThis->commentWindow_.OnParentMove();
		// 実際に捉えたいVideo Containerウィンドウの変化はすこし遅れるため
		SetTimer(pThis->hForce_, TIMER_DONE_MOVE, 500, nullptr);
		break;
	case WM_SIZE:
		pThis->commentWindow_.OnParentSize();
		SetTimer(pThis->hForce_, TIMER_DONE_SIZE, 500, nullptr);
		break;
	case WM_DROPFILES:
		if (pThis->s_.dropLogfileMode == 0) {
			break;
		}
		if (pThis->m_pApp->GetFullscreen()) {
			// ファイルダイアログ等でのD&Dを無視するため(確実ではない)
			HWND hwndActive = GetActiveWindow();
			if (hwndActive && (GetWindowLong(GetAncestor(hwndActive, GA_ROOT), GWL_EXSTYLE) & WS_EX_DLGMODALFRAME) != 0) {
				break;
			}
		}
		// 読み込み可能な拡張子をもつ最初にみつかったファイルを開く
		pThis->dropFileTimeout_ = 0;
		for (UINT i = DragQueryFile(reinterpret_cast<HDROP>(wParam), 0xFFFFFFFF, nullptr, 0); i != 0; --i) {
			if (DragQueryFile(reinterpret_cast<HDROP>(wParam), i - 1, pThis->dropFileName_, _countof(pThis->dropFileName_))) {
				LPCTSTR ext = PathFindExtension(pThis->dropFileName_);
				if (!lstrcmpi(ext, TEXT(".jkl")) || !lstrcmpi(ext, TEXT(".xml")) || !lstrcmpi(ext, TEXT(".txt"))) {
					if (pThis->bSpecFile_) {
						pThis->ReadFromLogfile(-1);
						DeleteFile(pThis->tmpSpecFileName_);
						pThis->bSpecFile_ = false;
					}
					SendDlgItemMessage(pThis->hForce_, IDC_CHECK_SPECFILE, BM_SETCHECK, BST_UNCHECKED, 0);
					if (pThis->s_.dropLogfileMode == 2) {
						// ウィンドウの左右どちらにD&DされたかでRelチェックボックスを変える
						RECT rc;
						HWND hwndFull = pThis->GetFullscreenWindow();
						GetClientRect(hwndFull ? hwndFull : pThis->m_pApp->GetAppWindow(), &rc);
						POINT pt = {};
						DragQueryPoint(reinterpret_cast<HDROP>(wParam), &pt);
						SendDlgItemMessage(pThis->hForce_, IDC_CHECK_RELATIVE, BM_SETCHECK, pt.x > rc.right / 2 ? BST_CHECKED : BST_UNCHECKED, 0);
					}
					bool bRel = SendDlgItemMessage(pThis->hForce_, IDC_CHECK_RELATIVE, BM_GETCHECK, 0, 0) == BST_CHECKED;
					pThis->dropFileTimeout_ = 10;
					SetTimer(pThis->hForce_, TIMER_OPEN_DROPFILE, bRel ? 2000 : 0, nullptr);
					break;
				}
			}
		}
		// DragFinish()せずに本体のデフォルトプロシージャに任せる
		break;
	}
	return FALSE;
}

// コメント(chatタグ)1行を解釈してコメントウィンドウに送る
bool CNicoJK::ProcessChatTag(const char *tag, bool bShow, int showDelay)
{
	static const std::regex reChat("<chat(?= )(.*)>(.*?)</chat>");
	static const std::regex reMail(" mail=\"(.*?)\"");
	static const std::regex reAbone(" abone=\"1\"");
	static const std::regex reYourpost(" yourpost=\"1\"");
	static const std::regex reInsertAt(" insert_at=\"last\"");
	static const std::regex reAlign(" align=\"(left|right)");
	static const std::regex reUserID(" user_id=\"([0-9A-Za-z\\-_]{0,27})");
	static const std::regex reNo(" no=\"(\\d+)\"");
	static const std::regex reLogcmd(" logcmd=\"(.*?)\"");
	// 置換
	std::string rpl[2];
	if (!rplList_.empty()) {
		rpl[1] = tag;
		int i = 0;
		for (auto it = rplList_.cbegin(); it != rplList_.end(); ++it) {
			if (it->IsEnabled()) {
				try {
					rpl[i % 2] = std::regex_replace(rpl[(i + 1) % 2], it->re, it->fmt);
				} catch (std::regex_error&) {
					// 置換フォーマット異常のため無視する
					continue;
				}
				tag = rpl[i++ % 2].c_str();
			}
		}
	}
	std::cmatch m, mm;
	unsigned int tm;
	if (std::regex_match(tag, m, reChat) && GetChatDate(&tm, tag)) {
		TCHAR text[CHAT_TEXT_MAX];
		int len = MultiByteToWideChar(CP_UTF8, 0, m[2].first, static_cast<int>(m[2].length()), text, _countof(text) - 1);
		text[len] = TEXT('\0');
		DecodeEntityReference(text);
		// mail属性は無いときもある
		char mail[256];
		mail[0] = '\0';
		if (std::regex_search(m[1].first, m[1].second, mm, reMail)) {
			lstrcpynA(mail, mm[1].first, min<int>(static_cast<int>(mm[1].length()) + 1, _countof(mail)));
		}
		// abone属性(ローカル拡張)
		bool bAbone = std::regex_search(m[1].first, m[1].second, reAbone);
		if (bShow && !bAbone) {
			bool bYourpost = std::regex_search(m[1].first, m[1].second, reYourpost);
			// insert_at属性(ローカル拡張)
			bool bInsertLast = std::regex_search(m[1].first, m[1].second, reInsertAt);
			// align属性(ローカル拡張)
			CCommentWindow::CHAT_ALIGN align = CCommentWindow::CHAT_ALIGN_CENTER;
			if (std::regex_search(m[1].first, m[1].second, mm, reAlign)) {
				align = mm[1].first[0] == 'l' ? CCommentWindow::CHAT_ALIGN_LEFT : CCommentWindow::CHAT_ALIGN_RIGHT;
			}
			commentWindow_.AddChat(text, GetColor(mail), HasToken(mail, "shita") ? CCommentWindow::CHAT_POS_SHITA :
			                       HasToken(mail, "ue") ? CCommentWindow::CHAT_POS_UE : CCommentWindow::CHAT_POS_DEFAULT,
			                       HasToken(mail, "small") ? CCommentWindow::CHAT_SIZE_SMALL : CCommentWindow::CHAT_SIZE_DEFAULT,
			                       align, bInsertLast, bYourpost ? 160 : 0, showDelay);
		}

		// リストボックスのログ表示キューに追加
		LOG_ELEM e;
		FILETIME ftUtc, ft;
		UnixTimeToFileTime(tm, &ftUtc);
		FileTimeToLocalFileTime(&ftUtc, &ft);
		FileTimeToSystemTime(&ft, &e.st);
		e.no = 0;
		e.cr = RGB(0xFF, 0xFF, 0xFF);
		e.marker[0] = TEXT('\0');
		if (!bShow) {
			lstrcpy(e.marker, TEXT("."));
		} else if (std::regex_search(m[1].first, m[1].second, mm, reUserID)) {
			len = MultiByteToWideChar(CP_UTF8, 0, mm[1].first, static_cast<int>(mm[1].length()), e.marker, _countof(e.marker) - 1);
			e.marker[len] = TEXT('\0');
			if (std::regex_search(m[1].first, m[1].second, mm, reNo)) {
				e.no = atoi(mm[1].first);
			}
			// logcmd属性(ローカル拡張)
			char logcmd[256];
			if (std::regex_search(m[1].first, m[1].second, mm, reLogcmd)) {
				lstrcpynA(logcmd, mm[1].first, min<int>(static_cast<int>(mm[1].length()) + 1, _countof(logcmd)));
				e.cr = GetColor(logcmd);
			}
		}
		if (bAbone) {
			e.text = s_.abone;
			// 末尾の'&'は元コメントに置き換える (TODO: 末尾以外にも対応したほうがいいかも)
			if (!e.text.empty() && e.text.back() == TEXT('&')) {
				e.text.replace(e.text.size() - 1, 1, text);
			}
		} else {
			e.text = text;
		}
		logList_.push_back(std::move(e));
		return true;
	}
	return false;
}

// ログウィンドウにユーザへのメッセージログを出す
void CNicoJK::OutputMessageLog(LPCTSTR text)
{
	// リストボックスのログ表示キューに追加
	LOG_ELEM e;
	GetLocalTime(&e.st);
	e.no = 0;
	e.cr = RGB(0xFF, 0xFF, 0xFF);
	lstrcpy(e.marker, TEXT("#"));
	e.text = text;
	logList_.push_back(std::move(e));
	if (hForce_) {
		SendMessage(hForce_, WM_UPDATE_LIST, FALSE, 0);
	}
}

// コメント投稿欄の文字列を取得する
void CNicoJK::GetPostComboBoxText(LPTSTR comm, int commSize, LPTSTR mail, int mailSize)
{
	TCHAR text[512];
	if (!GetDlgItemText(hForce_, IDC_CB_POST, text, _countof(text))) {
		text[0] = TEXT('\0');
	}
	if (mail) {
		mail[0] = TEXT('\0');
	}
	// []で囲われた部分はmail属性値とする
	LPCTSTR p = text;
	if (*p == '[') {
		p += StrCSpn(p, TEXT("]"));
		if (*p == ']') {
			if (mail) {
				lstrcpyn(mail, &text[1], min(static_cast<int>(p - text), mailSize));
			}
			++p;
		}
	}
	lstrcpyn(comm, p, commSize);
}

// コメント投稿欄のローカルコマンドを処理する
void CNicoJK::ProcessLocalPost(LPCTSTR comm)
{
	// パラメータ分割
	TCHAR cmd[16];
	int cmdLen = StrCSpn(comm, TEXT(" "));
	lstrcpyn(cmd, comm, min<int>(cmdLen + 1, _countof(cmd)));
	LPCTSTR arg = &comm[cmdLen] + StrSpn(&comm[cmdLen], TEXT(" "));
	int nArg;
	if (!StrToIntEx(arg, STIF_DEFAULT, &nArg)) {
		nArg = INT_MAX;
	}
	if (!lstrcmpi(cmd, TEXT("help"))) {
		static const TCHAR text[] =
			TEXT("@help\tヘルプを表示")
			TEXT("\n@fopa N\t勢い窓の透過レベル1～10(Nを省略すると10)。")
			TEXT("\n@mask N\tログの時間(ID)部の省略マスク(Nを省略すると0)。")
			TEXT("\n@opa N\tコメントの透過レベル0～10(Nを省略すると10)。")
			TEXT("\n@fwd N\tコメントの前進")
			TEXT("\n@size N\tコメントの文字サイズをN%にする(Nを省略すると100%)。")
			TEXT("\n@speed N\tコメントの速度をN%にする(Nを省略すると100%)。")
			TEXT("\n@rl\t置換リストのすべてのCommentをリストする")
			TEXT("\n@rr\t置換リストを設定ファイルから再読み込みする")
			TEXT("\n@ra N\tPatternN0～N9を有効にする")
			TEXT("\n@rm N\tPatternN0～N9を無効にする")
			TEXT("\n@debug N\tデバッグ0～15");
		MessageBox(hForce_, text, TEXT("NicoJK - ローカルコマンド"), MB_OK);
	} else if (!lstrcmpi(cmd, TEXT("fopa"))) {
		s_.forceOpacity = 0 < nArg && nArg < 10 ? nArg * 255 / 10 : 255;
		LONG style = GetWindowLong(hForce_, GWL_EXSTYLE);
		SetWindowLong(hForce_, GWL_EXSTYLE, s_.forceOpacity == 255 ? style & ~WS_EX_LAYERED : style | WS_EX_LAYERED);
		SetLayeredWindowAttributes(hForce_, 0, static_cast<BYTE>(s_.forceOpacity), LWA_ALPHA);
	} else if (!lstrcmpi(cmd, TEXT("mask"))) {
		s_.headerMask = 0 < nArg && nArg < INT_MAX ? nArg : 0;
		TCHAR text[64];
		wsprintf(text, TEXT("現在の省略マスクは%d(0x%04x)です。"), s_.headerMask, s_.headerMask);
		OutputMessageLog(text);
	} else if (!lstrcmpi(cmd, TEXT("opa"))) {
		int opa = 0 <= nArg && nArg < 10 ? nArg : 10;
		SendDlgItemMessage(hForce_, IDC_SLIDER_OPACITY, TBM_SETPOS, TRUE, opa);
		SendMessage(hForce_, WM_HSCROLL, MAKEWPARAM(SB_THUMBTRACK, opa), reinterpret_cast<LPARAM>(GetDlgItem(hForce_, IDC_SLIDER_OPACITY)));
		TCHAR text[64];
		wsprintf(text, TEXT("現在の透過レベルは%dです。"), opa);
		OutputMessageLog(text);
	} else if (!lstrcmpi(cmd, TEXT("fwd")) && nArg != INT_MAX) {
		if (nArg == 0) {
			forwardOffsetDelta_ = -forwardOffset_;
		} else {
			forwardOffsetDelta_ += nArg;
		}
	} else if (!lstrcmpi(cmd, TEXT("size"))) {
		int rate = min(max(nArg == INT_MAX ? 100 : nArg, 10), 1000);
		commentWindow_.SetCommentSize(s_.commentSize * rate / 100, s_.commentSizeMin, s_.commentSizeMax, s_.commentLineMargin);
		TCHAR text[64];
		wsprintf(text, TEXT("現在のコメントの文字サイズは%d%%です。"), rate);
		OutputMessageLog(text);
	} else if (!lstrcmpi(cmd, TEXT("speed"))) {
		commentWindow_.SetDisplayDuration(s_.commentDuration * 100 / (nArg <= 0 || nArg == INT_MAX ? 100 : nArg));
		TCHAR text[64];
		wsprintf(text, TEXT("現在のコメントの表示期間は%dmsecです。"), commentWindow_.GetDisplayDuration());
		OutputMessageLog(text);
	} else if (!lstrcmpi(cmd, TEXT("rl"))) {
		std::wstring text;
		for (auto it = rplList_.cbegin(); it != rplList_.end(); ++it) {
			if (!it->comment.empty() && it->section == TEXT("CustomReplace")) {
				TCHAR key[64];
				wsprintf(key, TEXT("%sPattern%d="), it->IsEnabled() ? TEXT("") : TEXT("#"), it->key);
				text += key + it->comment + TEXT('\n');
			}
		}
		MessageBox(hForce_, text.c_str(), TEXT("NicoJK - ローカルコマンド"), MB_OK);
	} else if (!lstrcmpi(cmd, TEXT("rr"))) {
		rplList_.clear();
		LoadRplListFromIni(TEXT("AutoReplace"), &rplList_);
		LoadRplListFromIni(TEXT("CustomReplace"), &rplList_);
		OutputMessageLog(TEXT("置換リストを再読み込みしました。"));
	} else if (!lstrcmpi(cmd, TEXT("ra")) || !lstrcmpi(cmd, TEXT("rm"))) {
		bool bFound = false;
		for (auto it = rplList_.begin(); it != rplList_.end(); ++it) {
			if (it->key / 10 == nArg && it->section == TEXT("CustomReplace")) {
				bFound = true;
				it->SetEnabled(cmd[1] == TEXT('a'));
				TCHAR key[64];
				wsprintf(key, TEXT("Pattern%d("), it->key);
				OutputMessageLog((key + it->comment + TEXT(")を") + (it->IsEnabled() ? TEXT('有') : TEXT('無')) + TEXT("効にしました。")).c_str());
			}
		}
		if (bFound) {
			SaveRplListToIni(TEXT("CustomReplace"), rplList_, false);
		} else {
			OutputMessageLog(TEXT("Error:パターンが見つかりません。"));
		}
	} else if (!lstrcmpi(cmd, TEXT("debug"))) {
		commentWindow_.SetDebugFlags(nArg);
	} else {
		OutputMessageLog(TEXT("Error:不明なローカルコマンドです。"));
	}
}

// サブクラス化した勢いリストのプロシージャ
static LRESULT CALLBACK ForceListBoxProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg) {
	case WM_ERASEBKGND:
		{
			int n = ListBox_GetCount(hwnd);
			RECT rcLast;
			if (n > 0 && ListBox_GetItemRect(hwnd, n - 1, &rcLast) != LB_ERR) {
				// 背景消去範囲を限定することでちらつきを抑える
				RECT rc;
				GetClientRect(hwnd, &rc);
				if (rc.bottom > rcLast.bottom) {
					rc.top = rcLast.bottom;
					FillRect(reinterpret_cast<HDC>(wParam), &rc, reinterpret_cast<HBRUSH>(GetClassLongPtr(hwnd, GCLP_HBRBACKGROUND)));
				}
				return TRUE;
			}
		}
		break;
	case WM_GETDLGCODE:
		// 本体のアクセラレータを抑制するため
		return DLGC_WANTARROWS;
	}
	return CallWindowProc(reinterpret_cast<WNDPROC>(GetProp(hwnd, TEXT("DefProc"))), hwnd, uMsg, wParam, lParam);
}

// サブクラス化した投稿欄のプロシージャ
static LRESULT CALLBACK ForcePostEditBoxProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg) {
	case WM_CHAR:
		if (wParam == VK_RETURN) {
			// 改行入力
			if ((lParam & 0x40000000) == 0) {
				SendMessage(static_cast<HWND>(GetProp(hwnd, TEXT("Root"))), WM_POST_COMMENT, 0, 0);
			}
			return 0;
		} else if (wParam == 0x16) {
			// Ctrl+'V'入力
			HWND hwndRoot = static_cast<HWND>(GetProp(hwnd, TEXT("Root")));
			int len = GetWindowTextLength(GetDlgItem(hwndRoot, IDC_CB_POST));
			LONG selRange = static_cast<LONG>(SendDlgItemMessage(hwndRoot, IDC_CB_POST, CB_GETEDITSEL, 0, 0));
			// 入力欄が空になるときだけ処理
			if (len == 0 || MAKELONG(0, len) == selRange) {
				// クリップボードを取得
				TCHAR clip[512];
				clip[0] = TEXT('\0');
				if (OpenClipboard(nullptr)) {
					HGLOBAL hg = GetClipboardData(CF_UNICODETEXT);
					if (hg) {
						LPWSTR pg = static_cast<LPWSTR>(GlobalLock(hg));
						if (pg) {
							lstrcpyn(clip, pg, _countof(clip));
							GlobalUnlock(hg);
						}
					}
					CloseClipboard();
				}
				// 改行->レコードセパレータ
				LPTSTR q = clip;
				bool bLF = false;
				bool bMultiLine = false;
				for (LPCTSTR p = q; *p; ++p) {
					if (*p == TEXT('\n')) {
						*q++ = TEXT('\x1e');
						bLF = true;
					} else if (*p != TEXT('\r')) {
						*q++ = *p;
						bMultiLine = bLF;
					}
				}
				*q = TEXT('\0');
				// 複数行のペーストだけ独自に処理
				if (bMultiLine) {
					SetDlgItemText(hwndRoot, IDC_CB_POST, clip);
					SendMessage(hwndRoot, WM_COMMAND, MAKEWPARAM(IDC_CB_POST, CBN_EDITCHANGE), 0);
					return 0;
				}
			}
		}
		break;
	case WM_GETDLGCODE:
		// 本体のアクセラレータを抑制するため
		return DLGC_WANTALLKEYS;
	}
	return CallWindowProc(reinterpret_cast<WNDPROC>(GetProp(hwnd, TEXT("DefProc"))), hwnd, uMsg, wParam, lParam);
}

// サブクラス化したボタンのプロシージャ
static LRESULT CALLBACK TVTestPanelButtonProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg) {
	case WM_PAINT:
		{
			TVTest::CTVTestApp *pApp = reinterpret_cast<TVTest::CTVTestApp*>(GetProp(hwnd, TEXT("App")));
			int state = Button_GetState(hwnd);
			LPCWSTR style = state & BST_HOT ? L"control-panel.item.hot" : state & BST_CHECKED ? L"control-panel.item.checked" : L"control-panel.item";
			PAINTSTRUCT ps;
			HDC hdc = BeginPaint(hwnd, &ps);
			RECT rc;
			GetClientRect(hwnd, &rc);
			pApp->ThemeDrawBackground(style, hdc, rc);
			TCHAR text[256];
			if (GetWindowText(hwnd, text, _countof(text))) {
				HFONT hFont = reinterpret_cast<HFONT>(SendMessage(hwnd, WM_GETFONT, 0, 0));
				HFONT hFontOld = nullptr;
				if (hFont) {
					hFontOld = SelectFont(hdc, hFont);
				}
				int oldBkMode = SetBkMode(hdc, TRANSPARENT);
				pApp->ThemeDrawText(style, hdc, text, rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
				SetBkMode(hdc, oldBkMode);
				if (hFont) {
					SelectFont(hdc, hFontOld);
				}
			}
			EndPaint(hwnd, &ps);
		}
		return 0;
	}
	return CallWindowProc(reinterpret_cast<WNDPROC>(GetProp(hwnd, TEXT("DefProc"))), hwnd, uMsg, wParam, lParam);
}

static void SetTVTestPanelItem(HWND hButton, TVTest::CTVTestApp *pApp, LRESULT (CALLBACK *pProc)(HWND, UINT, WPARAM, LPARAM))
{
	SetProp(hButton, TEXT("App"), pApp);
	SetProp(hButton, TEXT("DefProc"), reinterpret_cast<HANDLE>(GetWindowLongPtr(hButton, GWLP_WNDPROC)));
	SetWindowLongPtr(hButton, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(pProc));
}

static void ResetTVTestPanelItem(HWND hButton)
{
	SetWindowLongPtr(hButton, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(GetProp(hButton, TEXT("DefProc"))));
	RemoveProp(hButton, TEXT("DefProc"));
	RemoveProp(hButton, TEXT("App"));
}

LRESULT CALLBACK CNicoJK::PanelWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	if (uMsg == WM_CREATE) {
		SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<LPCREATESTRUCT>(lParam)->lpCreateParams));
	}
	CNicoJK *pThis = reinterpret_cast<CNicoJK*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
	if (pThis) {
		switch (uMsg) {
		case WM_CREATE:
			SetProp(hwnd, TEXT("IsHide"), reinterpret_cast<HANDLE>('N'));
			SetTimer(hwnd, 1, 5000, nullptr);
			return TRUE;
		case WM_DESTROY:
			RemoveProp(hwnd, TEXT("IsHide"));
			pThis->hPanel_ = nullptr;
			break;
		case WM_TIMER:
			if (wParam == 1) {
				// パネルの表示非表示を捕捉する手段が思いつかないので苦肉の策
				bool bHide = pThis->hForce_ && !IsWindowVisible(pThis->hForce_);
				if (!bHide && GetProp(hwnd, TEXT("IsHide")) == reinterpret_cast<HANDLE>('Y')) {
					SetProp(hwnd, TEXT("IsHide"), reinterpret_cast<HANDLE>('N'));
					if (pThis->hForce_) {
						SendMessage(pThis->hForce_, WM_UPDATE_LIST, TRUE, 0);
						PostMessage(pThis->hForce_, WM_TIMER, TIMER_UPDATE, 0);
					}
				} else if (bHide && GetProp(hwnd, TEXT("IsHide")) == reinterpret_cast<HANDLE>('N')) {
					SetProp(hwnd, TEXT("IsHide"), reinterpret_cast<HANDLE>('Y'));
				}
			}
			break;
		case WM_SIZE:
			if (pThis->hForce_) {
				MoveWindow(pThis->hForce_, 0, 0, LOWORD(lParam), HIWORD(lParam), TRUE);
			}
			break;
		default:
			PostMessage(hwnd, WM_TIMER, 1, 0);
			break;
		}
	}
	return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK CNicoJK::ForceWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	if (uMsg == WM_CREATE) {
		SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<LPCREATESTRUCT>(lParam)->lpCreateParams));
	}
	CNicoJK *pThis = reinterpret_cast<CNicoJK*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
	return pThis ? pThis->ForceWindowProcMain(hwnd, uMsg, wParam, lParam) : DefWindowProc(hwnd, uMsg, wParam, lParam);
}

bool CNicoJK::CreateForceWindowItems(HWND hwnd)
{
	int padding = hPanel_ ? 0 : 4;
	if (CreateWindowEx(0, TEXT("BUTTON"), TEXT("勢い"), WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | BS_PUSHLIKE,
	        padding, padding, 60, 24, hwnd, reinterpret_cast<HMENU>(IDC_RADIO_FORCE), g_hinstDLL, nullptr) &&
	    CreateWindowEx(0, TEXT("BUTTON"), TEXT("ログ"), WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | BS_PUSHLIKE,
	        padding + 60, padding, 60, 24, hwnd, reinterpret_cast<HMENU>(IDC_RADIO_LOG), g_hinstDLL, nullptr) &&
	    CreateWindowEx(0, TEXT("BUTTON"), TEXT("File"), WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
	        padding + 124, padding + 4, 50, 16, hwnd, reinterpret_cast<HMENU>(IDC_CHECK_SPECFILE), g_hinstDLL, nullptr) &&
	    CreateWindowEx(0, TEXT("BUTTON"), TEXT("Rel"), WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
	        padding + 174, padding + 4, 50, 16, hwnd, reinterpret_cast<HMENU>(IDC_CHECK_RELATIVE), g_hinstDLL, nullptr) &&
	    // TODO: (描画がとても面倒なので)スライダーはパネルでは表示しない
	    CreateWindowEx(0, TRACKBAR_CLASS, TEXT("不透明度"), WS_CHILD | WS_VISIBLE | TBS_BOTH | TBS_NOTICKS | TBS_TOOLTIPS,
	        padding + 224, hPanel_ ? -100 : padding + 4, 64, 21, hwnd, reinterpret_cast<HMENU>(IDC_SLIDER_OPACITY), g_hinstDLL, nullptr) &&
	    CreateWindowEx(WS_EX_ACCEPTFILES, TEXT("LISTBOX"), nullptr, WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_BORDER | LBS_NOINTEGRALHEIGHT | LBS_HASSTRINGS | LBS_OWNERDRAWFIXED | LBS_NOTIFY,
	        padding, padding + 24, 100, 100, hwnd, reinterpret_cast<HMENU>(IDC_FORCELIST), g_hinstDLL, nullptr) &&
	    CreateWindowEx(0, TEXT("COMBOBOX"), nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWN | CBS_AUTOHSCROLL | CBS_HASSTRINGS,
	        padding, padding + 124, 100, 50, hwnd, reinterpret_cast<HMENU>(IDC_CB_POST), g_hinstDLL, nullptr))
	{
		if (hForceFont_) {
			SendDlgItemMessage(hwnd, IDC_RADIO_FORCE, WM_SETFONT, reinterpret_cast<WPARAM>(hForceFont_), 0);
			SendDlgItemMessage(hwnd, IDC_RADIO_LOG, WM_SETFONT, reinterpret_cast<WPARAM>(hForceFont_), 0);
			SendDlgItemMessage(hwnd, IDC_CHECK_SPECFILE, WM_SETFONT, reinterpret_cast<WPARAM>(hForceFont_), 0);
			SendDlgItemMessage(hwnd, IDC_CHECK_RELATIVE, WM_SETFONT, reinterpret_cast<WPARAM>(hForceFont_), 0);
			SendDlgItemMessage(hwnd, IDC_FORCELIST, WM_SETFONT, reinterpret_cast<WPARAM>(hForceFont_), 0);
			SendDlgItemMessage(hwnd, IDC_CB_POST, WM_SETFONT, reinterpret_cast<WPARAM>(hForceFont_), 0);
		}
		return true;
	}
	return false;
}

LRESULT CNicoJK::ForceWindowProcMain(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg) {
	case WM_CREATE:
		if (CreateForceWindowItems(hwnd)) {
			logList_.clear();
			logListDisplayedSize_ = 0;
			bPendingTimerUpdateList_ = false;
			lastCalcText_.clear();
			commentWindow_.SetStyle(s_.commentFontName, s_.commentFontNameMulti, s_.bCommentFontBold, s_.bCommentFontAntiAlias,
			                        s_.commentFontOutline, s_.bUseOsdCompositor, s_.bUseTexture, s_.bUseDrawingThread);
			commentWindow_.SetCommentSize(s_.commentSize, s_.commentSizeMin, s_.commentSizeMax, s_.commentLineMargin);
			commentWindow_.SetDisplayDuration(s_.commentDuration);
			commentWindow_.SetDrawLineCount(s_.commentDrawLineCount);
			commentWindow_.SetOpacity(static_cast<BYTE>(s_.commentOpacity));
			m_pApp->SetPluginCommandState(COMMAND_HIDE_COMMENT, commentWindow_.GetOpacity() != 0 ? TVTest::COMMAND_ICON_STATE_CHECKED : 0);
			if (commentWindow_.GetOpacity() != 0 && m_pApp->GetPreview()) {
				HWND hwndContainer = FindVideoContainer();
				commentWindow_.Create(hwndContainer);
				bHalfSkip_ = GetWindowHeight(hwndContainer) >= s_.halfSkipThreshold;
				ProcessChatTag("<!--<chat date=\"0\" mail=\"cyan ue\" user_id=\"-\">(NicoJK ON)</chat>-->");
			}
			bDisplayLogList_ = (s_.hideForceWindow & 2) != 0;
			forwardTick_ = timeGetTime();
			forwardOffset_ = 0;
			forwardOffsetDelta_ = 0;
			currentJKToGet_ = -1;
			jkLeaveThreadID_[0] = '\0';
			jkLeaveThreadCheck_ = 0;
			commentServerResponse_[0] = '\0';
			lastPostComm_[0] = TEXT('\0');
			bUsingLogfileDriver_ = IsMatchDriverName(s_.logfileDrivers.c_str());
			readLogfileTick_ = GetTickCount();
			bSpecFile_ = false;
			dropFileTimeout_ = 0;
			SendMessage(hwnd, WM_RESET_STREAM, 0, 0);

			channelSocket_.SetDoHalfClose(s_.bDoHalfClose);
			jkSocket_.SetDoHalfClose(s_.bDoHalfClose);
			postSocket_.SetDoHalfClose(s_.bDoHalfClose);

			SendDlgItemMessage(hwnd, bDisplayLogList_ ? IDC_RADIO_LOG : IDC_RADIO_FORCE, BM_SETCHECK, BST_CHECKED, 0);
			SendDlgItemMessage(hwnd, IDC_CHECK_RELATIVE, BM_SETCHECK, s_.bSetRelative ? BST_CHECKED : BST_UNCHECKED, 0);
			SendDlgItemMessage(hwnd, IDC_SLIDER_OPACITY, TBM_SETRANGE, TRUE, MAKELPARAM(0, 10));
			SendDlgItemMessage(hwnd, IDC_SLIDER_OPACITY, TBM_SETPOS, TRUE, (commentWindow_.GetOpacity() * 10 + 254) / 255);
			SendMessage(hwnd, WM_COMMAND, MAKEWPARAM(IDC_CB_POST, CBN_EDITCHANGE), 0);
			SetTimer(hwnd, TIMER_UPDATE, max(UPDATE_FORCE_INTERVAL, 10000), nullptr);
			if (s_.timerInterval >= 0) {
				SetTimer(hwnd, TIMER_FORWARD, s_.timerInterval, nullptr);
			}
			SetTimer(hwnd, TIMER_SETUP_CURJK, SETUP_CURJK_DELAY, nullptr);
			PostMessage(hwnd, WM_TIMER, TIMER_UPDATE, 0);
			PostMessage(hwnd, WM_TIMER, TIMER_JK_WATCHDOG, 0);
			if (hPanel_) {
				// パネルウィンドウに連動
				DeleteBrush(SetClassLongPtr(hwnd, GCLP_HBRBACKGROUND,
					reinterpret_cast<LONG_PTR>(CreateSolidBrush(m_pApp->GetColor(L"PanelBack")))));
				GetClientRect(hPanel_, &s_.rcForce);
			} else {
				// 位置を復元
				HMONITOR hMon = MonitorFromRect(&s_.rcForce, MONITOR_DEFAULTTONEAREST);
				MONITORINFO mi;
				mi.cbSize = sizeof(MONITORINFO);
				if (s_.rcForce.right <= s_.rcForce.left || !GetMonitorInfo(hMon, &mi) ||
				    s_.rcForce.right < mi.rcMonitor.left + 20 || mi.rcMonitor.right - 20 < s_.rcForce.left ||
				    s_.rcForce.bottom < mi.rcMonitor.top + 20 || mi.rcMonitor.bottom - 20 < s_.rcForce.top) {
					GetWindowRect(hwnd, &s_.rcForce);
				}
			}
			MoveWindow(hwnd, 0, 0, 64, 64, FALSE);
			MoveWindow(hwnd, s_.rcForce.left, s_.rcForce.top, s_.rcForce.right - s_.rcForce.left, s_.rcForce.bottom - s_.rcForce.top, FALSE);
			// 不透明度を復元
			LONG style = GetWindowLong(hwnd, GWL_EXSTYLE);
			SetWindowLong(hwnd, GWL_EXSTYLE, s_.forceOpacity == 255 ? style & ~WS_EX_LAYERED : style | WS_EX_LAYERED);
			SetLayeredWindowAttributes(hwnd, 0, static_cast<BYTE>(s_.forceOpacity), LWA_ALPHA);

			m_pApp->SetPluginCommandState(COMMAND_HIDE_FORCE, 0);
			if (hPanel_ || (s_.hideForceWindow & 1) == 0) {
				ShowWindow(hwnd, SW_SHOWNA);
				SendMessage(hwnd, WM_SET_ZORDER, 0, 0);
			}
			// TVTest起動直後はVideo Containerウィンドウの配置が定まっていないようなので再度整える
			SetTimer(hwnd, TIMER_DONE_SIZE, 500, nullptr);

			// 勢いリストのサブクラス化
			HWND hList = GetDlgItem(hwnd, IDC_FORCELIST);
			SetProp(hList, TEXT("DefProc"), reinterpret_cast<HANDLE>(GetWindowLongPtr(hList, GWLP_WNDPROC)));
			SetWindowLongPtr(hList, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ForceListBoxProc));
			// 投稿欄のサブクラス化
			COMBOBOXINFO cbi = {};
			cbi.cbSize = sizeof(cbi);
			if (GetComboBoxInfo(GetDlgItem(hwnd, IDC_CB_POST), &cbi)) {
				SetProp(cbi.hwndItem, TEXT("Root"), hwnd);
				SetProp(cbi.hwndItem, TEXT("DefProc"), reinterpret_cast<HANDLE>(GetWindowLongPtr(cbi.hwndItem, GWLP_WNDPROC)));
				SetWindowLongPtr(cbi.hwndItem, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ForcePostEditBoxProc));
			}
			// パネルアイテムのサブクラス化
			if (hPanel_) {
				SetTVTestPanelItem(GetDlgItem(hwnd, IDC_RADIO_FORCE), m_pApp, TVTestPanelButtonProc);
				SetTVTestPanelItem(GetDlgItem(hwnd, IDC_RADIO_LOG), m_pApp, TVTestPanelButtonProc);
				SetTVTestPanelItem(GetDlgItem(hwnd, IDC_CHECK_SPECFILE), m_pApp, TVTestPanelButtonProc);
				SetTVTestPanelItem(GetDlgItem(hwnd, IDC_CHECK_RELATIVE), m_pApp, TVTestPanelButtonProc);
			}
			return TRUE;
		}
		return FALSE;
	case WM_DESTROY:
		{
			// パネルアイテムのサブクラス化を解除
			if (hPanel_) {
				ResetTVTestPanelItem(GetDlgItem(hwnd, IDC_RADIO_FORCE));
				ResetTVTestPanelItem(GetDlgItem(hwnd, IDC_RADIO_LOG));
				ResetTVTestPanelItem(GetDlgItem(hwnd, IDC_CHECK_SPECFILE));
				ResetTVTestPanelItem(GetDlgItem(hwnd, IDC_CHECK_RELATIVE));
			}
			// 投稿欄のサブクラス化を解除
			COMBOBOXINFO cbi = {};
			cbi.cbSize = sizeof(cbi);
			if (GetComboBoxInfo(GetDlgItem(hwnd, IDC_CB_POST), &cbi)) {
				SetWindowLongPtr(cbi.hwndItem, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(GetProp(cbi.hwndItem, TEXT("DefProc"))));
				RemoveProp(cbi.hwndItem, TEXT("DefProc"));
				RemoveProp(cbi.hwndItem, TEXT("Root"));
			}
			// 勢いリストのサブクラス化を解除
			HWND hList = GetDlgItem(hwnd, IDC_FORCELIST);
			SetWindowLongPtr(hList, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(GetProp(hList, TEXT("DefProc"))));
			RemoveProp(hList, TEXT("DefProc"));

			// 位置を保存
			GetWindowRect(hwnd, &s_.rcForce);
			s_.commentOpacity = (s_.commentOpacity&~0xFF) | commentWindow_.GetOpacity();
			s_.bSetRelative = SendDlgItemMessage(hwnd, IDC_CHECK_RELATIVE, BM_GETCHECK, 0, 0) == BST_CHECKED;
			// ログファイルを閉じる
			WriteToLogfile(-1);
			ReadFromLogfile(-1);
			if (bSpecFile_) {
				DeleteFile(tmpSpecFileName_);
			}
			commentWindow_.Destroy();
			channelSocket_.Close();
			jkSocket_.Close();
			postSocket_.Close();

			if (hSyncThread_) {
				bQuitSyncThread_ = true;
				WaitForSingleObject(hSyncThread_, INFINITE);
				CloseHandle(hSyncThread_);
				hSyncThread_ = nullptr;
			}
			ToggleStreamCallback(false);
			m_pApp->SetWindowMessageCallback(nullptr);
			SaveToIni();
			m_pApp->SetPluginCommandState(COMMAND_HIDE_FORCE, TVTest::PLUGIN_COMMAND_STATE_DISABLED);
			m_pApp->SetPluginCommandState(COMMAND_HIDE_COMMENT, TVTest::PLUGIN_COMMAND_STATE_DISABLED);
			hForce_ = nullptr;
		}
		break;
	case WM_MEASUREITEM:
		{
			LPMEASUREITEMSTRUCT lpmis = reinterpret_cast<LPMEASUREITEMSTRUCT>(lParam);
			if (lpmis->CtlID == IDC_FORCELIST && hForceFont_) {
				HWND hItem = GetDlgItem(hwnd, IDC_FORCELIST);
				HDC hdc = GetDC(hItem);
				HFONT hFontOld = SelectFont(hdc, hForceFont_);
				TEXTMETRIC tm;
				GetTextMetrics(hdc, &tm);
				SelectFont(hdc, hFontOld);
				ReleaseDC(hItem, hdc);
				lpmis->itemHeight = tm.tmHeight + 1;
				return TRUE;
			}
		}
		break;
	case WM_CLOSE:
		// 隠すだけ
		ShowWindow(hwnd, SW_HIDE);
		return 0;
	case WM_DROPFILES:
		dropFileTimeout_ = 0;
		if (DragQueryFile(reinterpret_cast<HDROP>(wParam), 0, dropFileName_, _countof(dropFileName_))) {
			if (bSpecFile_) {
				ReadFromLogfile(-1);
				DeleteFile(tmpSpecFileName_);
				bSpecFile_ = false;
			}
			SendDlgItemMessage(hwnd, IDC_CHECK_SPECFILE, BM_SETCHECK, BST_UNCHECKED, 0);
			dropFileTimeout_ = 1;
			SetTimer(hwnd, TIMER_OPEN_DROPFILE, 0, nullptr);
		}
		break;
	case WM_HSCROLL:
		if (reinterpret_cast<HWND>(lParam) == GetDlgItem(hwnd, IDC_SLIDER_OPACITY) && LOWORD(wParam) == SB_THUMBTRACK) {
			BYTE newOpacity = static_cast<BYTE>(HIWORD(wParam) * 255 / 10);
			if (commentWindow_.GetOpacity() == 0 && newOpacity != 0 && m_pApp->GetPreview()) {
				commentWindow_.ClearChat();
				HWND hwndContainer = FindVideoContainer();
				commentWindow_.Create(hwndContainer);
				bHalfSkip_ = GetWindowHeight(hwndContainer) >= s_.halfSkipThreshold;
			} else if (commentWindow_.GetOpacity() != 0 && newOpacity == 0) {
				commentWindow_.Destroy();
			}
			commentWindow_.SetOpacity(newOpacity);
			m_pApp->SetPluginCommandState(COMMAND_HIDE_COMMENT, newOpacity != 0 ? TVTest::COMMAND_ICON_STATE_CHECKED : 0);
		}
		break;
	case WM_DRAWITEM:
		{
			LPDRAWITEMSTRUCT lpdis = reinterpret_cast<LPDRAWITEMSTRUCT>(lParam);
			if (lpdis->CtlType == ODT_LISTBOX) {
				bool bSelected = (lpdis->itemState & ODS_SELECTED) != 0;
				HBRUSH hbr = CreateSolidBrush(bSelected ? GetSysColor(COLOR_HIGHLIGHT) : GetBkColor(lpdis->hDC));
				FillRect(lpdis->hDC, &lpdis->rcItem, hbr);
				DeleteBrush(hbr);

				TCHAR text[1024];
				if (ListBox_GetTextLen(lpdis->hwndItem, lpdis->itemID) < _countof(text)) {
					int textLen = ListBox_GetText(lpdis->hwndItem, lpdis->itemID, text);
					if (textLen >= 0) {
						LPCTSTR pText = text;
						bool bEmphasis = false;
						if (pText[0] == TEXT('#')) {
							// 文字列の強調色表示
							bEmphasis = true;
							++pText;
						}
						COLORREF crBk = RGB(0xFF, 0xFF, 0xFF);
						if (pText[0] == TEXT('[')) {
							// 右側文字列の背景色指定
							LPCTSTR p = StrChr(++pText, TEXT(']'));
							if (p) {
								crBk = StrToInt(pText);
								pText = p + 1;
							}
						}
						int oldBkMode = SetBkMode(lpdis->hDC, TRANSPARENT);
						COLORREF crOld = SetTextColor(lpdis->hDC, bSelected ? GetSysColor(COLOR_HIGHLIGHTTEXT) :
						                                          bEmphasis ? RGB(0xFF, 0, 0) : GetTextColor(lpdis->hDC));
						RECT rc = lpdis->rcItem;
						rc.left += 1;
						if (pText[0] == TEXT('{')) {
							// 左側文字列の描画幅指定
							int fixedLen = StrCSpn(&pText[1], TEXT("}"));
							if (static_cast<int>(text + textLen - pText) >= 2 + 2 * fixedLen) {
								std::wstring calcText(&pText[1], &pText[1 + fixedLen]);
								std::wstring drawText(&pText[2 + fixedLen], &pText[2 + 2 * fixedLen]);
								pText += 2 + 2 * fixedLen;
								int mask = s_.headerMask;
								for (size_t i = 0; i < calcText.size(); mask >>= 1) {
									if (mask & 1) {
										calcText.erase(i, 1);
										drawText.erase(i, 1);
									} else {
										++i;
									}
								}
								if (!calcText.empty()) {
									if (lastCalcText_ != calcText) {
										RECT rcCalc = rc;
										DrawText(lpdis->hDC, calcText.c_str(), -1, &rcCalc, DT_SINGLELINE | DT_NOCLIP | DT_NOPREFIX | DT_CALCRECT);
										lastCalcText_ = calcText;
										lastCalcWidth_ = rcCalc.right - rcCalc.left;
									}
									DrawText(lpdis->hDC, drawText.c_str(), -1, &rc, DT_SINGLELINE | DT_NOCLIP | DT_NOPREFIX);
									rc.left += lastCalcWidth_;
								}
							}
						}
						COLORREF crBkOld = SetBkColor(lpdis->hDC, crBk);
						if (!bSelected && crBk != RGB(0xFF, 0xFF, 0xFF)) {
							SetBkMode(lpdis->hDC, OPAQUE);
							// TODO: ホントは黒文字を仮定してはいけない(ハイコントラストテーマとか)
							if (3*GetRValue(crBk) + 6*GetGValue(crBk) + GetBValue(crBk) < 255) {
								SetTextColor(lpdis->hDC, RGB(0xFF, 0xFF, 0xFF));
							}
						}
						DrawText(lpdis->hDC, pText, -1, &rc, DT_SINGLELINE | DT_NOCLIP | DT_NOPREFIX);
						SetBkColor(lpdis->hDC, crBkOld);
						SetTextColor(lpdis->hDC, crOld);
						SetBkMode(lpdis->hDC, oldBkMode);
					}
				}
				return TRUE;
			}
		}
		break;
	case WM_COMMAND:
		switch (LOWORD(wParam)) {
		case IDC_RADIO_FORCE:
		case IDC_RADIO_LOG:
			bDisplayLogList_ = SendDlgItemMessage(hwnd, IDC_RADIO_LOG, BM_GETCHECK, 0, 0 ) == BST_CHECKED;
			SendMessage(hwnd, WM_UPDATE_LIST, TRUE, 0);
			PostMessage(hwnd, WM_TIMER, TIMER_UPDATE, 0);
			break;
		case IDC_CHECK_SPECFILE:
			if (bSpecFile_ != (SendDlgItemMessage(hwnd, IDC_CHECK_SPECFILE, BM_GETCHECK, 0, 0) == BST_CHECKED)) {
				if (bSpecFile_) {
					ReadFromLogfile(-1);
					DeleteFile(tmpSpecFileName_);
					bSpecFile_ = false;
				} else {
					FILETIME ft;
					TCHAR path[MAX_PATH];
					bool bRel = SendDlgItemMessage(hwnd, IDC_CHECK_RELATIVE, BM_GETCHECK, 0, 0) == BST_CHECKED;
					// ダイアログを開いている間にD&Dされるかもしれない
					if ((!bRel || GetCurrentTot(&ft)) &&
					    FileOpenDialog(hwnd, TEXT("実況ログ(*.jkl;*.xml)\0*.jkl;*.xml\0すべてのファイル\0*.*\0"), path, _countof(path)) &&
					    !bSpecFile_ && ImportLogfile(path, tmpSpecFileName_, bRel ? FileTimeToUnixTime(ft) + 2 : 0))
					{
						readLogfileTick_ = GetTickCount();
						bSpecFile_ = true;
					}
				}
				SendDlgItemMessage(hwnd, IDC_CHECK_SPECFILE, BM_SETCHECK, bSpecFile_ ? BST_CHECKED : BST_UNCHECKED, 0);
			}
			break;
		case IDC_FORCELIST:
			if (HIWORD(wParam) == LBN_SELCHANGE) {
				if (!bDisplayLogList_) {
					// 勢いリスト表示中
					int index = ListBox_GetCurSel((HWND)lParam);
					int jkID = -1;
					if (0 <= index && index < (int)forceList_.size()) {
						jkID = forceList_[index].jkID;
					}
					if (currentJKToGet_ != jkID) {
						currentJKToGet_ = jkID;
						jkSocket_.Shutdown();
						commentWindow_.ClearChat();
						SetTimer(hwnd, TIMER_JK_WATCHDOG, 1000, nullptr);
					}
					if (s_.bSetChannel && !bUsingLogfileDriver_ && !bRecording_ && jkID > 0) {
						// 本体のチャンネル切り替えをする
						int currentTuning = m_pApp->GetTuningSpace();
						for (int stage = 0; stage < 2; ++stage) {
							NETWORK_SERVICE_ID_ELEM e = {};
							for (int i = 0; GetChannelNetworkServiceID(currentTuning, i, &e.ntsID); ++i) {
								std::vector<NETWORK_SERVICE_ID_ELEM>::const_iterator it =
									std::lower_bound(ntsIDList_.begin(), ntsIDList_.end(), e,
										[](const NETWORK_SERVICE_ID_ELEM &a, const NETWORK_SERVICE_ID_ELEM &b) { return a.ntsID < b.ntsID; });
								int chJK = it!=ntsIDList_.end() && it->ntsID==e.ntsID ? it->jkID : -1;
								// 実況IDが一致するチャンネルに切替
								// 実況IDからチャンネルへの対応は一般に一意ではないので優先度を設ける
								if ((stage > 0 || (chJK & NETWORK_SERVICE_ID_ELEM::JKID_PRIOR)) && jkID == (chJK & ~NETWORK_SERVICE_ID_ELEM::JKID_PRIOR)) {
									// すでに表示中なら切り替えない
									if (e.ntsID != GetCurrentNetworkServiceID()) {
										m_pApp->SetChannel(currentTuning, i, e.ntsID >> 16);
									}
									stage = 2;
									break;
								}
							}
						}
					}
				}
			} else if (HIWORD(wParam) == LBN_DBLCLK) {
				int index = ListBox_GetCurSel((HWND)lParam);
				if (bDisplayLogList_ && 0 <= index && index < (int)logList_.size()) {
					std::list<LOG_ELEM>::const_iterator it = logList_.begin();
					std::advance(it, index);
					if (it->marker[0] != TEXT('#') && it->marker[0] != TEXT('.')) {
						// ユーザーNGの置換パターンをつくる
						RPL_ELEM e;
						e.section = TEXT("AutoReplace");
						// 14文字で切っているのは単に表現を短くするため。深い理由はない
						TCHAR pattern[256];
						wsprintf(pattern, TEXT("s/^<chat(?=.*? user_id=\"%.14s%s.*>.*<)/<chat abone=\"1\"/g"),
						         it->marker, lstrlen(it->marker) > 14 ? TEXT("") : TEXT("\""));
						if (e.SetPattern(pattern)) {
							// 既存パターンかどうか調べる
							std::vector<RPL_ELEM> autoRplList;
							LoadRplListFromIni(TEXT("AutoReplace"), &autoRplList);
							auto jt = std::find_if(autoRplList.cbegin(), autoRplList.cend(), [&](const RPL_ELEM &a) { return a.pattern == e.pattern; });
							// メッセージボックスで確認
							TCHAR header[_countof(it->marker) + 32];
							wsprintf(header, TEXT(">>%d ID:%s\n"), it->no, it->marker);
							if (jt != autoRplList.end()) {
								if (MessageBox(hwnd, (header + it->text).c_str(), TEXT("NicoJK - NG【解除】します"), MB_OKCANCEL) == IDOK) {
									autoRplList.erase(jt);
									for (int i = 0; i < (int)autoRplList.size(); autoRplList[i].key = i, ++i);
									SaveRplListToIni(TEXT("AutoReplace"), autoRplList);
								}
							} else {
								if (MessageBox(hwnd, (header + it->text).c_str(), TEXT("NicoJK - NG登録します"), MB_OKCANCEL) == IDOK) {
									autoRplList.push_back(e);
									while ((int)autoRplList.size() > max(s_.maxAutoReplace, 0)) {
										autoRplList.erase(autoRplList.begin());
									}
									for (int i = 0; i < (int)autoRplList.size(); autoRplList[i].key = i, ++i);
									SaveRplListToIni(TEXT("AutoReplace"), autoRplList);
								}
							}
							// 置換リストを更新
							rplList_ = autoRplList;
							LoadRplListFromIni(TEXT("CustomReplace"), &rplList_);
						}
					}
				}
			}
			break;
		case IDC_CB_POST:
			if (HIWORD(wParam) == CBN_EDITCHANGE) {
				// コメント装飾例を作成する
				TCHAR comm[POST_COMMENT_MAX + 32];
				GetPostComboBoxText(comm, _countof(comm));
				while (SendDlgItemMessage(hwnd, IDC_CB_POST, CB_DELETESTRING, 0, 0) > 0);
				for (LPCTSTR p = s_.mailDecorations.c_str(); *p; ) {
					int len = StrCSpn(p, TEXT(":"));
					TCHAR text[_countof(comm) + 64];
					lstrcpyn(text, p, min(len + 1, 64));
					lstrcat(text, comm);
					SendDlgItemMessage(hwnd, IDC_CB_POST, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
					p += p[len] ? len + 1 : len;
				}
				// 文字数警告する
				int excess = lstrlen(comm) - (POST_COMMENT_MAX - 1);
				if (excess > 0) {
					TCHAR text[64];
					wsprintf(text, TEXT("Warning:%d文字を超えています(+%d)。"), POST_COMMENT_MAX - 1, excess);
					OutputMessageLog(text);
				}
			}
			break;
		}
		break;
	case WM_TIMER:
		switch (wParam) {
		case TIMER_UPDATE:
			if (!bDisplayLogList_ && IsWindowVisible(hwnd)) {
				// 勢いを更新する
				char szGet[_countof(cookie_) + 256];
				lstrcpyA(szGet, "GET /api/v2_app/getchannels HTTP/1.1\r\n");
				AppendHttpHeader(szGet, "Host: ", JK_HOST_NAME, "\r\n");
				AppendHttpHeader(szGet, "Cookie: ", cookie_, "\r\n");
				AppendHttpHeader(szGet, "Connection: ", "close", "\r\n\r\n");
				// 前回の通信が完了していなくてもfalseを返すだけ。気にせず呼ぶ
				if (channelSocket_.Send(hwnd, WMS_FORCE, JK_HOST_NAME, 80, szGet)) {
					channelBuf_.clear();
				}
			}
			break;
		case TIMER_JK_WATCHDOG:
			SetTimer(hwnd, TIMER_JK_WATCHDOG, max(JK_WATCHDOG_INTERVAL, 10000), nullptr);
			if (jkLeaveThreadCheck_ > 0 && --jkLeaveThreadCheck_ == 0) {
				OutputMessageLog(TEXT("leave_threadタグにより切断します。"));
				jkSocket_.Shutdown();
				SetTimer(hwnd, TIMER_JK_WATCHDOG, 1000, nullptr);
			}
			if (currentJKToGet_ >= 0 && !bUsingLogfileDriver_) {
				// パーマリンクを取得
				char szGet[_countof(cookie_) + 256];
				wsprintfA(szGet, "GET /api/v2/getflv?v=jk%d HTTP/1.1\r\n", currentJKToGet_);
				AppendHttpHeader(szGet, "Host: ", JK_HOST_NAME, "\r\n");
				AppendHttpHeader(szGet, "Cookie: ", cookie_, "\r\n");
				AppendHttpHeader(szGet, "Connection: ", "close", "\r\n\r\n");
				if (jkSocket_.Send(hwnd, WMS_JK, JK_HOST_NAME, 80, szGet)) {
					currentJK_ = currentJKToGet_;
					bConnectedToCommentServer_ = false;
					jkBuf_.clear();
					OutputMessageLog(TEXT("パーマリンクに接続開始しました。"));
				}
			}
			break;
		case TIMER_FORWARD:
			bFlipFlop_ = !bFlipFlop_;
			if (hSyncThread_ || !bHalfSkip_ || bFlipFlop_) {
				bool resyncComment = false;
				{
					CBlockLock lock(&streamLock_);
					if (bResyncComment_) {
						resyncComment = true;
						bResyncComment_ = false;
					}
				}
				// オフセットを調整する
				bool bNotify = false;
				if (0 < forwardOffsetDelta_ && forwardOffsetDelta_ <= 30000) {
					// 前進させて調整
					int delta = min(forwardOffsetDelta_, forwardOffsetDelta_ < 10000 ? 500 : 2000);
					forwardOffset_ += delta;
					forwardOffsetDelta_ -= delta;
					bNotify = forwardOffsetDelta_ == 0;
					commentWindow_.Forward(delta);
				} else if (forwardOffsetDelta_ != 0) {
					// ログファイルを閉じて一気に調整
					forwardOffset_ += forwardOffsetDelta_;
					forwardOffsetDelta_ = 0;
					bNotify = true;
					ReadFromLogfile(-1);
					commentWindow_.ClearChat();
				} else if (resyncComment) {
					// シーク時のコメント再生位置の再調整
					ReadFromLogfile(-1);
					commentWindow_.ClearChat();
				}
				if (bNotify) {
					TCHAR text[32];
					wsprintf(text, TEXT("(Offset %d)"), forwardOffset_ / 1000);
					commentWindow_.AddChat(text, RGB(0x00,0xFF,0xFF), CCommentWindow::CHAT_POS_UE);
				}
				// コメントの表示を進める
				DWORD tick = timeGetTime();
				commentWindow_.Forward(min(static_cast<int>(tick - forwardTick_), 5000));
				forwardTick_ = tick;
				// 過去ログがあれば処理する
				FILETIME ft;
				if (GetCurrentTot(&ft)) {
					bool bRead = false;
					char text[CHAT_TAG_MAX];
					unsigned int tm = FileTimeToUnixTime(ft);
					tm = forwardOffset_ < 0 ? tm - (-forwardOffset_ / 1000) : tm + forwardOffset_ / 1000;
					while (ReadFromLogfile(bSpecFile_ ? 0 : currentJKToGet_, text, _countof(text), tm)) {
						ProcessChatTag(text);
						bRead = true;
					}
					if (bRead) {
						// date属性値は秒精度しかないのでコメント表示が団子にならないよう適当にごまかす
						commentWindow_.ScatterLatestChats(1000);
						PostMessage(hwnd, WM_UPDATE_LIST, FALSE, 0);
					}
				}
				commentWindow_.Update();
				bPendingTimerForward_ = false;
			}
			break;
		case TIMER_SETUP_CURJK:
			{
				// 視聴状態が変化したので視聴中のサービスに対応する実況IDを調べて変更する
				KillTimer(hwnd, TIMER_SETUP_CURJK);
				NETWORK_SERVICE_ID_ELEM e = {GetCurrentNetworkServiceID(), 0};
				std::vector<NETWORK_SERVICE_ID_ELEM>::const_iterator it =
					std::lower_bound(ntsIDList_.begin(), ntsIDList_.end(), e,
						[](const NETWORK_SERVICE_ID_ELEM &a, const NETWORK_SERVICE_ID_ELEM &b) { return a.ntsID < b.ntsID; });
				int jkID = it!=ntsIDList_.end() && (it->ntsID==e.ntsID || !(e.ntsID&0xFFFF) && e.ntsID==(it->ntsID&0xFFFF0000)) && it->jkID > 0 ?
					(it->jkID & ~NETWORK_SERVICE_ID_ELEM::JKID_PRIOR) : -1;
				if (currentJKToGet_ != jkID) {
					currentJKToGet_ = jkID;
					jkSocket_.Shutdown();
					commentWindow_.ClearChat();
					SetTimer(hwnd, TIMER_JK_WATCHDOG, 1000, nullptr);
					// 選択項目を更新するため
					SendMessage(hwnd, WM_UPDATE_LIST, TRUE, 0);
				}
			}
			break;
		case TIMER_OPEN_DROPFILE:
			// D&Dされた実況ログファイルを開く
			// TSファイルとの同時D&Dを考慮してRelチェック時は基準とするTOTの取得タイミングを遅らせる
			if (--dropFileTimeout_ < 0 || bSpecFile_) {
				KillTimer(hwnd, TIMER_OPEN_DROPFILE);
			} else {
				FILETIME ft;
				bool bRel = SendDlgItemMessage(hwnd, IDC_CHECK_RELATIVE, BM_GETCHECK, 0, 0) == BST_CHECKED;
				if (!bRel || GetCurrentTot(&ft)) {
					KillTimer(hwnd, TIMER_OPEN_DROPFILE);
					if (ImportLogfile(dropFileName_, tmpSpecFileName_, bRel ? FileTimeToUnixTime(ft) + 2 : 0)) {
						readLogfileTick_ = GetTickCount();
						bSpecFile_ = true;
						SendDlgItemMessage(hwnd, IDC_CHECK_SPECFILE, BM_SETCHECK, BST_CHECKED, 0);
					}
				}
			}
			break;
		case TIMER_DONE_MOVE:
			KillTimer(hwnd, TIMER_DONE_MOVE);
			commentWindow_.OnParentMove();
			break;
		case TIMER_DONE_SIZE:
			KillTimer(hwnd, TIMER_DONE_SIZE);
			commentWindow_.OnParentSize();
			bHalfSkip_ = GetWindowHeight(FindVideoContainer()) >= s_.halfSkipThreshold;
			break;
		case TIMER_DONE_POSCHANGE:
			KillTimer(hwnd, TIMER_DONE_POSCHANGE);
			if (!m_pApp->GetFullscreen() && ((s_.hideForceWindow & 4) || (GetWindowLong(m_pApp->GetAppWindow(), GWL_STYLE) & WS_MAXIMIZE))) {
				SendMessage(hwnd, WM_SET_ZORDER, 0, 0);
			}
			break;
		case TIMER_UPDATE_LIST:
			KillTimer(hwnd, TIMER_UPDATE_LIST);
			lastUpdateListTick_ = 0;
			bPendingTimerUpdateList_ = false;
			SendMessage(hwnd, WM_UPDATE_LIST, FALSE, 0);
			break;
		}
		break;
	case WM_RESET_STREAM:
		dprintf(TEXT("CNicoJK::ForceDialogProcMain() WM_RESET_STREAM\n")); // DEBUG
		{
			CBlockLock lock(&streamLock_);
			ftTot_[0].dwHighDateTime = 0xFFFFFFFF;
		}
		ReadFromLogfile(-1);
		return TRUE;
	case WM_UPDATE_LIST:
		{
			if (!wParam) {
				// 再描画の頻度を抑える
				DWORD tick = lastUpdateListTick_;
				lastUpdateListTick_ = GetTickCount();
				if (bPendingTimerUpdateList_) {
					return TRUE;
				}
				if (lastUpdateListTick_ - tick < COMMENT_REDRAW_INTERVAL) {
					bPendingTimerUpdateList_ = true;
					SetTimer(hwnd, TIMER_UPDATE_LIST, COMMENT_REDRAW_INTERVAL - (lastUpdateListTick_ - tick), nullptr);
					return TRUE;
				}
			}
			HWND hList = GetDlgItem(hwnd, IDC_FORCELIST);
			if (!bDisplayLogList_ || !IsWindowVisible(hwnd)) {
				// リストが増え続けないようにする
				for (; logList_.size() > COMMENT_TRIMEND; logList_.pop_front());
				logListDisplayedSize_ = 0;
			}
			if (!IsWindowVisible(hwnd)) {
				// 非表示中はサボる
				if (ListBox_GetCount(hList) != 0) {
					ListBox_ResetContent(hList);
				}
				return TRUE;
			} else if (!bDisplayLogList_ && !wParam) {
				// 勢いリスト表示中は差分更新(wParam==FALSE)しない
				return TRUE;
			}
			// 描画を一時停止
			SendMessage(hList, WM_SETREDRAW, FALSE, 0);
			int iTopItemIndex = ListBox_GetTopIndex(hList);
			// wParam!=FALSEのときはリストの内容をリセットする
			if (wParam) {
				ListBox_ResetContent(hList);
				// wParam==2のときはスクロール位置を保存する
				if (wParam != 2) {
					iTopItemIndex = 0;
				}
			}
			if (bDisplayLogList_) {
				// ログリスト表示中
				int iSelItemIndex = ListBox_GetCurSel(hList);
				if (logList_.size() < logListDisplayedSize_ || ListBox_GetCount(hList) != logListDisplayedSize_) {
					ListBox_ResetContent(hList);
					logListDisplayedSize_ = 0;
				}
				// logList_とリストボックスの内容が常に同期するように更新する
				std::list<LOG_ELEM>::const_iterator it = logList_.end();
				for (size_t i = logList_.size() - logListDisplayedSize_; i > 0; --i, --it);
				for (; it != logList_.end(); ++it) {
					TCHAR text[256];
					int len = wsprintf(text, TEXT("%s[%u]{00:00:00 (MMM)}%02d:%02d:%02d (%.3s)%s"),
					         it->marker[0] == TEXT('#') ? TEXT("#") : TEXT(""), static_cast<DWORD>(it->cr),
					         it->st.wHour, it->st.wMinute, it->st.wSecond,
					         it->marker, TEXT("   ") + min(lstrlen(it->marker), 3));
					lstrcpyn(text + len, it->text.c_str(), _countof(text) - len);
					ListBox_AddString(hList, text);
					++logListDisplayedSize_;
				}
				while (logList_.size() > COMMENT_TRIMEND) {
					logList_.pop_front();
					ListBox_DeleteString(hList, 0);
					--logListDisplayedSize_;
					--iSelItemIndex;
					--iTopItemIndex;
				}
				if (iSelItemIndex < 0) {
					ListBox_SetTopIndex(hList, ListBox_GetCount(hList) - 1);
				} else {
					ListBox_SetCurSel(hList, iSelItemIndex);
					ListBox_SetTopIndex(hList, max(iTopItemIndex, 0));
				}
			} else {
				// 勢いリスト表示中
				for (auto it = forceList_.cbegin(); it != forceList_.end(); ++it) {
					TCHAR text[_countof(it->name) + 64];
					wsprintf(text, TEXT("jk%d (%s) 勢い：%d"), it->jkID, it->name, it->force);
					ListBox_AddString(hList, text);
					if (it->jkID == currentJKToGet_) {
						ListBox_SetCurSel(hList, ListBox_GetCount(hList) - 1);
					}
				}
				ListBox_SetTopIndex(hList, iTopItemIndex);
			}
			// 描画を再開
			SendMessage(hList, WM_SETREDRAW, TRUE, 0);
			InvalidateRect(hList, nullptr, FALSE);
		}
		return TRUE;
	case WMS_FORCE:
		{
			static const std::regex reChannel("<((?:bs_)?channel)>([^]*?)</\\1>");
			static const std::regex reChannelRadio("<((?:bs_|radio_)?channel)>([^]*?)</\\1>");
			static const std::regex reVideo("<video>jk(\\d+)</video>");
			static const std::regex reForce("<force>(\\d+)</force>");
			static const std::regex reName("<name>([^<]*)</name>");

			int ret = channelSocket_.ProcessRecv(wParam, lParam, &channelBuf_);
			if (ret == -2) {
				// 切断
				channelBuf_.push_back('\0');
				bool bCleared = false;
				std::cmatch m, mVideo, mForce, mName;
				const char *p = &channelBuf_[FindHttpBody(&channelBuf_[0])];
				const char *pLast = &p[lstrlenA(p)];
				for (; std::regex_search(p, pLast, m, s_.bShowRadio ? reChannelRadio : reChannel); p = m[0].second) {
					if (std::regex_search(m[2].first, m[2].second, mVideo, reVideo) &&
					    std::regex_search(m[2].first, m[2].second, mForce, reForce) &&
					    std::regex_search(m[2].first, m[2].second, mName, reName))
					{
						FORCE_ELEM e;
						e.jkID = atoi(mVideo[1].first);
						e.force = atoi(mForce[1].first);
						int len = MultiByteToWideChar(CP_UTF8, 0, mName[1].first, static_cast<int>(mName[1].length()), e.name, _countof(e.name) - 1);
						e.name[len] = TEXT('\0');
						DecodeEntityReference(e.name);
						if (!bCleared) {
							forceList_.clear();
							bCleared = true;
						}
						forceList_.push_back(e);
					}
				}
				SendMessage(hwnd, WM_UPDATE_LIST, 2, 0);
			}
		}
		return TRUE;
	case WMS_JK:
		{
			static const std::regex reMs("^(?=.*?(?:^|&)done=true(?:&|$)).*?(?:^|&)ms=(\\d+\\.\\d+\\.\\d+\\.\\d+)(?:&|$)");
			static const std::regex reMsPort("(?:^|&)ms_port=(\\d+)(?:&|$)");
			static const std::regex reThreadID("(?:^|&)thread_id=(\\d+)(?:&|$)");
			static const std::regex reUserID("(?:^|&)user_id=(\\d+)(?:&|$)");
			static const std::regex reIsPremium("(?:^|&)is_premium=1(?:&|$)");
			static const std::regex reNickname("(?:^|&)nickname=([0-9A-Za-z]*)");
			static const std::regex reChatNo("^<chat(?= )[^>]*? no=\"(\\d+)\"");
			static const std::regex reChatResult("^<chat_result(?= ).*? status=\"(?!0\")(\\d+)\"");
			static const std::regex reLeaveThreadID("^<leave_thread(?= )(?=.*? reason=\"2\").*? thread=\"(\\d+)\"");

			int ret = jkSocket_.ProcessRecv(wParam, lParam, &jkBuf_);
			if (ret < 0) {
				// 切断
				if (bConnectedToCommentServer_) {
					bConnectedToCommentServer_ = false;
					commentServerResponse_[0] = '\0';
					jkLeaveThreadCheck_ = 0;
					OutputMessageLog(TEXT("コメントサーバとの通信を切断しました。"));
					WriteToLogfile(-1);
				} else if (ret == -2 && currentJK_ == currentJKToGet_) {
					jkBuf_.push_back('\0');
					const char *p = &jkBuf_[FindHttpBody(&jkBuf_[0])];
					std::cmatch mMs, mMsPort, mThreadID;
					if (std::regex_search(p, mMs, reMs) &&
					    std::regex_search(p, mMsPort, reMsPort) &&
					    std::regex_search(p, mThreadID, reThreadID) &&
					    lstrcmpA(jkLeaveThreadID_, mThreadID[1].str().c_str()))
					{
						// コメントサーバに接続
						static const char szRequestTemplate[] = "<thread res_from=\"-10\" version=\"20061206\" thread=\"%.15s\" />";
						char szRequest[_countof(szRequestTemplate) + 16];
						wsprintfA(szRequest, szRequestTemplate, mThreadID[1].str().c_str());
						jkLeaveThreadID_[0] = '\0';
						// '\0'まで送る
						if (jkSocket_.Send(hwnd, WMS_JK, mMs[1].str().c_str(), static_cast<unsigned short>(atoi(mMsPort[1].first)), szRequest, lstrlenA(szRequest) + 1, true)) {
							bConnectedToCommentServer_ = true;
							jkBuf_.clear();
							// コメント投稿のため
							bGetflvIsPremium_ = std::regex_search(p, reIsPremium);
							getflvUserID_[0] = '\0';
							std::cmatch m;
							if (std::regex_search(p, m, reUserID)) {
								lstrcpynA(getflvUserID_, m[1].str().c_str(), _countof(getflvUserID_));
							}
							if (getflvUserID_[0] && std::regex_search(p, m, reNickname)) {
								TCHAR text[128];
								wsprintf(text, TEXT("コメントサーバに接続開始しました(login=%.16S)。"), m[1].str().c_str());
								OutputMessageLog(text);
							} else {
								OutputMessageLog(TEXT("コメントサーバに接続開始しました。"));
							}
						}
					}
				}
			} else {
				// 受信中
				if (bConnectedToCommentServer_) {
					jkBuf_.push_back('\0');
					const char *p = &jkBuf_[0];
					const char *tail = &jkBuf_[jkBuf_.size() - 1];
					bool bRead = false;
					bool bCarried = false;
					while (p < tail) {
						// ログで特別な意味をもつため改行文字は数値文字参照に置換
						int len, rplLen;
						char rpl[CHAT_TAG_MAX + 16];
						for (len = 0, rplLen = 0; p[len]; ++len) {
							if (rplLen < CHAT_TAG_MAX) {
								if (p[len] == '\n' || p[len] == '\r') {
									rplLen += wsprintfA(&rpl[rplLen], "&#%d;", p[len]);
								} else {
									rpl[rplLen++] = p[len];
								}
							}
						}
						rpl[rplLen] = '\0';

						if (&p[len] == tail) {
							// タグの途中でパケット分割される場合があるため繰り越し
							jkBuf_.pop_back();
							jkBuf_.erase(jkBuf_.begin(), jkBuf_.end() - len);
							bCarried = true;
							break;
						}
						// 指定ファイル再生中は混じると鬱陶しいので表示しない。後退指定はある程度反映
						if (ProcessChatTag(rpl, !bSpecFile_, min(max(-forwardOffset_, 0), 30000))) {
							dprintf(TEXT("#")); // DEBUG
							WriteToLogfile(currentJK_, rpl);
							bRead = true;
						}
						std::cmatch m;
						if (std::regex_search(rpl, m, reChatNo)) {
							// コメント投稿のために最新コメントのコメ番を記録
							lastChatNo_ = atoi(m[1].first);
						} else if (!commentServerResponse_[0] && !StrCmpNA(rpl, "<thread ", 8)) {
							// コメント投稿のために接続応答を記録。これが空文字列でない間は投稿可能
							lstrcpynA(commentServerResponse_, rpl, _countof(commentServerResponse_));
							commentServerResponseTick_ = GetTickCount();
						} else if (std::regex_search(rpl, m, reChatResult)) {
							// コメント投稿失敗の応答を取得した
							TCHAR text[64];
							wsprintf(text, TEXT("Error:コメント投稿に失敗しました(status=%d)。"), atoi(m[1].first));
							OutputMessageLog(text);
						} else if (std::regex_search(rpl, m, reLeaveThreadID)) {
							// leave_thread reason="2"(≒4時リセット?)により切断されようとしている
							lstrcpynA(jkLeaveThreadID_, m[1].str().c_str(), _countof(jkLeaveThreadID_));
							// たまにサーバから切断されない場合があるため
							jkLeaveThreadCheck_ = 2;
						}
#ifdef _DEBUG
						TCHAR debug[512];
						int debugLen = MultiByteToWideChar(CP_UTF8, 0, p, -1, debug, _countof(debug) - 1);
						debug[debugLen] = TEXT('\0');
						dprintf(TEXT("%s\n"), debug); // DEBUG
#endif
						p += len + 1;
					}
					if (!bCarried) {
						jkBuf_.clear();
					}
					if (bRead && bDisplayLogList_) {
						SendMessage(hwnd, WM_UPDATE_LIST, FALSE, 0);
					}
				}
			}
		}
		return TRUE;
	case WMS_POST:
		{
			static const std::regex rePostkey("postkey=([0-9A-Za-z\\-_.]+)");
			static const std::regex reThread("^<thread[^>]*? thread=\"(\\d+)\"");
			static const std::regex reTicket("^<thread[^>]*? ticket=\"(.+?)\"");
			static const std::regex reServerTime("^<thread[^>]*? server_time=\"(\\d+)\"");
			static const std::regex reMailIsValid("[0-9A-Za-z #]*");

			int ret = postSocket_.ProcessRecv(wParam, lParam, &postBuf_);
			if (ret == -2) {
				// 切断
				postBuf_.push_back('\0');
				std::cmatch mPostkey, mThread, mTicket, mServerTime;
				const char *p = &postBuf_[FindHttpBody(&postBuf_[0])];
				if (std::regex_search(p, mPostkey, rePostkey) &&
				    std::regex_search(commentServerResponse_, mThread, reThread) &&
				    std::regex_search(commentServerResponse_, mTicket, reTicket) &&
				    std::regex_search(commentServerResponse_, mServerTime, reServerTime) &&
				    getflvUserID_[0] && GetTickCount() - lastPostTick_ >= POST_COMMENT_INTERVAL)
				{
					// コメント欄を文字コード変換
					TCHAR comm[POST_COMMENT_MAX], mail[64];
					GetPostComboBoxText(comm, _countof(comm), mail, _countof(mail));
					// Tab文字orレコードセパレータ->改行
					for (LPTSTR q = comm; *q; ++q) {
						if (*q == TEXT('\t') || *q == TEXT('\x1e')) *q = TEXT('\n');
					}
					char u8comm[_countof(comm) * 3];
					char u8mail[_countof(mail) * 3];
					char u8commEnc[_countof(comm) * 5];
					int len = WideCharToMultiByte(CP_UTF8, 0, comm, -1, u8comm, _countof(u8comm) - 1, nullptr, nullptr);
					u8comm[len] = '\0';
					len = WideCharToMultiByte(CP_UTF8, 0, mail, -1, u8mail, _countof(u8mail) - 1, nullptr, nullptr);
					u8mail[len] = '\0';
					EncodeEntityReference(u8comm, u8commEnc, _countof(u8commEnc));
					// vposは10msec単位。内部時計のずれに影響されないようにサーバ時刻を基準に補正
					int vpos = (int)((LONGLONG)strtoul(mServerTime[1].first, nullptr, 10) - strtoul(mThread[1].first, nullptr, 10)) * 100 +
					           (int)(GetTickCount() - commentServerResponseTick_) / 10;
					if (std::regex_match(u8mail, reMailIsValid) && vpos >= 0) {
						// コメント投稿
						static const char szRequestTemplate[] =
							"<chat thread=\"%.15s\" ticket=\"%.15s\" vpos=\"%d\" postkey=\"%.40s\" mail=\"%s%s\" user_id=\"%s\" premium=\"%d\" staff=\"0\">%s</chat>";
						char szRequest[_countof(szRequestTemplate) + _countof(u8commEnc) + _countof(u8mail) + _countof(getflvUserID_) + 256];
						wsprintfA(szRequest, szRequestTemplate, mThread[1].str().c_str(), mTicket[1].str().c_str(), vpos, mPostkey[1].str().c_str(),
						          u8mail, s_.bAnonymity ? " 184" : "", getflvUserID_, (int)bGetflvIsPremium_, u8commEnc);
						// '\0'まで送る
						if (jkSocket_.Send(hwnd, WMS_JK, nullptr, 0, szRequest, lstrlenA(szRequest) + 1, true)) {
							lastPostTick_ = GetTickCount();
							GetPostComboBoxText(lastPostComm_, _countof(lastPostComm_));
							// アンドゥできるように選択削除で消す
							if (SendDlgItemMessage(hwnd, IDC_CB_POST, CB_SETEDITSEL, 0, MAKELPARAM(0, -1)) == TRUE) {
								SendDlgItemMessage(hwnd, IDC_CB_POST, WM_CLEAR, 0, 0);
							}
#ifdef _DEBUG
							OutputDebugStringA((std::string("##POST##") + szRequest + "\n").c_str());
#endif
							return TRUE;
						}
					}
				}
				OutputMessageLog(TEXT("Error:コメント投稿をキャンセルしました。"));
			}
		}
		return TRUE;
	case WM_SET_ZORDER:
		// 全画面や最大化時は前面のほうが都合がよいはず
		if ((s_.hideForceWindow & 4) || m_pApp->GetFullscreen() || (GetWindowLong(m_pApp->GetAppWindow(), GWL_STYLE) & WS_MAXIMIZE)) {
			// TVTestウィンドウの前面にもってくる
			SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
			SetWindowPos(hwnd, m_pApp->GetFullscreen() || m_pApp->GetAlwaysOnTop() ? HWND_TOPMOST : HWND_TOP,
			             0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
		} else {
			// TVTestウィンドウの背面にもってくる
			SetWindowPos(hwnd, m_pApp->GetAppWindow(), 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
		}
		return TRUE;
	case WM_POST_COMMENT:
		{
			TCHAR comm[POST_COMMENT_MAX + 1];
			if (GetDlgItemText(hwnd, IDC_CB_POST, comm, _countof(comm)) && comm[0] == TEXT('@')) {
				// ローカルコマンドとして処理
				ProcessLocalPost(&comm[1]);
				return TRUE;
			}
			GetPostComboBoxText(comm, _countof(comm));
			if (GetTickCount() - lastPostTick_ < POST_COMMENT_INTERVAL) {
				OutputMessageLog(TEXT("Error:投稿間隔が短すぎます。"));
			} else if (lstrlen(comm) >= POST_COMMENT_MAX) {
				OutputMessageLog(TEXT("Error:投稿コメントが長すぎます。"));
			} else if (comm[0] && !lstrcmp(comm, lastPostComm_)) {
				OutputMessageLog(TEXT("Error:投稿コメントが前回と同じです。"));
			} else if (comm[0]) {
				static const std::regex reThread("^<thread[^>]*? thread=\"(\\d+)\"");
				std::cmatch mThread;
				if (!std::regex_search(commentServerResponse_, mThread, reThread) || !getflvUserID_[0]) {
					OutputMessageLog(TEXT("Error:コメントサーバに接続していないかログインしていません。"));
				} else {
					// ポストキー取得開始
					char szGet[_countof(cookie_) + 256];
					wsprintfA(szGet, "GET /api/v2/getpostkey?thread=%.15s&block_no=%d HTTP/1.1\r\n", mThread[1].str().c_str(), (lastChatNo_ + 1) / 100);
					AppendHttpHeader(szGet, "Host: ", JK_HOST_NAME, "\r\n");
					AppendHttpHeader(szGet, "Cookie: ", cookie_, "\r\n");
					AppendHttpHeader(szGet, "Connection: ", "close", "\r\n\r\n");
					if (postSocket_.Send(hwnd, WMS_POST, JK_HOST_NAME, 80, szGet)) {
						postBuf_.clear();
					}
				}
			}
		}
		return TRUE;
	case WM_SHOWWINDOW:
		m_pApp->SetPluginCommandState(COMMAND_HIDE_FORCE, wParam != 0 ? TVTest::COMMAND_ICON_STATE_CHECKED : 0);
		// FALL THROUGH!
	case WM_SIZE:
		{
			RECT rcParent, rc;
			GetClientRect(hwnd, &rcParent);
			HWND hItem = GetDlgItem(hwnd, IDC_CB_POST);
			GetWindowRect(hItem, &rc);
			MapWindowPoints(nullptr, hwnd, reinterpret_cast<LPPOINT>(&rc), 2);
			int padding = rc.left;
			if (!cookie_[0]) {
				// クッキーが設定されていなければ間違いなく投稿不能なので入力ボックスを表示しない
				SetWindowPos(hItem, nullptr, rc.left, rcParent.bottom, rcParent.right-rc.left*2, rc.bottom-rc.top, SWP_NOZORDER);
			} else {
				padding += 6 + static_cast<int>(SendMessage(hItem, CB_GETITEMHEIGHT, static_cast<WPARAM>(-1), 0));
				SetWindowPos(hItem, nullptr, rc.left, rcParent.bottom-padding, rcParent.right-rc.left*2, rc.bottom-rc.top, SWP_NOZORDER);
			}
			hItem = GetDlgItem(hwnd, IDC_FORCELIST);
			GetWindowRect(hItem, &rc);
			MapWindowPoints(nullptr, hwnd, reinterpret_cast<LPPOINT>(&rc), 2);
			if (cookie_[0]) {
				// ボタン類が入力ボックスと被らないようにする
				int swShow = rcParent.bottom-rc.top-padding < -4 ? SW_HIDE : SW_SHOW;
				if (uMsg == WM_SHOWWINDOW || (GetWindowLong(GetDlgItem(hwnd, IDC_RADIO_FORCE), GWL_STYLE) & WS_VISIBLE ? true : false) != (swShow != SW_HIDE)) {
					ShowWindow(GetDlgItem(hwnd, IDC_RADIO_FORCE), swShow);
					ShowWindow(GetDlgItem(hwnd, IDC_RADIO_LOG), swShow);
					ShowWindow(GetDlgItem(hwnd, IDC_CHECK_SPECFILE), swShow);
					ShowWindow(GetDlgItem(hwnd, IDC_CHECK_RELATIVE), swShow);
					ShowWindow(GetDlgItem(hwnd, IDC_SLIDER_OPACITY), swShow);
				}
			}
			SetWindowPos(hItem, nullptr, 0, 0, rcParent.right-rc.left*2, rcParent.bottom-rc.top-padding, SWP_NOMOVE | SWP_NOZORDER);
		}
		break;
	}
	return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

// ストリームコールバック(別スレッド)
BOOL CALLBACK CNicoJK::StreamCallback(BYTE *pData, void *pClientData)
{
	CNicoJK *pThis = static_cast<CNicoJK*>(pClientData);
	int pid = ((pData[1]&0x1F)<<8) | pData[2];
	BYTE bTransportError = pData[1]&0x80;
	BYTE bPayloadUnitStart = pData[1]&0x40;
	BYTE bHasAdaptation = pData[3]&0x20;
	BYTE bHasPayload = pData[3]&0x10;
	BYTE bAdaptationLength = pData[4];
	BYTE bPcrFlag = pData[5]&0x10;

	// シークやポーズを検出するためにPCRを調べる
	if (bHasAdaptation && bAdaptationLength >= 5 && bPcrFlag && !bTransportError) {
		DWORD pcr = (static_cast<DWORD>(pData[5+1])<<24) | (pData[5+2]<<16) | (pData[5+3]<<8) | pData[5+4];
		// 参照PIDのPCRが現れることなく5回別のPCRが出現すれば、参照PIDを変更する
		if (pid != pThis->pcrPid_) {
			int i = 0;
			for (; pThis->pcrPids_[i] >= 0; ++i) {
				if (pThis->pcrPids_[i] == pid) {
					if (++pThis->pcrPidCounts_[i] >= 5) {
						pThis->pcrPid_ = pid;
					}
					break;
				}
			}
			if (pThis->pcrPids_[i] < 0 && i + 1 < _countof(pThis->pcrPids_)) {
				pThis->pcrPids_[i] = pid;
				pThis->pcrPidCounts_[i] = 1;
				pThis->pcrPids_[++i] = -1;
			}
		}
		if (pid == pThis->pcrPid_) {
			pThis->pcrPids_[0] = -1;
		}
		//dprintf(TEXT("CNicoJK::StreamCallback() PCR\n")); // DEBUG
		CBlockLock lock(&pThis->streamLock_);
		DWORD tick = GetTickCount();
		// 2秒以上PCRを取得できていない→ポーズから回復?
		bool bReset = tick - pThis->pcrTick_ >= 2000;
		pThis->pcrTick_ = tick;
		if (pid == pThis->pcrPid_) {
			long long pcrDiff = DWORD_MSB(pcr - pThis->pcr_) ? -static_cast<long long>(pThis->pcr_ - pcr) : static_cast<long long>(pcr - pThis->pcr_);
			// ラップアラウンド近傍を特別扱いする必要はない(またいでシークする場合だってある)

			if (bReset || 0 <= pcrDiff && pcrDiff < 45000) {
				// 1秒以内は通常の再生と見なす
			} else if (abs(pcrDiff) < 15 * 60 * 45000) {
				// -15～0分、+1秒～15分PCRが飛んでいる場合、シークとみなし、
				// シークした分だけTOTをずらして読み込み直す
				if (pThis->ftTot_[0].dwHighDateTime != 0xFFFFFFFF && pThis->ftTot_[2].dwHighDateTime != 0xFFFFFFFF) {
					long long totDiff = pcrDiff * FILETIME_MILLISECOND / 45;
					pThis->ftTot_[0] += totDiff;
					if (pThis->ftTot_[1].dwHighDateTime != 0xFFFFFFFF) {
						pThis->ftTot_[1] += totDiff;
					}
					// 保留中のTOTはシーク後に取得した可能性があるので捨てる(再生速度の推定が狂ってコメントが大量に流れたりするのを防ぐため)
					pThis->ftTot_[2].dwHighDateTime = 0xFFFFFFFE;
					pThis->bResyncComment_ = true;
				} else {
					bReset = true;
				}
			} else {
				// それ以上飛んでたら別ストリームと見なしてリセット
				bReset = true;
			}
			// 保留中のTOT(要素2)はPCRの取得後に利用可能(要素0にシフト)にする
			if (pThis->ftTot_[0].dwHighDateTime != 0xFFFFFFFF && pThis->ftTot_[2].dwHighDateTime != 0xFFFFFFFE) {
				if (pThis->ftTot_[2].dwHighDateTime != 0xFFFFFFFF) {
					pThis->ftTot_[1] = pThis->ftTot_[0];
					pThis->ftTot_[0] = pThis->ftTot_[2];
					pThis->totTick_[1] = pThis->totTick_[0];
					pThis->totTick_[0] = pThis->totTick_[2];
				}
				pThis->ftTot_[2].dwHighDateTime = 0xFFFFFFFE;
			}
			pThis->pcr_ = pcr;
		}
		if (bReset) {
			pThis->ftTot_[0].dwHighDateTime = 0xFFFFFFFF;
			PostMessage(pThis->hForce_, WM_RESET_STREAM, 0, 0);
		}
	}

	// TOTパケットは地上波の実測で6秒に1個程度
	// ARIB規格では最低30秒に1個
	if (pid == 0x14 && bPayloadUnitStart && bHasPayload && !bTransportError) {
		BYTE *pPayload = pData + 4;
		if (bHasAdaptation) {
			// アダプテーションフィールドをスキップする
			if (bAdaptationLength > 182) {
				pPayload = nullptr;
			} else {
				pPayload += 1 + bAdaptationLength;
			}
		}
		if (pPayload) {
			BYTE *pTable = pPayload + 1 + pPayload[0];
			// TOT or TDT (ARIB STD-B10)
			if (pTable + 7 < pData + 188 && (pTable[0] == 0x73 || pTable[0] == 0x70)) {
				// TOT時刻とTickカウントを記録する
				SYSTEMTIME st;
				FILETIME ft;
				if (AribToSystemTime(&pTable[3], &st) && SystemTimeToFileTime(&st, &ft)) {
					// UTCに変換
					ft += -32400000LL * FILETIME_MILLISECOND;
					dprintf(TEXT("CNicoJK::StreamCallback() TOT\n")); // DEBUG
					CBlockLock lock(&pThis->streamLock_);
					// 時刻が変化したときだけ
					if (ft - pThis->ftTot_[0] != 0) {
						pThis->ftTot_[2] = ft;
						pThis->totTick_[2] = GetTickCount();
						if (pThis->ftTot_[0].dwHighDateTime == 0xFFFFFFFF) {
							pThis->ftTot_[0] = pThis->ftTot_[2];
							pThis->totTick_[0] = pThis->totTick_[2];
							pThis->ftTot_[1].dwHighDateTime = 0xFFFFFFFF;
							pThis->ftTot_[2].dwHighDateTime = 0xFFFFFFFF;
						}
					}
				}
			}
		}
	}
	return TRUE;
}

TVTest::CTVTestPlugin *CreatePluginClass()
{
	return new CNicoJK();
}
