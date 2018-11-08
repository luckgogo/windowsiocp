#include "COnvifIocp.h"
#include <MSTcpIP.h >
#pragma comment(lib, "ws2_32")


//创建socket
SOCKET  COnvifIocp::CreateSocket(TParCreateSocket& ScoketPar)
{
	SOCKET retSocket = socket(AF_INET,SOCK_STREAM, IPPROTO_TCP);
	if(retSocket == INVALID_SOCKET)
	{
		LOG_ERROR << "CreateSocket"<< WSAGetLastError(); 
		return INVALID_SOCKET;
	}

	//设置非阻塞
	BOOL bNoBlock = TRUE;
	::ioctlsocket(retSocket, FIONBIO, (ULONG*)&bNoBlock);

	//设置保活---更多socket的设置最好在iocp模块支持，目前允许上层自己设置。
	BOOL bKeepAlivOnOff	= (ScoketPar.dwKeepAliveTime > 0 && ScoketPar.dwKeepAliveInterval > 0);

	::tcp_keepalive in = {bKeepAlivOnOff, ScoketPar.dwKeepAliveTime, ScoketPar.dwKeepAliveInterval};
	DWORD dwBytes;
	if(::WSAIoctl	(
		retSocket, 
		SIO_KEEPALIVE_VALS, 
		(LPVOID)&in, 
		sizeof(in), 
		nullptr, 
		0, 
		&dwBytes, 
		nullptr, 
		nullptr
		) == SOCKET_ERROR)
	{
		int nRet = ::WSAGetLastError();
		if(nRet == WSAEWOULDBLOCK)
		{
			nRet = NO_ERROR;
		}
		else
		{
			LOG_ERROR << "WSAIoctl SIO_KEEPALIVE_VALS error" << ::WSAGetLastError();
			ManualCloseSocket(retSocket);
			return INVALID_SOCKET;
		}
	}

	//服务端设置
	if(ScoketPar.isServer)
	{
		BOOL isOK = TRUE;
		do{
			//绑定本地地址
			SOCKADDR	addr4;
			int nSize = sizeof(SOCKADDR_IN);
			SOCKADDR_IN *p=(SOCKADDR_IN *) &addr4;
			if(::WSAStringToAddress(TEXT("0.0.0.0"), AF_INET, nullptr,&addr4, &nSize) != NO_ERROR)
			{
				LOG_ERROR<< "WSAStringToAddress "<< ScoketPar.lpszBindAddress << " 不成功" << WSAGetLastError();
				isOK = FALSE;
				break;
			}
			p->sin_port = htons(ScoketPar.usPort);

			if(::bind(retSocket, &addr4, nSize) != SOCKET_ERROR)
			{
				//绑定成功就监听
				if(::listen(retSocket, ScoketPar.dwSocketListenQueue) == SOCKET_ERROR)
				{
					LOG_ERROR << "Listen Socket listen error:" << WSAGetLastError();
					isOK=FALSE;
					break;
				}
			
			}
			else
			{
				LOG_ERROR<<"bind error"<< WSAGetLastError();
				isOK = FALSE;
				break;
			}

		}while(0);

		if(!isOK)
		{
			ManualCloseSocket(retSocket);
			return INVALID_SOCKET;
		}
	}
	return retSocket;
}


//关闭socket的封装
int COnvifIocp::ManualCloseSocket(SOCKET sock, int iShutdownFlag, BOOL bGraceful, BOOL bReuseAddress)
{
	if(!bGraceful)
	{
		linger ln = {1,0};
		::setsockopt(sock, SOL_SOCKET, SO_LINGER, (CHAR*)&ln, sizeof(linger));
	}
	
	if(bReuseAddress)
	{
		::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (CHAR*)&bReuseAddress, sizeof(BOOL));
	}

	if(iShutdownFlag != 0xFF)
		::shutdown(sock, iShutdownFlag);

	return ::closesocket(sock);
}

