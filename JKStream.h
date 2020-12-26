#pragma once
#include "Util.h"

// 実況データ送受信用のプロセスと通信する
class CJKStream
{
public:
	CJKStream();
	~CJKStream();
	void BeginClose();
	void Close();
	bool Send(HWND hwnd, UINT msg, char command, const char *buf);
	int ProcessRecv(std::vector<char> &recvBuf);
	bool Shutdown();
private:
	bool CreateWorker(HWND hwnd, UINT msg);
	static bool CreateJKProcess(HANDLE &hProcess, HANDLE &hAsyncReadPipe, HANDLE &hWritePipe);
	static unsigned int __stdcall WorkerThread(void *pParam);
	void WorkerThread_();

	recursive_mutex_ workerLock_;
	HANDLE hWorkerThread_;
	HANDLE hWorkerEvent_;
	HANDLE hProcess_;
	bool bWorkerCreated_;
	bool bContinueWorker_;
	bool bStopWroker_;
	bool bOpened_;
	bool bShutdown_;
	bool bShutdownSent_;
	HWND hwnd_;
	UINT msg_;
	std::vector<char> sendBuf_;
	std::vector<char> recvBuf_;
};
