#include <Winsock2.h>
#include <Windows.h>
#include <WS2tcpip.h>
#include <mswsock.h>
#include <stdio.h>
#include <process.h>
#include "../common/CLog.h"
#include "../common/CommDef.h"
#include "../common/CommBizDefine.h"
#include "../common/Sem.h"
#include "../common/Queue.h"
#include "../common/CommIoBuf.h"
#include "OpensslProxyTlsHandler.h"
#include "OpensslProxyWorker.h"
#include "OpensslProxyPacketDispatch.h"
#include "OpenSSLProxyMgr.h"
#include "OpensslProxyMsgCtrl.h"


/******************************************PerSocket的信息********************************************************/
/*为了接收其它线程消息，方便访问，修改为全局变量*/
WORKER_CTX_S *g_pstWorker = NULL;


/*将内容进行判断*/
BOOLEAN OpensslProxy_IsPacketProxyPermit()
{
	return TRUE;
}


/*代理Socket处理*/
INT32	OpensslProxy_NetworkEventProxyHandler(SOCK_MGR_S*         pstSockMgr, PPERSOCKINFO_S	pstProxySockInfo)
{
	return SYS_OK;
}

/*本地Socket处理*/
INT32	OpensslProxy_NetworkEventLocalHandler(SOCK_MGR_S*         pstSockMgr, PPERSOCKINFO_S	pstLocalSockInfo)
{
	INT32                       iError = 0;
	INT32                       iRet	  = 0;
	CHAR                       acRecvBuf[MSG_RECVBUF] = { 0 };

	/*加密的处理流程*/
	if ( pstLocalSockInfo->bIsTls )
	{
		if ( NULL == pstLocalSockInfo->stTlsInfo.pstSsl )
		{
			pstLocalSockInfo->stTlsInfo.pstSsl = SSL_new(pstSockMgr->pstTlsCtxServer);
			if (NULL == pstLocalSockInfo->stTlsInfo.pstSsl)
			{
				CLOG_writelog_level("LPXY", CLOG_LEVEL_ERROR, "Local socket SSL_new, ssl getError=%d, sockfd=%d", ERR_get_error(), pstLocalSockInfo->sSockfd);
				ERR_print_errors_fp(stdout);
				return SYS_ERR;
			}
			else
			{
				SSL_set_fd(pstLocalSockInfo->stTlsInfo.pstSsl, (int)pstLocalSockInfo->sSockfd);
				iError = SSL_accept(pstLocalSockInfo->stTlsInfo.pstSsl);
				if (iError == -1)
				{
					iRet = SSL_get_error(pstLocalSockInfo->stTlsInfo.pstSsl, iError);
					//iRet = ERR_get_error();
					//ERR_print_errors_fp(stdout);
					//if ( NULL == ERR_lib_error_string(iRet) 
					if (SSL_ERROR_WANT_READ == iRet )
					{
						CLOG_writelog_level("LPXY", CLOG_LEVEL_EVENT, "Local socket SSL_Accept, ssl continue, error=%d:%s,sockfd=%d", iRet, ERR_lib_error_string(iRet), pstLocalSockInfo->sSockfd);
						return SYS_CONTUE;
					}
					else
					{
						CLOG_writelog_level("LPXY", CLOG_LEVEL_ERROR, "Local socket SSL_Accept, ssl Error=%d:%s, sockfd=%d", iRet, ERR_lib_error_string(iRet),  pstLocalSockInfo->sSockfd);
						return SYS_ERR;
					}
				}
				else
				{
					pstLocalSockInfo->stTlsInfo.IsSslConnected = TRUE;
					CLOG_writelog_level("LPXY", CLOG_LEVEL_EVENT, "SSL_Accept successful! sockfd=%d", pstLocalSockInfo->sSockfd);
				}
			}
		}
		else
		{
			if ( pstLocalSockInfo->stTlsInfo.IsSslConnected )
			{
				/*握手成功后，进入读取的数据阶段*/
				iError = SSL_read(pstLocalSockInfo->stTlsInfo.pstSsl, acRecvBuf, MSG_RECVBUF);
				if (iError > 0)
				{
					CLOG_writelog_level("LPXY", CLOG_LEVEL_EVENT, "SSL_read successful! acRecvBuf=[%d]:%s, sockfd=%d", iError, acRecvBuf, pstLocalSockInfo->sSockfd);
				}
				else
				{
					CLOG_writelog_level("LPXY", CLOG_LEVEL_ERROR, "SSL_read error! errorCode=%d,sockfd=%d", SSL_get_error(pstLocalSockInfo->stTlsInfo.pstSsl, iError), pstLocalSockInfo->sSockfd);
					return SYS_ERR;
				}
			}
			else
			{
				iError = SSL_accept(pstLocalSockInfo->stTlsInfo.pstSsl);
				if (iError == -1)
				{
					iRet = SSL_get_error(pstLocalSockInfo->stTlsInfo.pstSsl, iError);
					//iRet = ERR_get_error();
					//ERR_print_errors_fp(stdout);
					//if (NULL == ERR_lib_error_string(iRet)
					if( SSL_ERROR_WANT_READ == iRet)
					{
						CLOG_writelog_level("LPXY", CLOG_LEVEL_EVENT, "Again SSL_Accept, ssl continue, error=%d:%s, sockfd=%d", iRet, ERR_lib_error_string(iRet), pstLocalSockInfo->sSockfd);
						return SYS_CONTUE;
					}
					else
					{
						CLOG_writelog_level("LPXY", CLOG_LEVEL_ERROR, "Again SSL_Accept, ssl Error=%d:%s, sockfd=%d", iRet, ERR_lib_error_string(iRet), pstLocalSockInfo->sSockfd);
						return SYS_ERR;
					}
				}
				else
				{
					pstLocalSockInfo->stTlsInfo.IsSslConnected = TRUE;
				}
			}
		}
	}
	/*非加密的处理流程*/
	else
	{
		iRet = recv(pstLocalSockInfo->sSockfd, acRecvBuf, MSG_RECVBUF, 0);
		if (iRet == 0 || (iRet == SOCKET_ERROR && WSAGetLastError() == WSAECONNRESET))
		{
			CLOG_writelog_level("LPXY", CLOG_LEVEL_ERROR, "WSAEnumNetworkEvents error, iRet=%d errorcode=(%d)", iRet, WSAGetLastError());
			return SYS_ERR;
		}
		else
		{
			CLOG_writelog_level("LPXY", CLOG_LEVEL_EVENT, "[LOCAL!] Read the Content=%s,\n", acRecvBuf);
		}
	}

	return SYS_OK;
}

