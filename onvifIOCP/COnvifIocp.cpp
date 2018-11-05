#include "COnvifIocp.h"

int COnvifIocp::ManualCloseSocket(SOCKET sock, int iShutdownFlag, BOOL bGraceful, BOOL bReuseAddress)
{
	if(!bGraceful)
	{
		linger ln = {1,0};
		setsockopt(sock, SOL_SOCKET, SO_LINGER, (CHAR*)&ln, sizeof(linger));
	}
	
	if(bReuseAddress)
	{
		setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (CHAR*)&bReuseAddress, sizeof(BOOL));
	}

	if(iShutdownFlag != 0xFF)
		shutdown(sock, iShutdownFlag);

	return closesocket(sock);
}

EOnvifIocpError COnvifIocp::Start()
{
    if(m_eState == EONVIFIOCPSTATE_BIRTH)
	{
	    InitCpmpletePort();
		m_pfnAcceptEx = nullptr;

		//准备buffer,使用默认的尺寸
		m_BufferObjPool.Prepare();
        m_eState = EONVIFIOCPSTATE_PREPARED;

	}
	else
	{
	    return EONVIFIOCP_HASSTARTED;
	}
    return EONVIFIOCP_SUCCESS;
}

TSocketObj* COnvifIocp::CreateSocketObj()
{
	TSocketObj* pSocketObj = (TSocketObj*)m_PrivHeap.Alloc(sizeof(TSocketObj));
	
	if(pSocketObj == NULL)
	{
		LOG_ERROR << "CreateSocketObj no memory!!";
		return NULL;
	}

	pSocketObj->TSocketObj::TSocketObj(m_BufferObjPool);

	return pSocketObj;
}

//这个接口用于后期优化，不是多余的。
TSocketObj*	COnvifIocp::GetNewSocketObj(CONNID dwConnID, SOCKET soClient)
{
	//看看回收区有没有
	DWORD dwIndex;
	TSocketObj* pSocketObj = nullptr;

    //优化一个缓存使用sockObj，这里直接从新开辟一个。
	pSocketObj = CreateSocketObj();
	if(pSocketObj == NULL){return NULL;}
	pSocketObj->Reset(dwConnID, soClient);

	return pSocketObj;
}

void COnvifIocp::DoRecvState(TBufferObj *pBufferObj,TSocketObj *pSocketObj)
{
	//为了保证上层接收到数据一致性，必须加锁,这个不是多余的操作，切记！
	CLocalLock<CReentrantSpinGuard> localLockHelper(pSocketObj->csRecv);
	pNotify->Publisher(CNotifyManager::ENOTIFY_PREPARE_RECVED,"已经接收到网络数据");
	//直接推给上层? 先不做。
	return;
}

void COnvifIocp::DoAcceptState(TBufferObj *pBufferObj,TSocketObj *pSocketObj)
{
	//首先通知上层，准备接收新连接，由上层决定是否继续向完成端口提交accept
	pNotify->Publisher(CNotifyManager::ENOTIFY_PREPARE_ACCEPT_NEW_CONNECT,"Accept a new connect");
	if(m_pfnGetAcceptExSockaddrs == nullptr)
	{
	    if(pBufferObj->hlistenFd == INVALID_SOCKET)
		{
			LOG_ERROR << "DoAcceptState Listenfd INVALID";
			return;
		}

		GUID guid = WSAID_GETACCEPTEXSOCKADDRS;
		DWORD dwBytes;
		PVOID pfn = nullptr;

		::WSAIoctl	(
			pBufferObj->hlistenFd,
			SIO_GET_EXTENSION_FUNCTION_POINTER,
			&guid,
			sizeof(guid),
			&m_pfnGetAcceptExSockaddrs,
			sizeof(m_pfnGetAcceptExSockaddrs),
			&dwBytes,
			nullptr,
			nullptr
			);
	}

	//得到连接参数
	Onvifiocp_Sockaddr pLocalAddr, pRemoteAddr;
	int nLocalLen, nRmoteLen;
	do{
		int iLen = sizeof(SOCKADDR_IN) + 16;

		m_pfnGetAcceptExSockaddrs
			(
				pBufferObj->buff.buf,
				0,
				iLen,
				iLen,
				(SOCKADDR**)&pLocalAddr,
				&nLocalLen,
				(SOCKADDR**)&pRemoteAddr,
				&nRmoteLen
			);
	}while(0);
	
	//准备创建新连接
	CONNID dwConnID = 0;
	SOCKET hSocket	= pBufferObj->hClientFd;
	BOOL isOK = TRUE;
	do{
		//首先锁定一个连接id，这里的处理需要小心谨慎 ----后期需要优化的点
		if(m_eState == EONVIFIOCPSTATE_BIRTH && !m_bfActiveSocktObj.AcquireLock(dwConnID))
		{
			isOK = FALSE;
			break;
		}

		TSocketObj* pSocketObj = GetNewSocketObj(dwConnID, hSocket);
		if(pSocketObj == NULL)
		{
			pNotify->Publisher(CNotifyManager::ENOTIFY_ACCEPT_FAIL,"a new connect recv buf no socketObj");
			isOK = FALSE;
		}
	}
   while(0);

   if(!isOK)
   {
	   //果断关闭
	   ManualCloseSocket(hSocket,SD_BOTH);

	   //放入回收区 注意这样这个accept就没有了.
	   m_BufferObjPool.PutFreeItem(pBufferObj);
	   LOG_ERROR << "DoAcceptState error no Active Sockets !!";
	   return;

   }

   //不会失败~~
   ::setsockopt(hSocket, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (CHAR*)&pBufferObj->hlistenFd, sizeof(SOCKET));


   //初始化socketobj
   pSocketObj->remoteAddr = pRemoteAddr;
   pSocketObj->connTime	  = ::timeGetTime();
   pSocketObj->activeTime = pSocketObj->connTime;

   //加入无锁hash，正式接受连接。
   m_bfActiveSocktObj.ReleaseLock(dwConnID, pSocketObj);
      
   //准备向完成端口投递接收数据请求
   ::CreateIoCompletionPort((HANDLE)hSocket, m_hCompPort, (ULONG_PTR)pSocketObj, 0);

   //投递前通知应用层
   pNotify->Publisher(CNotifyManager::ENOTIFY_PREPARE_RECV,"prepare recv");

   //投递
   ActionPostRecv(pBufferObj,pSocketObj);
 
   return;
}

