﻿#pragma once

// プラグインクラス
class CNicoJK : public TVTest::CTVTestPlugin
{
public:
	// リストボックスのログ表示の最大数
	static const int COMMENT_TRIMEND = 1000;
	// リストボックスのログ表示の最小描画間隔
	static const int COMMENT_REDRAW_INTERVAL = 250;
	// 処理できるchatタグの最大文字数(char)
	static const int CHAT_TAG_MAX = 4096;
	// 表示できるコメントの最大文字数
	static const int CHAT_TEXT_MAX = 2048;
	// 設定値の最大読み込み文字数
	static const int SETTING_VALUE_MAX = 8192;
	// 勢いリストを更新する間隔(あんまり短くしちゃダメ!)
	static const int UPDATE_FORCE_INTERVAL = 20000;
	// コメントサーバ切断をチェックして再接続する間隔(あんまり短くしちゃダメ!)
	static const int JK_WATCHDOG_INTERVAL = 20000;
	// ログファイルフォルダの更新をチェックする間隔
	static const int READ_LOG_FOLDER_INTERVAL = 3000;
	// チャンネル変更などの後に適当な実況IDのチェックを行うまでの猶予
	static const int SETUP_CURJK_DELAY = 3000;
	// 投稿できる最大コメント文字数(たぶん安易に変更しないほうがいい)
	static const int POST_COMMENT_MAX = 76;
	// 連投制限(短いと規制されるとのウワサ)
	static const int POST_COMMENT_INTERVAL = 2000;
	// CTVTestPlugin
	CNicoJK();
	bool GetPluginInfo(TVTest::PluginInfo *pInfo);
	bool Initialize();
	bool Finalize();
private:
	struct SETTINGS {
		int hideForceWindow;
		int forceFontSize;
		TCHAR forceFontName[LF_FACESIZE];
		int timerInterval;
		int halfSkipThreshold;
		int commentLineMargin;
		int commentFontOutline;
		int commentSize;
		int commentSizeMin;
		int commentSizeMax;
		TCHAR commentFontName[LF_FACESIZE];
		TCHAR commentFontNameMulti[LF_FACESIZE];
		bool bCommentFontBold;
		bool bCommentFontAntiAlias;
		int commentDuration;
		int commentDrawLineCount;
		int logfileMode;
		tstring logfileDrivers;
		tstring nonTunerDrivers;
		tstring logfileFolder;
		tstring execGetCookie;
		tstring execGetV10Key;
		std::string channelsUri;
		tstring mailDecorations;
		bool bAnonymity;
		bool bUseOsdCompositor;
		bool bUseTexture;
		bool bUseDrawingThread;
		bool bSetChannel;
		int maxAutoReplace;
		tstring abone;
		int dropLogfileMode;
		int defaultPlaybackDelay;
		int forwardList[26];
		RECT rcForce;
		int forceOpacity;
		int commentOpacity;
		int headerMask;
		bool bSetRelative;
		bool bUsePanel;
	};
	struct FORCE_ELEM {
		int jkID;
		int force;
		tstring name;
		std::string chatStreamID;
	};
	struct LOG_ELEM {
		SYSTEMTIME st;
		int no;
		COLORREF cr;
		TCHAR marker[28];
		tstring text;
	};
	struct RPL_ELEM {
		int key;
		tstring section;
		tstring comment;
		tstring pattern;
		std::regex re;
		std::string fmt;
		bool IsEnabled() const { return !pattern.empty() && TEXT('a') <= pattern[0] && pattern[0] <= TEXT('z'); }
		void SetEnabled(bool b);
		bool SetPattern(LPCTSTR patt);
	};
	bool TogglePlugin(bool bEnabled);
	void ToggleStreamCallback(bool bSet);
	static unsigned int __stdcall SyncThread(void *pParam);
	void LoadFromIni();
	void SaveToIni();
	void LoadForceListFromIni();
	void LoadRplListFromIni(LPCTSTR section, std::vector<RPL_ELEM> *pRplList);
	void SaveRplListToIni(LPCTSTR section, const std::vector<RPL_ELEM> &rplList, bool bClearSection = true);
	HWND GetFullscreenWindow();
	HWND FindVideoContainer();
	DWORD GetCurrentNetworkServiceID();
	bool GetChannelNetworkServiceID(int tuningSpace, int channelIndex, DWORD *pNtsID);
	bool GetCurrentTot(FILETIME *pft);
	bool IsMatchDriverName(LPCTSTR drivers);
	void WriteToLogfile(int jkID, const char *text = nullptr);
	bool ReadFromLogfile(int jkID, char *text = nullptr, int len = 0, unsigned int tmToRead = 0);
	static LRESULT CALLBACK EventCallback(UINT Event, LPARAM lParam1, LPARAM lParam2, void *pClientData);
	static BOOL CALLBACK WindowMsgCallback(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT *pResult, void *pUserData);
	bool ProcessChatTag(const char *tag, bool bShow = true, int showDelay = 0);
	void OutputMessageLog(LPCTSTR text);
	void GetPostComboBoxText(LPTSTR comm, size_t commSize, LPTSTR mail = nullptr, size_t mailSize = 0);
	void ProcessLocalPost(LPCTSTR comm);
	static LRESULT CALLBACK PanelWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
	static LRESULT CALLBACK ForceWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
	bool CreateForceWindowItems(HWND hwnd);
	LRESULT ForceWindowProcMain(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
	static BOOL CALLBACK StreamCallback(BYTE *pData, void *pClientData);

	// 設定ファイルの名前
	tstring iniFileName_;
	SETTINGS s_;
	std::vector<NETWORK_SERVICE_ID_ELEM> ntsIDList_;
	std::vector<RPL_ELEM> rplList_;
	char cookie_[2048];
	bool bDragAcceptFiles_;

	// 勢い窓
	HWND hPanel_;
	HWND hForce_;
	HFONT hForceFont_;
	bool bDisplayLogList_;
	std::vector<FORCE_ELEM> forceList_;
	std::list<LOG_ELEM> logList_;
	size_t logListDisplayedSize_;
	bool bPendingTimerUpdateList_;
	DWORD lastUpdateListTick_;
	tstring lastCalcText_;
	int lastCalcWidth_;

	// コメント描画ウィンドウ
	CCommentWindow commentWindow_;
	DWORD forwardTick_;
	HANDLE hSyncThread_;
	bool bQuitSyncThread_;
	bool bPendingTimerForward_;
	bool bHalfSkip_;
	bool bFlipFlop_;
	int forwardOffset_;
	int forwardOffsetDelta_;

	// 通信用
	CJKStream channelStream_;
	CJKStream jkStream_;
	std::vector<char> channelBuf_;
	std::vector<char> jkBuf_;
	int currentJKToGet_;
	int currentJK_;
	DWORD lastPostTick_;
	TCHAR lastPostComm_[POST_COMMENT_MAX];

	// 過去ログ関係
	bool bRecording_;
	bool bUsingLogfileDriver_;
	bool bSetStreamCallback_;
	bool bResyncComment_;
	int currentLogfileJK_;
	HANDLE hLogfile_;
	HANDLE hLogfileLock_;
	int currentReadLogfileJK_;
	CTextFileReader readLogfile_;
	char readLogText_[CHAT_TAG_MAX];
	unsigned int tmReadLogText_;
	DWORD readLogfileTick_;
	FILETIME ftTot_[3];
	DWORD totTick_[3];
	DWORD pcr_;
	DWORD pcrTick_;
	int pcrPid_;
	int pcrPids_[8];
	int pcrPidCounts_[8];
	recursive_mutex_ streamLock_;

	// 指定ファイル再生
	bool bSpecFile_;
	tstring tmpSpecFileName_;
	tstring dropFileName_;
	int dropFileTimeout_;
};