unsigned int __stdcall OpensslProxy_NetworkEventsWorker(void *pvArgv)
{
    ULONG                    ulArrayIndex = 0;
    INT32                       iRet = 0;
    INT32                       iEvtIndex = 0;
    INT32                       iError = 0;
    SOCKET                    sSocket = INVALID_SOCKET;
    SOCK_MGR_S*         pstSockMgr = NULL;
	PPERSOCKINFO_S	pstPerSockInfo = NULL;
    CHAR                       acRecvBuf[MSG_RECVBUF] = {0};
    struct  sockaddr_in  stClientInfo = { 0 };
    WSANETWORKEVENTS NetworkEvents = { 0 };
	SSL_CTX*					pstTlsCtxClient = NULL;
	SSL_CTX*					pstTlsCtxServer = NULL;
	INT32                       iLen = sizeof(stClientInfo);

    if (NULL == pvArgv)
    {
        return -1;
    }

    pstSockMgr = (SOCK_MGR_S *)pvArgv;

	pstTlsCtxClient = SSL_CTX_new(SSLv23_client_method());
	if (NULL == pstTlsCtxClient)
	{
		CLOG_writelog_level("LPXY", CLOG_LEVEL_ERROR, "SSL Ctx client create error!");
		return -1;
	}
	pstSockMgr->pstTlsCtxClient = pstTlsCtxClient;

	pstTlsCtxServer = SSLPROXY_TLSCtxNewServer();
	if (NULL == pstTlsCtxServer)
	{
		CLOG_writelog_level("LPXY", CLOG_LEVEL_ERROR, "SSL Ctx client create error!");
		SSL_CTX_free(pstTlsCtxClient);
		pstTlsCtxClient = NULL;
		return -1;
	}
	pstSockMgr->pstTlsCtxServer = pstTlsCtxServer;

    SetEvent(pstSockMgr->hCompleteEvent);

    while (TRUE)
    {
        iRet = WSAWaitForMultipleEvents(pstSockMgr->ulSockNums, pstSockMgr->stNetEvent.arrWSAEvts, FALSE, INFINITE, FALSE);
        if ( iRet == WSA_WAIT_FAILED || iRet == WSA_WAIT_TIMEOUT)
        {
            iError = GetLastError();
            if ( iError != ERROR_INVALID_PARAMETER )
            {
                CLOG_writelog_level("LPXY", CLOG_LEVEL_ERROR, "WSAEvent continue, iRet=%d errorcode=(%d), sockNums=%d",
                    iRet, iError, pstSockMgr->ulSockNums);
            }
            continue;
        }

        iEvtIndex = iRet - WSA_WAIT_EVENT_0;
        sSocket   = pstSockMgr->stNetEvent.arrSocketEvts[iEvtIndex];

        WSAEnumNetworkEvents(sSocket, pstSockMgr->stNetEvent.arrWSAEvts[iEvtIndex], &NetworkEvents);

        if (NetworkEvents.lNetworkEvents & FD_READ)
        {
			memset(acRecvBuf, 0, MSG_RECVBUF);
            /*消息处理*/
            if (sSocket == pstSockMgr->sUdpMsgSock )
            {
                iRet = recvfrom(sSocket, acRecvBuf, MSG_RECVBUF, 0, (struct sockaddr*)&stClientInfo, &iLen);
                if (iRet > 0 )
                {
                    if (SYS_ERR == OpensslProxy_MessageCtrlMain(pstSockMgr, acRecvBuf, iRet))
                    {
                        CLOG_writelog_level("LPXY", CLOG_LEVEL_ERROR, "MessageCtrlMain handler error, iRet=%d errorcode=(%d)", iRet, GetLastError() );
                    }
                }
                else
                {
                    CLOG_writelog_level("LPXY", CLOG_LEVEL_ERROR, "MessageCtrlMain udp recvfrom error, iRet=%d errorcode=(%d)", iRet, GetLastError());
                }

				/*直接继续接收*/
				continue;
            }
            else
            {
				pstPerSockInfo = &pstSockMgr->stArrySockInfo[iEvtIndex];

				if ( TLSVERSION_INIT == pstPerSockInfo->stTlsInfo.uiTlsVersion )
				{
					pstPerSockInfo->stTlsInfo.uiTlsVersion = SSLPROXY_TLSVersionProtoCheck(pstPerSockInfo->sSockfd);
					if (TLSVERSION_NOTSSL != pstPerSockInfo->stTlsInfo.uiTlsVersion)
					{
						pstPerSockInfo->bIsTls = TRUE;
					}
				}

				switch ( pstPerSockInfo->eSockType )
				{
					case SOCKTYPE_LOCAL:
						if ( SYS_ERR== OpensslProxy_NetworkEventLocalHandler(pstSockMgr, pstPerSockInfo) )
						{
							(VOID)OpensslProxy_PerSockEventDel(pstSockMgr, iEvtIndex);
							CLOG_writelog_level("LPXY", CLOG_LEVEL_ERROR, "Network Event Local handler error");
						}
						break;
					case SOCKTYPE_PROXY:
						if (SYS_ERR == OpensslProxy_NetworkEventProxyHandler(pstSockMgr, pstPerSockInfo))
						{
							(VOID)OpensslProxy_PerSockEventDel(pstSockMgr, iEvtIndex);
							CLOG_writelog_level("LPXY", CLOG_LEVEL_ERROR, "Network Event Proxy handler error");
						}
						break;
					default:

						break;
				}
            }
        }

        if (NetworkEvents.lNetworkEvents & FD_CLOSE)
        {
			CLOG_writelog_level("LPXY", CLOG_LEVEL_ERROR, "NetworkEvents.lNetworkEvents FD_Close, iEvtIndex=%d sockfd=(%d)", iEvtIndex, pstPerSockInfo->sSockfd);
			(VOID)OpensslProxy_PerSockEventDel(pstSockMgr, iEvtIndex);
        }
    }

	CLOG_writelog_level("LPXY", CLOG_LEVEL_EVENT, "Network Event Pthread Handler End!index=%d", pstSockMgr->uiArryIndex);


    return 0;
}