//server端这个方法不能暴露给上层来直接调用
BOOL COnvifIocp::ActionPostRecv(TBufferObj *pBufferObj,TSocketObj *pSocketObj)
{
	
	int result				= NO_ERROR;
	DWORD dwFlag			= 0;
	DWORD dwBytes			= 0;
	pBufferObj->hClientFd	= pSocketObj->socket;
	pBufferObj->eState  	= EONVIFIOCP_STATE_RECEIVE;
	pBufferObj->buff.len    = pBufferObj->capacity;

	if(::WSARecv(
		pBufferObj->hClientFd,
		&pBufferObj->buff,
		1,
		&dwBytes,
		&dwFlag,
		&pBufferObj->ov,
		nullptr
		) == SOCKET_ERROR)
	{
		result = ::WSAGetLastError();
		LOG_ERROR << "WSARecv error: " << result;
	}

	return result;
}

BOOL COnvifIocp::ActionPostAccept(SOCKET hListenFd)
{
	//准备一个连接socket 不考虑ipv6
	SOCKET	hClientFd = socket(AF_INET,SOCK_STREAM, IPPROTO_TCP);
	if(hClientFd == INVALID_SOCKET)
	{
		LOG_ERROR << "socket error:" << ::WSAGetLastError();
		return FALSE;
	}

	//创建了一个buffer实体，可以认为是框架的控制块，掌握数据。
	TBufferObj *pBufferObj = m_BufferObjPool.PickFreeItem();
	if(pBufferObj == NULL)
	{
		LOG_ERROR << "socket error:" << ::WSAGetLastError();
		ManualCloseSocket(hClientFd);
		return FALSE;
	}

	if(! pBufferObj->ListenFdIsSet())
	{
		pBufferObj->SetListenFd(hListenFd);
	}
	pBufferObj->eState = EONVIFIOCP_STATE_ACCEPT;

	//准备调用系统函数
	if(m_pfnAcceptEx == nullptr)
	{
		DWORD dwBytes;
		GUID guid = WSAID_ACCEPTEX;
		::WSAIoctl	(
			hListenFd,
			SIO_GET_EXTENSION_FUNCTION_POINTER,
			&guid,
			sizeof(guid),
			&m_pfnAcceptEx,
			sizeof(m_pfnAcceptEx),
			&dwBytes,
			nullptr,
			nullptr);
	}

	do{
		int iLen = sizeof(SOCKADDR_IN) + 16;
		pBufferObj->hClientFd = hClientFd;
		if(!m_pfnAcceptEx(
			hListenFd,
			pBufferObj->hClientFd,
			pBufferObj->buff.buf,
			0,
			iLen,
			iLen,
			nullptr,
			&pBufferObj->ov
			)
			)
		{
			//应用层需要释放SOcket
			LOG_ERROR << "AcceptEx error:" << ::WSAGetLastError();
			return FALSE;
		}
	}while(0);
	return TRUE;
}

//强破应用层处理框架的各种状况
void COnvifIocp::EventSwicth(OVERLAPPED* pOverlapped,DWORD dwBytes, ULONG_PTR pBackObj)
{
    if(pOverlapped != NULL)
	{
		LOG_ERROR<<"EventSwicth error:not a onvif iocp Event!!";
		return;
	}


	switch(dwBytes)
	{
		case ONVIFIOCP_EV_ACCEPT:
			{
				if(ActionPostAccept(pBackObj))
				{
					pNotify->Publisher(CNotifyManager::ENOTIFY_ACCEPT_OK,NULL);
				}
				else
				{
					pNotify->Publisher(CNotifyManager::ENOTIFY_ACCEPT_FAIL,"See log");
				}
			};break;
			

	}
}