//准备iocp的资源，私有堆和buffer缓冲区
EOnvifIocpError COnvifIocp::Start()
{
    if(m_eState == EONVIFIOCPSTATE_BIRTH)
	{
		WORD sockVersion = MAKEWORD(2,2);
		WSADATA wsaData;
		WSAStartup(sockVersion, &wsaData);

	    InitCpmpletePort();
		m_pfnAcceptEx = nullptr;
		m_pfnGetAcceptExSockaddrs = nullptr;

		//准备buffer,使用默认的尺寸
		m_BufferObjPool.Prepare();
        m_eState = EONVIFIOCPSTATE_PREPARED;

		//准备socket生命周期容器
		m_bfActiveSocktObj.Reset(300);
		m_bfInactiveSocktObj.Reset(300);

		CreateTaskThreads();
	}
	else
	{
	    return EONVIFIOCP_HASSTARTED;
	}
    return EONVIFIOCP_SUCCESS;
}

void COnvifIocp::CloseConnectSocket(TSocketObj *pSocktObj)
{
	ASSERT(TSocketObj::IsExist(pSocktObj));

	//close之前通知上层
	m_pNotify->ActionSwitch(CNotifyManager::ENOTIFY_ACTION_CLOSE,this,(BYTE*)"before close",(int)12);

	//置位不可用
	SOCKET hSocket = pSocktObj->socket;
	pSocktObj->socket = INVALID_SOCKET;

	ManualCloseSocket(hSocket);
	return;

}


void COnvifIocp::AddToInactiveSocketObj(TSocketObj *pSocktObj)
{
	//make TSocketObj Invalid
	if(TSocketObj::IsValid(pSocktObj))
	{
		//等待收发，其实已经不能正常保证业务的进行,只是力求优雅
		CLocalLock<CReentrantSpinGuard>	locallock(pSocktObj->csRecv);
		CLocalLock<CInterCriSec>	locallock2(pSocktObj->csSend);

		if(TSocketObj::IsValid(pSocktObj))
		{
			TSocketObj::Invalid(pSocktObj);
		}
	}

	//关闭连接socket。
	CloseConnectSocket(pSocktObj);
	
	m_bfActiveSocktObj.Remove(pSocktObj->connID);
	TSocketObj::Release(pSocktObj);

	//加入到缓存区，等一会儿再使用
	BOOL isOk = m_bfInactiveSocktObj.TryPut(pSocktObj);

	//处理缓存区满的情况
	if(!isOk)
	{
		//直接del
		ASSERT(pSocktObj);
		pSocktObj->TSocketObj::~TSocketObj();
		m_PrivHeap.Free(pSocktObj);
	}
	return;
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
	m_pNotify->Publisher(CNotifyManager::ENOTIFY_RECVED,"已经接收到网络数据");
	
	//默认为推送形式，后期需要加入pull模式
	int nRet =  m_pNotify->ActionSwitch(CNotifyManager::ENOTIFY_ACTIOON_RECV,
		this,(BYTE*)(pBufferObj->buff.buf),pBufferObj->dwRecvLen);
	
	//一直读吧
	if(nRet == Action_Continue)
	{
		do{
			nRet = ActionPostRecv(pBufferObj,pSocketObj,FALSE);
			if(nRet != NO_ERROR)
			{
				//处理对端关闭
				//if(nRet == WSAEDISCON) 以后完成
				if(nRet == WSAEWOULDBLOCK)
				{
					break;
				}
				AddToInactiveSocketObj(pSocketObj);
				break;
			}
			nRet =  m_pNotify->ActionSwitch(CNotifyManager::ENOTIFY_ACTIOON_RECV,
				this,(BYTE*)(pBufferObj->buff.buf),pBufferObj->buff.len);
		}while(nRet == Action_Continue);
	}

	return;
}