VOID   OpensslProxy_NetworkEventReset(SOCK_MGR_S *pstSockMgr, UINT32 uiEvtIndex)
{
    if (uiEvtIndex >= WSAEVT_NUMS)
    {
        return;
    }
    pstSockMgr->stNetEvent.arrWSAEvts[uiEvtIndex] = NULL;
    pstSockMgr->stNetEvent.arrSocketEvts[uiEvtIndex] = INVALID_SOCKET;
}

INT32 OpensslProxy_PerSockEventCtrl(SOCK_MGR_S *pstSockMgr, UINT32 uiIndex, UINT32 uiCtrlCode)
{
    /*TODO: 这块其实应该是需要获取原先的，然后取消或者添加相关位*/
    ULONG   ulEventMask = 0;

    if ( NULL == pstSockMgr)
    {
        return SYS_ERR;
    }

    switch (uiCtrlCode)
    {
        case SOCKCTRL_SHUTDOWN:
                (VOID)OpensslProxy_PerSockEventDel(pstSockMgr, uiIndex);
            break;
        case SOCKCTRL_CLOSE_RECV:
            ulEventMask = FD_CLOSE;
            break;
        case SOCKCTRL_OPEN_RECV:
            ulEventMask = FD_READ | FD_CLOSE;
            break;
        case SOCKCTRL_CLOSE_SEND:
            ulEventMask =  FD_CLOSE;
            break;
        case SOCKCTRL_OPEN_SEND:
            ulEventMask = FD_WRITE | FD_CLOSE;
            break;
        case SOCKCTRL_UNKNOW:
        default:
            break;
    }

    WSAEventSelect(pstSockMgr->stArrySockInfo[uiIndex].sSockfd, pstSockMgr->stArrySockInfo[uiIndex].hEvtHandle, ulEventMask);

    return SYS_OK;
}