//网络事件交换机
void COnvifIocp::EventSwicth(CONNID ptrConnid,TSocketObj *pSocketObj,TBufferObj *pBufferObj,DWORD dwBytes,DWORD dwErrorCode)
{
	if(dwBytes == 0 && pBufferObj->eState != EONVIFIOCP_BUFFER_STATE_ACCEPT)
	{
		//对buffer回收 因为涉及关闭客户端fd所以等后续处理
		//AddFreeSocketObj(pSocketObj, SCF_CLOSE);
		//AddFreeBufferObj(pBufferObj);
		return;
	}

	switch(pBufferObj->eState)
	{
		case EONVIFIOCP_BUFFER_STATE_ACCEPT:
			DoAcceptState(pBufferObj,pSocketObj);
			break;

		case EONVIFIOCP_BUFFER_STATE_RECEIVE:
			DoRecvState(pBufferObj,pSocketObj);
			break;
	}
	return;
}


UINT __stdcall COnvifIocp::TaskProc(LPVOID pv)
{
	DWORD dwErrorCode = NO_ERROR;
    DWORD dwBytes;
	TBufferObj* pBufferObj = nullptr;
    TSocketObj* pSocketObj = nullptr;
    OVERLAPPED* pOverlapped;
    while(TRUE)
    {
		pBufferObj = nullptr;
		pSocketObj = nullptr;
		CONNID dwConnID = NULL;

		BOOL bRet = ::GetQueuedCompletionStatus
			(
			m_hCompPort,
			&dwBytes,
			(PULONG_PTR)pSocketObj,
			&pOverlapped,
			INFINITE
			);

		//Onvif iocp事件处理
		if(pOverlapped == NULL)
		{
			EventSwicth(pOverlapped, dwBytes, (ULONG_PTR)pSocketObj);
			continue;
		}
	
		//得到控制块
		pBufferObj = CONTAINING_RECORD(pOverlapped, TBufferObj, ov);
		dwConnID = pBufferObj->eState !=  EONVIFIOCP_BUFFER_STATE_ACCEPT? pSocketObj->connID : 0;

		//处理底层事件
		EventSwicth(dwConnID,pSocketObj,pBufferObj,dwBytes,dwErrorCode);
	}
	//得到了pobj然后就依据obj的状态来出来处理底层事件---
    return  0;  
}

//默认预投递accept为50个，
BOOL COnvifIocp::EventPostAccept(SOCKET hListenFd,ULONG_PTR pBackObj,DWORD dwAcceptNum)
{
	//这里需要创建预accept来提高性能。
	if(m_bIsServer && m_eState != EONVIFIOCPSTATE_ACCEPT_PREPARED &&
	 ::CreateIoCompletionPort((HANDLE)hListenFd, m_hCompPort, (ULONG_PTR)hListenFd, 0))
	{
        if(dwAcceptNum == 0 || dwAcceptNum > ONVIFIOCP_DEFAULT_ACCEPT_NUM_MAX)
        {
            LOG_ERROR << "Prepare accept number is bad number:" << dwAcceptNum;
            return FALSE;
        }

		//预置的accept数量由上层提供
		for(DWORD i = 0; i < dwAcceptNum; i++)
		{
			if(::PostQueuedCompletionStatus(m_hCompPort, ONVIFIOCP_EV_ACCEPT, (ULONG_PTR)hListenFd, nullptr))
			{
                LOG_ERROR << "PostQueuedCompletionStatus is bad return num:" << dwAcceptNum;
                return FALSE;
			}  
		}
		m_eState = EONVIFIOCPSTATE_ACCEPT_PREPARED;
		return TRUE;
	}

	//这里需要上层去触发继续欧accept和增加预accept
	if(m_bIsServer && m_eState == EONVIFIOCPSTATE_ACCEPT_PREPARED)
	{
		if(::PostQueuedCompletionStatus(m_hCompPort, ONVIFIOCP_EV_ACCEPT, (ULONG_PTR)hListenFd, nullptr))
		{
			LOG_ERROR << "PostQueuedCompletionStatus is bad return num:" << dwAcceptNum;
			return FALSE;
		}  
	}
    return TRUE;
}


BOOL COnvifIocp::CreateTaskThreads()
{
	BOOL isOk = TRUE;

	for(DWORD i = 0; i < m_dwTaskNum; i++)
	{
		HANDLE hThread = (HANDLE)_beginthreadex(nullptr, 0, TaskProc, (LPVOID)this, 0, nullptr);
		if(hThread)
			m_vtTaskThreads.push_back(hThread);
		else
		{
			LOG_DEBUG << "_beginthreadex error" << " " << ::GetLastError();  
			isOk = FALSE;
			break;
		}
	}

	return isOk;
}

int main(int argc,char**argv)
{
    google::InitGoogleLogging(argv[0]);
    google::LogToStderr();
    LOG_TRACE << "test!!!";
	COnvifIocp testObj;
	testObj.InitCpmpletePort();
    getchar();
	return 0;
}
