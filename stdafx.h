﻿// stdafx.h : 標準のシステム インクルード ファイルのインクルード ファイル、または
// 参照回数が多く、かつあまり変更されない、プロジェクト専用のインクルード ファイル
// を記述します。
//

#pragma once

#ifndef WINVER
#define WINVER 0x0501		// Windows XP
#endif

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0501	// Windows XP
#endif

#ifndef _WIN32_IE
#define _WIN32_IE 0x0600	// Internet Explorer 6.0
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <WindowsX.h>
#include <MMSystem.h>
#include <objbase.h>
#include <GdiPlus.h>
#include <vector>
#include <list>
#include <regex>
#include <memory>
#include <utility>
#include <algorithm>
#include <tchar.h>
#include <dwmapi.h>
#include <CommDlg.h>
#include <ShellAPI.h>
#include <process.h>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "imm32.lib")

// NOMINMAXではGdiPlus.hが通らない
#undef min
#undef max
using std::min;
using std::max;

typedef std::basic_string<TCHAR> tstring;