VOID   OpensslProxy_PerSockInfoReset(PERSOCKINFO_S *pstPerSockInfo)
{
    if ( NULL == pstPerSockInfo )
    {
        return;
    }

    InitializeListHead(&pstPerSockInfo->stNode);
    InitializeListHead(&pstPerSockInfo->stIoBufList);
    pstPerSockInfo->eSockType = SOCKTYPE_UNKNOW;
    pstPerSockInfo->sSockfd = INVALID_SOCKET;
    pstPerSockInfo->hEvtHandle = NULL;
    pstPerSockInfo->lEvtsIndex = -1; 
    pstPerSockInfo->lPeerEvtsIndex = -1;
    pstPerSockInfo->pfSockCtrlCb = OpensslProxy_PerSockEventCtrl;
}

/*获取对端的事件索引*/
INT32    OpensslProxy_GetPeerSockEventIndex(PERSOCKINFO_S *pstPerSockInfo)
{
    return pstPerSockInfo->lPeerEvtsIndex;
}

INT32       OpensslProxy_UpdateSockEventPeerIndex(SOCK_MGR_S *pstSockMgr, UINT32 uiEvtIndex, UINT32 uiPeerIndex)
{
	if (NULL == pstSockMgr
		|| uiEvtIndex >= WSAEVT_NUMS)
	{
		return SYS_ERR;
	}

	if (INVALID_SOCKET != pstSockMgr->stArrySockInfo[uiEvtIndex].sSockfd)
		pstSockMgr->stArrySockInfo[uiEvtIndex].lPeerEvtsIndex = uiPeerIndex;

	return SYS_OK;
}
VOID       OpensslProxy_PerSockEventClear(PERSOCKINFO_S *pstPerSockInfo)
{
    /*直接检查释放掉所有的本Socket相关IoBuf资源*/
    COMM_IOBUF_BufListRelease(&pstPerSockInfo->stIoBufList);
	if (pstPerSockInfo->bIsTls)
	{
		if ( NULL != pstPerSockInfo->stTlsInfo.pstSsl )
		{
			SSL_free(pstPerSockInfo->stTlsInfo.pstSsl);
			pstPerSockInfo->stTlsInfo.pstSsl = NULL;
		}
	}
    /*先关闭本socket的所有资源*/
    shutdown(pstPerSockInfo->sSockfd, SD_BOTH);
    closesocket(pstPerSockInfo->sSockfd);
}

INT32       OpensslProxy_PerSockEventAdd(SOCK_MGR_S *pstSockMgr,SOCKET sSocketFd, UINT32 uiSockType)
{
    if ( NULL == pstSockMgr 
        || INVALID_SOCKET == sSocketFd 
        || pstSockMgr->ulSockNums >= WSAEVT_NUMS-2 )
    {
        return SYS_ERR;
    }

    /*开始进行遍历插入动作*/
    for (int iIndex = 0; iIndex < WSAEVT_NUMS; iIndex++)
    {
        if ( INVALID_SOCKET == pstSockMgr->stArrySockInfo[iIndex].sSockfd )
        {
            /*确保没有乱入残留的数据*/
            InitializeListHead(&pstSockMgr->stArrySockInfo[iIndex].stIoBufList);
            pstSockMgr->stArrySockInfo[iIndex].eSockType = (SOCKTYPE_E)uiSockType;
            pstSockMgr->stArrySockInfo[iIndex].lEvtsIndex = iIndex;
            pstSockMgr->stArrySockInfo[iIndex].sSockfd = sSocketFd;
            pstSockMgr->stArrySockInfo[iIndex].hEvtHandle = WSACreateEvent();

			/*SSL相关*/
			pstSockMgr->stArrySockInfo[iIndex].bIsTls = FALSE;
			pstSockMgr->stArrySockInfo[iIndex].stTlsInfo.IsSslConnected = FALSE;
			pstSockMgr->stArrySockInfo[iIndex].stTlsInfo.pstSsl = NULL;
			pstSockMgr->stArrySockInfo[iIndex].stTlsInfo.uiSslType = SSLTYPE_UNKNOW;
			pstSockMgr->stArrySockInfo[iIndex].stTlsInfo.uiTlsVersion = TLSVERSION_INIT;

            /*添加到对应的网络事件*/
            pstSockMgr->stNetEvent.arrSocketEvts[iIndex] = sSocketFd;
            pstSockMgr->stNetEvent.arrWSAEvts[iIndex] = pstSockMgr->stArrySockInfo[iIndex].hEvtHandle;
            WSAEventSelect(sSocketFd, pstSockMgr->stNetEvent.arrWSAEvts[iIndex], FD_READ | FD_CLOSE);

            InterlockedIncrement(&pstSockMgr->ulSockNums);
            return SYS_OK;
        }
    }

    return SYS_ERR;
}

