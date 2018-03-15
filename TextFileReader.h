#pragma once

// マルチバイトテキストファイル読み込み(ReadFileのラッパ)
class CTextFileReader
{
public:
	static const size_t BUF_SIZE = 512;
	CTextFileReader();
	~CTextFileReader();
	bool Open(LPCTSTR path, DWORD shareMode, DWORD flagsAndAttributes);
	void Close();
	bool ResetPointer();
	size_t ReadLine(char *text, size_t textMax);
	size_t ReadLastLine(char *text, size_t textMax);
	int Seek(int scale);
	bool IsOpen() const { return hFile_ != INVALID_HANDLE_VALUE; }
private:
	HANDLE hFile_;
	bool bEof_;
	char buf_[BUF_SIZE];
};