void COnvifIocp::DoAcceptState(TBufferObj *pBufferObj,TSocketObj *pSocketObj)
{
	//首先通知上层，准备接收新连接，由上层决定是否继续向完成端口提交accept
	m_pNotify->Publisher(CNotifyManager::ENOTIFY_PREPARE_ACCEPT_NEW_CONNECT,"Accept a new connect");
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
	OnvifiocppSockaddr pLocalAddr, pRemoteAddr;
	int nLocalLen, nRmoteLen;
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
	
	
	//准备创建新连接
	CONNID dwConnID = 0;
	SOCKET hSocket	= pBufferObj->hClientFd;
	BOOL isOK = TRUE;
	TSocketObj* pNewSocketObj;
	do{
		//首先锁定一个连接id，这里的处理需要小心谨慎 ----后期需要优化的点
		if(m_eState != EONVIFIOCPSTATE_BIRTH && !m_bfActiveSocktObj.AcquireLock(dwConnID))
		{
			isOK = FALSE;
			break;
		}

		pNewSocketObj = GetNewSocketObj(dwConnID, hSocket);
		if(pSocketObj == NULL)
		{
			m_pNotify->Publisher(CNotifyManager::ENOTIFY_ACCEPT_FAIL,"a new connect recv buf no socketObj");
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
   pNewSocketObj->remoteAddr = *pRemoteAddr;
   pNewSocketObj->connTime	  = ::timeGetTime();
   pNewSocketObj->activeTime = pNewSocketObj->connTime;

   //加入无锁hash，正式接受连接。
   m_bfActiveSocktObj.ReleaseLock(dwConnID, pNewSocketObj);
      
   //准备向完成端口投递接收数据请求
   ::CreateIoCompletionPort((HANDLE)hSocket, m_hCompPort, (ULONG_PTR)pNewSocketObj, 0);

   //投递前通知应用层
   m_pNotify->Publisher(CNotifyManager::ENOTIFY_PREPARE_RECV,"prepare recv");

   //投递
   int nRet = ActionPostRecv(pBufferObj,pNewSocketObj,TRUE);
   if(nRet != NO_ERROR)
   {
	   //释放pSocketObj
	   AddToInactiveSocketObj(pNewSocketObj);
   }
   return;
}

//server端这个方法不能暴露给上层来直接调用
int COnvifIocp::ActionPostRecv(TBufferObj *pBufferObj,TSocketObj *pSocketObj,BOOL bPost)
{
	
	int nRet				= NO_ERROR;
	DWORD dwFlag			= 0;
	DWORD dwBytes			= 0;
	pBufferObj->hClientFd	= pSocketObj->socket;
	pBufferObj->eState  	= EONVIFIOCP_BUFFER_STATE_RECEIVE;
	pBufferObj->buff.len    = pBufferObj->capacity;

	if(::WSARecv(
		pBufferObj->hClientFd,
		&pBufferObj->buff,
		1,
		&dwBytes,
		&dwFlag,
		bPost? &pBufferObj->ov:nullptr,
		nullptr
		) == SOCKET_ERROR)
	{
		nRet = ::WSAGetLastError();
		//io进行中，不要返回错误
		if(nRet == WSA_IO_PENDING)
		{
			nRet = NO_ERROR;
		}
		else
		{
			LOG_ERROR << "WSARecv error: " << nRet;
		}
	}
	else
	{
		if(!bPost &&dwBytes > 0)
		{
			pBufferObj->buff.len = dwBytes;
		}
		else if(!bPost)
		{
			nRet = WSAEDISCON;
		}
		else
		{
			nRet = ::WSAGetLastError();
		}
	}

	return nRet;
}

BOOL COnvifIocp::ActionPostAccept(SOCKET hListenFd)
{
	int nRet = NO_ERROR;
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
	pBufferObj->eState = EONVIFIOCP_BUFFER_STATE_ACCEPT;

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
			//io进行中，不要返回错误
			nRet = ::WSAGetLastError();
			if(nRet == WSA_IO_PENDING)
			{
				nRet = NO_ERROR;
			}
			else
			{
				//应用层需要释放SOcket
				LOG_ERROR << "AcceptEx error:" << nRet;
				return FALSE;
			}
		}
	}while(0);
	return (nRet == NO_ERROR);
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
				LOG_ERROR<< "ONVIFIOCP_EV_ACCEPT !!";
				if(ActionPostAccept(pBackObj))
				{
					m_pNotify->Publisher(CNotifyManager::ENOTIFY_ACCEPT_OK,"Accept ok");
				}
				else
				{
					m_pNotify->Publisher(CNotifyManager::ENOTIFY_ACCEPT_FAIL,"See log");
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
			pBufferObj->dwRecvLen = dwBytes;
			DoRecvState(pBufferObj,pSocketObj);
			break;
	}
	return;
}