/*删除本网络事件，注意： 确保在删除之前，将对端也删除*/
INT32       OpensslProxy_PerSockEventDel(SOCK_MGR_S *pstSockMgr, UINT32 uiEvtIndex)
{
    PPERSOCKINFO_S pstPeerSockInfo = NULL;
    INT32  iPeerIndex = 0;

    if (NULL == pstSockMgr
        || uiEvtIndex >= WSAEVT_NUMS )
    {
        return SYS_ERR;
    }

    if ( INVALID_SOCKET != pstSockMgr->stArrySockInfo[uiEvtIndex].sSockfd )
    {
        /*保存对端的*/
        iPeerIndex = OpensslProxy_GetPeerSockEventIndex(&pstSockMgr->stArrySockInfo[uiEvtIndex]);
        
        /*1. 先处理本身的Socket*/
        OpensslProxy_PerSockEventClear(&pstSockMgr->stArrySockInfo[uiEvtIndex]);
       /*移除相关事件*/
       WSACloseEvent(pstSockMgr->stNetEvent.arrWSAEvts[uiEvtIndex]);
       /*重置一下*/
       OpensslProxy_PerSockInfoReset(&pstSockMgr->stArrySockInfo[uiEvtIndex]);
       /*清空事件*/
       OpensslProxy_NetworkEventReset(pstSockMgr, uiEvtIndex);

       InterlockedDecrement(&pstSockMgr->ulSockNums);

        /*然后通知对端的socket进行关闭处理*/
       if ( iPeerIndex > 0 
           && iPeerIndex < WSAEVT_NUMS )
       {
           pstPeerSockInfo = &pstSockMgr->stArrySockInfo[iPeerIndex];
           /*对端可能已经被释放了，需要判断有效性*/
           if ( NULL != pstPeerSockInfo 
               && pstPeerSockInfo->lEvtsIndex > 0
               && pstPeerSockInfo->sSockfd != INVALID_SOCKET )
           {
               /*通知对端也进行关闭*/
               (VOID)pstPeerSockInfo->pfSockCtrlCb(pstSockMgr, iPeerIndex, SOCKCTRL_SHUTDOWN);
           }
       }
    }
    
    return SYS_OK;
}

/*获取通信端口*/
USHORT  OpensslProxy_GetMsgSocketPortByIndex(UINT32   uiArryIndex)
{
    USHORT  usPort = 0;

    if (NULL == g_pstWorker
        || uiArryIndex >= MGR_ARRYNUMS )
    {
        return 0;
    }

    /*多线程锁*/
    EnterCriticalSection(&g_pstWorker->stWorkerLock);
    if (NULL != g_pstWorker->pstArryWorker[uiArryIndex] )
    {
        usPort = g_pstWorker->pstArryWorker[uiArryIndex]->usUdpMsgPort;
    }
    LeaveCriticalSection(&g_pstWorker->stWorkerLock);
    
    return usPort;
}


/*获取通信端口*/
USHORT  OpensslProxy_GenMsgSocketPort()
{
    USHORT  usPort = 0;

    if (NULL == g_pstWorker )
    {
        return 0;
    }

    /*直接还是线程锁吧*/
    EnterCriticalSection(&g_pstWorker->stWorkerLock);
    InterlockedIncrement(&g_pstWorker->usMsgPortNum);
    usPort = g_pstWorker->usMsgPortNum;
    LeaveCriticalSection(&g_pstWorker->stWorkerLock);

    return usPort;
}

