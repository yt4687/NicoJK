#include "stdafx.h"
#include "TextFileReader.h"

CTextFileReader::CTextFileReader()
	: hFile_(INVALID_HANDLE_VALUE)
	, bEof_(false)
{
	buf_[0] = '\0';
}

CTextFileReader::~CTextFileReader()
{
	Close();
}

bool CTextFileReader::Open(LPCTSTR path, DWORD shareMode, DWORD flagsAndAttributes)
{
	Close();
	hFile_ = CreateFile(path, GENERIC_READ, shareMode, nullptr, OPEN_EXISTING, flagsAndAttributes, nullptr);
	return IsOpen();
}

void CTextFileReader::Close()
{
	if (IsOpen()) {
		CloseHandle(hFile_);
		hFile_ = INVALID_HANDLE_VALUE;
	}
	bEof_ = false;
	buf_[0] = '\0';
}

// ファイルポインタを先頭に戻す
bool CTextFileReader::ResetPointer()
{
	if (IsOpen()) {
		if (SetFilePointer(hFile_, 0, nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER) {
			bEof_ = false;
			buf_[0] = '\0';
			return true;
		}
	}
	return false;
}

// 1行またはNULを含む最大textMax(>0)バイト読み込む
// 改行文字は取り除く
// 戻り値はNULを含む読み込まれたバイト数、終端に達すると0を返す
size_t CTextFileReader::ReadLine(char *text, size_t textMax)
{
	if (!IsOpen()) {
		return 0;
	}
	size_t textLen = 0;
	for (;;) {
		if (!bEof_) {
			size_t bufLen = strlen(buf_);
			DWORD read;
			if (!ReadFile(hFile_, buf_ + bufLen, static_cast<DWORD>(BUF_SIZE - bufLen - 1), &read, nullptr)) {
				buf_[bufLen] = '\0';
				bEof_ = true;
			} else {
				buf_[bufLen + read] = '\0';
				if (strlen(buf_) < BUF_SIZE - 1) {
					bEof_ = true;
				}
			}
		}
		if (!textLen && !buf_[0]) {
			return 0;
		}
		size_t lineLen = strcspn(buf_, "\n");
		size_t copyNum = min(lineLen, textMax - textLen - 1);
		strncpy_s(text + textLen, textMax - textLen, buf_, copyNum);
		textLen += copyNum;
		if (lineLen < BUF_SIZE - 1) {
			if (buf_[lineLen] == '\n') ++lineLen;
			memmove(buf_, buf_ + lineLen, sizeof(buf_) - lineLen);
			if (textLen >= 1 && text[textLen-1] == '\r') {
				text[--textLen] = '\0';
			}
			return textLen + 1;
		}
		buf_[0] = '\0';
	}
}

// 最終行を1行またはNULを含む最大textMax(>0)バイト(収まらない場合行頭側をカット)読み込む
// 改行文字は取り除く
// ファイルポインタは先頭に戻る
// 戻り値はNULを含む読み込まれたバイト数
size_t CTextFileReader::ReadLastLine(char *text, size_t textMax)
{
	if (!IsOpen()) {
		return 0;
	}
	// 2GB以上には対応しない
	size_t fileSize = GetFileSize(hFile_, nullptr);
	if (fileSize > 0x7FFFFFFF ||
	    SetFilePointer(hFile_, -static_cast<int>(min(textMax - 1, fileSize)), nullptr, FILE_END) == INVALID_SET_FILE_POINTER) {
		return 0;
	}
	DWORD read;
	if (!ReadFile(hFile_, text, static_cast<DWORD>(textMax - 1), &read, nullptr)) {
		ResetPointer();
		return 0;
	}
	text[read] = '\0';
	size_t textLen = strlen(text);
	if (textLen >= 1 && text[textLen-1] == '\n') {
		text[--textLen] = '\0';
	}
	if (textLen >= 1 && text[textLen-1] == '\r') {
		text[--textLen] = '\0';
	}
	for (size_t i = textLen; i > 0; --i) {
		if (text[i - 1] == '\n') {
			memmove(text, text + i, textLen - i + 1);
			textLen -= i;
			break;
		}
	}
	ResetPointer();
	return textLen + 1;
}

// 現在位置からファイルサイズ/scaleだけシークする
// 戻り値はファイルポインタの移動バイト数
int CTextFileReader::Seek(int scale)
{
	if (!IsOpen() || scale == 0) {
		return 0;
	}
	DWORD fileSize = GetFileSize(hFile_, nullptr);
	DWORD filePos = SetFilePointer(hFile_, 0, nullptr, FILE_CURRENT);
	if (fileSize > 0x7FFFFFFF || filePos == INVALID_SET_FILE_POINTER) {
		return 0;
	}
	LONGLONG llNextPos = static_cast<LONGLONG>(fileSize) / (scale < 0 ? -scale : scale) * (scale < 0 ? -1 : 1) + filePos;
	DWORD nextPos = llNextPos < 0 ? 0 : llNextPos >= fileSize ? filePos : static_cast<DWORD>(llNextPos);
	if (nextPos == filePos) {
		return 0;
	}
	nextPos = SetFilePointer(hFile_, nextPos, nullptr, FILE_BEGIN);
	if (nextPos == INVALID_SET_FILE_POINTER) {
		return 0;
	}
	bEof_ = false;
	buf_[0] = '\0';
	return static_cast<int>(nextPos) - static_cast<int>(filePos);
}