unsigned int __stdcall COnvifIocp::TaskProc(LPVOID pv)
{
	DWORD dwErrorCode = NO_ERROR;
    DWORD dwBytes;
	TBufferObj* pBufferObj = nullptr;
    TSocketObj* pSocketObj = nullptr;
    OVERLAPPED* pOverlapped;
	COnvifIocp *pIocpApi = (COnvifIocp *)pv;
    while(TRUE)
    {
		pBufferObj = nullptr;
		pSocketObj = nullptr;
		CONNID dwConnID = NULL;
		pOverlapped = NULL;

		BOOL bRet = ::GetQueuedCompletionStatus
			(
			pIocpApi->GetComPort(),
			&dwBytes,
			(PULONG_PTR)&pSocketObj,
			&pOverlapped,
			INFINITE
			);

		LOG_DEBUG <<" thread dwBytes:" << dwBytes << " bRet:" <<bRet;
		if(!bRet)
		{
			LOG_DEBUG << "error:" << WSAGetLastError();
		}
		//Onvif iocp事件处理
		if(pOverlapped == NULL)
		{
			pIocpApi->EventSwicth(pOverlapped, dwBytes, (ULONG_PTR)pSocketObj);
			continue;
		}
	
		//得到控制块
		pBufferObj = CONTAINING_RECORD(pOverlapped, TBufferObj, ov);
		dwConnID = pBufferObj->eState !=  EONVIFIOCP_BUFFER_STATE_ACCEPT? pSocketObj->connID : 0;

		//处理底层事件
		pIocpApi->EventSwicth(dwConnID,pSocketObj,pBufferObj,dwBytes,dwErrorCode);
	}
	//得到了pobj然后就依据obj的状态来出来处理底层事件---
    return  0;  
}

//默认预投递accept为50个，
BOOL COnvifIocp::EventPostAccept(SOCKET hListenFd,ULONG_PTR pBackObj,DWORD dwAcceptNum)
{
	ASSERT(hListenFd != INVALID_SOCKET);
	//这里需要创建预accept来提高性能。
	if(m_bIsServer && m_eState != EONVIFIOCPSTATE_ACCEPT_PREPARED)
	{
	
		int nRet = (int)::CreateIoCompletionPort((HANDLE)hListenFd, m_hCompPort, (ULONG_PTR)hListenFd, 0);
		if(!nRet)
		{
			LOG_ERROR<<"EventPostAccept CreateIoCompletionPort！！" << WSAGetLastError();
			return FALSE;
		}

        if(dwAcceptNum == 0 || dwAcceptNum > ONVIFIOCP_DEFAULT_ACCEPT_NUM_MAX)
        {
            LOG_ERROR << "Prepare accept number is bad number:" << dwAcceptNum;
            return FALSE;
        }

		//预置的accept数量由上层提供
		for(DWORD i = 0; i < dwAcceptNum; i++)
		{
			if(!::PostQueuedCompletionStatus(m_hCompPort, ONVIFIOCP_EV_ACCEPT, (ULONG_PTR)hListenFd, nullptr))
			{
                LOG_ERROR << "PostQueuedCompletionStatus is bad return num:" << dwAcceptNum 
					<< "last error:" << WSAGetLastError(); 
                return FALSE;
			}  
		}
		m_eState = EONVIFIOCPSTATE_ACCEPT_PREPARED;
		return TRUE;
	}
	else
	{
		LOG_ERROR<< "Server error post accept!!";
		return FALSE;
	}

	//这里需要上层去触发继续欧accept和增加预accept
	if(m_bIsServer && m_eState == EONVIFIOCPSTATE_ACCEPT_PREPARED)
	{
		if(!::PostQueuedCompletionStatus(m_hCompPort, ONVIFIOCP_EV_ACCEPT, (ULONG_PTR)hListenFd, nullptr))
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
	CNotifyManager notifyobj;
	COnvifIocp testObj(TRUE,2,&notifyobj);
	TParCreateSocket tpar;
	testObj.Start();
	SOCKET testfd = testObj.CreateSocket(tpar);
	testObj.EventPostAccept(testfd,NULL,30);
	while(1)
	{
		Sleep(0);
	}
	return 0;
}
