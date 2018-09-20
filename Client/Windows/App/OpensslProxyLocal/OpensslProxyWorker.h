#pragma once

typedef struct tagWorkerContext
{
	PSOCK_MGR_S				pstArryWorker[MGR_ARRYNUMS];		/*工作线程最大数量*/
    UINT32                            usMsgPortNum;                                  /*通信端口的计数，避免重复*/
	UINT32							uiWorkerNums;									/*工作的线程个数*/
	CRITICAL_SECTION			stWorkerLock;										/*统一锁*/
}WORKER_CTX_S, *PWORKER_CTX_S;

WORKER_CTX_S *OpensslProxy_NetworkEventWorkerCreate();

VOID OpensslProxy_NetworkEventWorkerRelease(PWORKER_CTX_S pstWorker);