/*直接往相关的索引线程发送消息*/
INT32 OpensslProxy_SockMgr_MainWorkerSendto(CHAR *pcSndBuf, UINT32 uiSendLen,  UINT32 uiArryIndex)
{
    USHORT          usPort = 0;
    INT32              iRet = 0;
    SOCKET          sSocket = INVALID_SOCKET;
    sockaddr_in    remoteAddr = {0};
    int                   nAddrLen = sizeof(remoteAddr);

    usPort = OpensslProxy_GetMsgSocketPortByIndex(uiArryIndex);
    if (0 == usPort)
    {
        return SYS_ERR;
    }

    /*创建UDP通信端口*/
    sSocket = socket(AF_INET, SOCK_DGRAM, 0);
    if (INVALID_SOCKET == sSocket)
    {
        CLOG_writelog_level("LPXY", CLOG_LEVEL_ERROR, "Director message udp socket error=%d", GetLastError());
        return SYS_ERR;
    }

    remoteAddr.sin_family = AF_INET;
    remoteAddr.sin_port = htons(usPort);
    inet_pton(AF_INET, MGR_LOCALADDRA, &remoteAddr.sin_addr);

    iRet = sendto(sSocket, pcSndBuf, uiSendLen,0, (sockaddr *)&remoteAddr, nAddrLen);
    if (iRet <= 0 )
    {
        CLOG_writelog_level("LPXY", CLOG_LEVEL_ERROR, "Director message udp socket sendto error=%d", GetLastError());
        return SYS_ERR;
    }

    return SYS_OK;
}

SOCK_MGR_S *OpensslProxy_SockMgrCreate(WORKER_CTX_S *pstWorker, UINT32   uiArryIndex)
{
    SOCK_MGR_S*          pstSockMgr  = NULL;
    SOCKADDR_IN         stSerAddr = { 0 };
    DWORD				     dwStatckSize = MGR_STACKSIZE;
    ULONG                    ulBlock = 1;
    INT32                       iRet = 0;

    pstSockMgr = (SOCK_MGR_S *)malloc(sizeof(SOCK_MGR_S));
    if (NULL == pstSockMgr)
    {
        CLOG_writelog_level("LPXY", CLOG_LEVEL_ERROR, "malloc socket manager context error!");
        return NULL;
    }

    RtlZeroMemory(pstSockMgr, sizeof(SOCK_MGR_S));
    pstSockMgr->iErrorCode = MGR_ERRCODE_SUCCESS;
    pstSockMgr->uiArryIndex = uiArryIndex;

    for ( int index =0; index< WSAEVT_NUMS; index++ )
    {
        OpensslProxy_PerSockInfoReset(&pstSockMgr->stArrySockInfo[index]);
        OpensslProxy_NetworkEventReset(pstSockMgr, index);
    }

    pstSockMgr->hCompleteEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (NULL == pstSockMgr->hCompleteEvent)
    {
        CLOG_writelog_level("LPXY", CLOG_LEVEL_ERROR, "Dispatch ctx create complete event error!");
        free(pstSockMgr);
        pstSockMgr = NULL;
        return NULL;
    }

    /*创建UDP通信端口*/
    pstSockMgr->sUdpMsgSock = socket(AF_INET, SOCK_DGRAM, 0);
    if (INVALID_SOCKET == pstSockMgr->sUdpMsgSock)
    {
        CLOG_writelog_level("LPXY", CLOG_LEVEL_ERROR, "msg udp socket create error=%d", GetLastError());
        CloseHandle(pstSockMgr->hCompleteEvent);
        free(pstSockMgr);
        pstSockMgr = NULL;
        return NULL;
    }

    pstSockMgr->usUdpMsgPort = OpensslProxy_GenMsgSocketPort();

    stSerAddr.sin_family = AF_INET;
    stSerAddr.sin_port = htons(pstSockMgr->usUdpMsgPort);
    inet_pton(AF_INET, MGR_LOCALADDRA, &stSerAddr.sin_addr);

    //stSerAddr.sin_addr.S_un.S_addr = htonl(INADDR_ANY);
    if (bind(pstSockMgr->sUdpMsgSock, (sockaddr *)&stSerAddr, sizeof(stSerAddr)) == SOCKET_ERROR)
    {
        CLOG_writelog_level("LPXY", CLOG_LEVEL_ERROR, "msg udp socket bind error=%d", GetLastError());
        closesocket(pstSockMgr->sUdpMsgSock); 
        CloseHandle(pstSockMgr->hCompleteEvent);
        free(pstSockMgr);
        pstSockMgr = NULL;
        return NULL;
    }

    iRet = ioctlsocket(pstSockMgr->sUdpMsgSock, FIONBIO, (unsigned long *)&ulBlock);//设置成非阻塞模式??  
    if (iRet == SOCKET_ERROR)//设置失败。
    {
        CLOG_writelog_level("LPXY", CLOG_LEVEL_ERROR, "Set ioctrl fionbio error=%d\n", WSAGetLastError());
        closesocket(pstSockMgr->sUdpMsgSock);
        CloseHandle(pstSockMgr->hCompleteEvent);
        free(pstSockMgr);
        pstSockMgr = NULL;
        return NULL;
    }

    /*添加消息socket到本网络事件中进行处理*/
    if ( SYS_ERR == OpensslProxy_PerSockEventAdd(pstSockMgr,  pstSockMgr->sUdpMsgSock, SOCKTYPE_MSG) )
    {
        CLOG_writelog_level("LPXY", CLOG_LEVEL_ERROR, "Add socket network event error=%d\n", WSAGetLastError());
        closesocket(pstSockMgr->sUdpMsgSock);
        CloseHandle(pstSockMgr->hCompleteEvent);
        free(pstSockMgr);
        pstSockMgr = NULL;
        return NULL;
    }

    /*直接在当前线程先创建Accept的本地服务端*/
    pstSockMgr->hThreadHandle = _beginthreadex(NULL, dwStatckSize, OpensslProxy_NetworkEventsWorker, pstSockMgr, 0, NULL);

    WaitForSingleObject(pstSockMgr->hCompleteEvent, INFINITE);

    /*表示线程初始化过程中存在问题，直接退出*/
    if (pstSockMgr->iErrorCode != MGR_ERRCODE_SUCCESS)
    {
        CLOG_writelog_level("LPXY", CLOG_LEVEL_ERROR, "Socker Network Event pthread create error,  errorCode=%d", pstSockMgr->iErrorCode);
        closesocket(pstSockMgr->sUdpMsgSock);
        CloseHandle(pstSockMgr->hCompleteEvent);
        free(pstSockMgr);
        pstSockMgr = NULL;
        return NULL;
    }

    /*整个线程也需要计数*/
    InterlockedIncrement(&pstWorker->uiWorkerNums);

    return pstSockMgr;
}

VOID OpensslProxy_SockMgrRelease(SOCK_MGR_S *pstSockMgr)
{
	PERSOCKINFO_S *pstPerSockInfo = NULL;

    if (NULL != pstSockMgr)
    {
		for (int index = 0; index < WSAEVT_NUMS; index++)
		{
			pstPerSockInfo = &pstSockMgr->stArrySockInfo[index];
			if (INVALID_SOCKET != pstPerSockInfo->sSockfd )
			{
				OpensslProxy_PerSockEventDel(pstSockMgr, pstPerSockInfo->lEvtsIndex);
			}
		}
        free(pstSockMgr);
    }
}

/**********************************************Worker总的上下文的管理器************************************************************/
/*全局访问，可以直接访问到通信socket*/
/*根据算法派发相关的ClientSocket*/
INT32 OpensslProxy_DispatchNetworkByBlanceAlgm(SOCKET sNewClientFd, UINT32 uiBlanceAlgm)
{
    UINT32                              uiArryIndex = 0;
    INT32                                 iRet = 0;
    MCTRL_CLIENTINFO_S      stClientCtrlInfo = {0};
    CHAR                                 acMessageBuf[MCTL_BUFSIZE] = {0};

    /*TODO: 根据算法，获取具体要分发的Worker线程索引*/
    switch (uiBlanceAlgm)
    {
        case 0:
            /*默认就是第0个*/
            uiArryIndex = 0;
            break;
        default:
            break;
    }

    stClientCtrlInfo.uiCtrlCode = CLNTNFO_CTRLCODE_SOCKADD;
    stClientCtrlInfo.sClientSockfd = sNewClientFd;

    /*获取到具体的索引，然后开始客户端信息分发*/
    iRet = OpensslProxy_MessageCtrl_ClientInfo(acMessageBuf, &stClientCtrlInfo);
    if ( SYS_ERR == iRet )
    {
        CLOG_writelog_level("LPXY", CLOG_LEVEL_ERROR, "msg ctrl make pack Info:  [ client info ] error!");
        return SYS_ERR;
    }
    else
    {
        /*发送给对方*/
        if (SYS_ERR == OpensslProxy_SockMgr_MainWorkerSendto(acMessageBuf, MCTL_BUFSIZE, uiArryIndex))
        {
            CLOG_writelog_level("LPXY", CLOG_LEVEL_ERROR, "msg ctrl send pack error!");
            return SYS_ERR;
        }
    }

    return SYS_OK;
}


unsigned int __stdcall OpensslProxy_WorkerMsgCtrl(PVOID pvArg)
{
    WORKER_CTX_S *pstWorker = NULL;
    CHAR                  acMsg[MCTL_BUFSIZE] = {0};
    struct  sockaddr_in stClientInfo = { 0 };
    INT32                   iLen = sizeof(stClientInfo);
    INT32                   iRet = 0;

    if (NULL == pvArg)
    {
        return -1;
    }

    pstWorker = (WORKER_CTX_S *)pvArg;

    while (1)
    {
        RtlZeroMemory(acMsg, MCTL_BUFSIZE);
        iRet = recvfrom(pstWorker->sMsgCtrlSockFd, acMsg, MCTL_BUFSIZE, 0, (struct sockaddr*)&stClientInfo, &iLen);
        if (iRet < 0)
        {
            CLOG_writelog_level("LPXY", CLOG_LEVEL_ERROR, "worker msg ctrl udps recvfrom message error=%08x!", GetLastError());
            break;
        }
        else
        {
            if ( SYS_ERR == OpensslProxy_MessageCtrlMain(pstWorker, acMsg,  iRet) )
            {
                CLOG_writelog_level("LPXY", CLOG_LEVEL_ERROR, "worker msg ctrl udps message handler error!");
            }
        }
    }

    return 0;
}

WORKER_CTX_S *OpensslProxy_NetworkEventWorkerCreate()
{
    UINT32                  uiIndex       = 0;
    USHORT                usPort         = 0;
    SOCKADDR_IN      stSerAddr   = {0};

    g_pstWorker = (WORKER_CTX_S *)malloc(sizeof(WORKER_CTX_S));
	if (NULL == g_pstWorker)
	{
		CLOG_writelog_level("LPXY", CLOG_LEVEL_ERROR, "malloc worker context error!");
		return NULL;
	}

	RtlZeroMemory(g_pstWorker, sizeof(WORKER_CTX_S));
    InitializeCriticalSection(&g_pstWorker->stWorkerLock);

    for (int i = 0; i < MGR_ARRYNUMS; i++)
    {
        g_pstWorker->pstArryWorker[i] = NULL;
    }

    g_pstWorker->uiWorkerNums = MSG_UDPPORT_START;

	SSLPROXY_TlsHandler_EnvInit();

    /*创建UDP通信端口*/
    g_pstWorker->sMsgCtrlSockFd = socket(AF_INET, SOCK_DGRAM, 0);
    if (INVALID_SOCKET == g_pstWorker->sMsgCtrlSockFd)
    {
        CLOG_writelog_level("LPXY", CLOG_LEVEL_ERROR, "msg udp socket create error=%d", GetLastError());
        DeleteCriticalSection(&g_pstWorker->stWorkerLock);
        free(g_pstWorker);
        return NULL;
    }

    usPort = OpensslProxy_GenMsgSocketPort();
    stSerAddr.sin_family = AF_INET;
    stSerAddr.sin_port = htons(usPort);
    inet_pton(AF_INET, MGR_LOCALADDRA, &stSerAddr.sin_addr);
    //stSerAddr.sin_addr.S_un.S_addr = htonl(INADDR_ANY);
    if ( bind(g_pstWorker->sMsgCtrlSockFd, (sockaddr *)&stSerAddr, sizeof(stSerAddr)) == SOCKET_ERROR )
    {
        CLOG_writelog_level("LPXY", CLOG_LEVEL_ERROR, "msg udp socket bind error=%d", GetLastError());
        closesocket(g_pstWorker->sMsgCtrlSockFd);
        DeleteCriticalSection(&g_pstWorker->stWorkerLock);
        free(g_pstWorker);
        return NULL;
    }

    /*直接创建消息线程, 简单的消息控制，不可靠，需要添加可靠队列*/
    _beginthreadex(NULL, 0, OpensslProxy_WorkerMsgCtrl, g_pstWorker, 0, NULL);

    /*默认先创建一个*/
    uiIndex = 0;
    g_pstWorker->pstArryWorker[uiIndex] = OpensslProxy_SockMgrCreate(g_pstWorker, uiIndex);
    if (NULL == g_pstWorker->pstArryWorker[uiIndex] )
    {
        CLOG_writelog_level("LPXY", CLOG_LEVEL_ERROR, "create socket manager error!");
        closesocket(g_pstWorker->sMsgCtrlSockFd);
        DeleteCriticalSection(&g_pstWorker->stWorkerLock);
        free(g_pstWorker);
        return NULL;
    }

	return g_pstWorker;
}


VOID OpensslProxy_NetworkEventWorkerRelease()
{
	if (NULL != g_pstWorker)
	{
        for (int i = 0; i < MGR_ARRYNUMS; i++)
        {
            OpensslProxy_SockMgrRelease(g_pstWorker->pstArryWorker[i]);
        }

        DeleteCriticalSection(&g_pstWorker->stWorkerLock);
		free(g_pstWorker);
        g_pstWorker = NULL;
	}
}








