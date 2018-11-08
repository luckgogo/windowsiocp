#include "COnvifIocp.h"
#include <MSTcpIP.h >
#pragma comment(lib, "ws2_32")


//����socket
SOCKET  COnvifIocp::CreateSocket(TParCreateSocket& ScoketPar)
{
	SOCKET retSocket = socket(AF_INET,SOCK_STREAM, IPPROTO_TCP);
	if(retSocket == INVALID_SOCKET)
	{
		LOG_ERROR << "CreateSocket"<< WSAGetLastError(); 
		return INVALID_SOCKET;
	}

	//���÷�����
	BOOL bNoBlock = TRUE;
	::ioctlsocket(retSocket, FIONBIO, (ULONG*)&bNoBlock);

	//���ñ���---����socket�����������iocpģ��֧�֣�Ŀǰ�����ϲ��Լ����á�
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

	//���������
	if(ScoketPar.isServer)
	{
		BOOL isOK = TRUE;
		do{
			//�󶨱��ص�ַ
			SOCKADDR	addr4;
			int nSize = sizeof(SOCKADDR_IN);
			SOCKADDR_IN *p=(SOCKADDR_IN *) &addr4;
			if(::WSAStringToAddress(TEXT("0.0.0.0"), AF_INET, nullptr,&addr4, &nSize) != NO_ERROR)
			{
				LOG_ERROR<< "WSAStringToAddress "<< ScoketPar.lpszBindAddress << " ���ɹ�" << WSAGetLastError();
				isOK = FALSE;
				break;
			}
			p->sin_port = htons(ScoketPar.usPort);

			if(::bind(retSocket, &addr4, nSize) != SOCKET_ERROR)
			{
				//�󶨳ɹ��ͼ���
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


//�ر�socket�ķ�װ
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

//׼��iocp����Դ��˽�жѺ�buffer������
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

		//׼��buffer,ʹ��Ĭ�ϵĳߴ�
		m_BufferObjPool.Prepare();
        m_eState = EONVIFIOCPSTATE_PREPARED;

		//׼��socket������������
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

	//close֮ǰ֪ͨ�ϲ�
	m_pNotify->ActionSwitch(CNotifyManager::ENOTIFY_ACTION_CLOSE,this,(BYTE*)"before close",(int)12);

	//��λ������
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
		//�ȴ��շ�����ʵ�Ѿ�����������֤ҵ��Ľ���,ֻ����������
		CLocalLock<CReentrantSpinGuard>	locallock(pSocktObj->csRecv);
		CLocalLock<CInterCriSec>	locallock2(pSocktObj->csSend);

		if(TSocketObj::IsValid(pSocktObj))
		{
			TSocketObj::Invalid(pSocktObj);
		}
	}

	//�ر�����socket��
	CloseConnectSocket(pSocktObj);
	
	m_bfActiveSocktObj.Remove(pSocktObj->connID);
	TSocketObj::Release(pSocktObj);

	//���뵽����������һ�����ʹ��
	BOOL isOk = m_bfInactiveSocktObj.TryPut(pSocktObj);

	//���������������
	if(!isOk)
	{
		//ֱ��del
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

//����ӿ����ں����Ż������Ƕ���ġ�
TSocketObj*	COnvifIocp::GetNewSocketObj(CONNID dwConnID, SOCKET soClient)
{
	//������������û��
	TSocketObj* pSocketObj = nullptr;

    //�Ż�һ������ʹ��sockObj������ֱ�Ӵ��¿���һ����
	pSocketObj = CreateSocketObj();
	if(pSocketObj == NULL){return NULL;}
	pSocketObj->Reset(dwConnID, soClient);

	return pSocketObj;
}

void COnvifIocp::DoRecvState(TBufferObj *pBufferObj,TSocketObj *pSocketObj)
{
	//Ϊ�˱�֤�ϲ���յ�����һ���ԣ��������,������Ƕ���Ĳ������мǣ�
	CLocalLock<CReentrantSpinGuard> localLockHelper(pSocketObj->csRecv);
	m_pNotify->Publisher(CNotifyManager::ENOTIFY_RECVED,"�Ѿ����յ���������");
	
	//Ĭ��Ϊ������ʽ��������Ҫ����pullģʽ
	int nRet =  m_pNotify->ActionSwitch(CNotifyManager::ENOTIFY_ACTIOON_RECV,
		this,(BYTE*)(pBufferObj->buff.buf),pBufferObj->dwRecvLen);
	
	//һֱ����
	if(nRet == Action_Continue)
	{
		do{
			nRet = ActionPostRecv(pBufferObj,pSocketObj,FALSE);
			if(nRet != NO_ERROR)
			{
				//����Զ˹ر�
				//if(nRet == WSAEDISCON) �Ժ����
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
	//����֪ͨ�ϲ㣬׼�����������ӣ����ϲ�����Ƿ��������ɶ˿��ύaccept
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

	//�õ����Ӳ���
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
	
	
	//׼������������
	CONNID dwConnID = 0;
	SOCKET hSocket	= pBufferObj->hClientFd;
	BOOL isOK = TRUE;
	TSocketObj* pNewSocketObj;
	do{
		//��������һ������id������Ĵ�����ҪС�Ľ��� ----������Ҫ�Ż��ĵ�
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
	   //���Ϲر�
	   ManualCloseSocket(hSocket,SD_BOTH);

	   //��������� ע���������accept��û����.
	   m_BufferObjPool.PutFreeItem(pBufferObj);
	   LOG_ERROR << "DoAcceptState error no Active Sockets !!";
	   return;

   }

   //����ʧ��~~
   ::setsockopt(hSocket, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (CHAR*)&pBufferObj->hlistenFd, sizeof(SOCKET));


   //��ʼ��socketobj
   pNewSocketObj->remoteAddr = *pRemoteAddr;
   pNewSocketObj->connTime	  = ::timeGetTime();
   pNewSocketObj->activeTime = pNewSocketObj->connTime;

   //��������hash����ʽ�������ӡ�
   m_bfActiveSocktObj.ReleaseLock(dwConnID, pNewSocketObj);
      
   //׼������ɶ˿�Ͷ�ݽ�����������
   ::CreateIoCompletionPort((HANDLE)hSocket, m_hCompPort, (ULONG_PTR)pNewSocketObj, 0);

   //Ͷ��ǰ֪ͨӦ�ò�
   m_pNotify->Publisher(CNotifyManager::ENOTIFY_PREPARE_RECV,"prepare recv");

   //Ͷ��
   int nRet = ActionPostRecv(pBufferObj,pNewSocketObj,TRUE);
   if(nRet != NO_ERROR)
   {
	   //�ͷ�pSocketObj
	   AddToInactiveSocketObj(pNewSocketObj);
   }
   return;
}

//server������������ܱ�¶���ϲ���ֱ�ӵ���
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
		//io�����У���Ҫ���ش���
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
	//׼��һ������socket ������ipv6
	SOCKET	hClientFd = socket(AF_INET,SOCK_STREAM, IPPROTO_TCP);
	if(hClientFd == INVALID_SOCKET)
	{
		LOG_ERROR << "socket error:" << ::WSAGetLastError();
		return FALSE;
	}

	//������һ��bufferʵ�壬������Ϊ�ǿ�ܵĿ��ƿ飬�������ݡ�
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

	//׼������ϵͳ����
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
			//io�����У���Ҫ���ش���
			nRet = ::WSAGetLastError();
			if(nRet == WSA_IO_PENDING)
			{
				nRet = NO_ERROR;
			}
			else
			{
				//Ӧ�ò���Ҫ�ͷ�SOcket
				LOG_ERROR << "AcceptEx error:" << nRet;
				return FALSE;
			}
		}
	}while(0);
	return (nRet == NO_ERROR);
}

//ǿ��Ӧ�ò㴦���ܵĸ���״��
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

//�����¼�������
void COnvifIocp::EventSwicth(CONNID ptrConnid,TSocketObj *pSocketObj,TBufferObj *pBufferObj,DWORD dwBytes,DWORD dwErrorCode)
{
	if(dwBytes == 0 && pBufferObj->eState != EONVIFIOCP_BUFFER_STATE_ACCEPT)
	{
		//��buffer���� ��Ϊ�漰�رտͻ���fd���ԵȺ�������
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
		//Onvif iocp�¼�����
		if(pOverlapped == NULL)
		{
			pIocpApi->EventSwicth(pOverlapped, dwBytes, (ULONG_PTR)pSocketObj);
			continue;
		}
	
		//�õ����ƿ�
		pBufferObj = CONTAINING_RECORD(pOverlapped, TBufferObj, ov);
		dwConnID = pBufferObj->eState !=  EONVIFIOCP_BUFFER_STATE_ACCEPT? pSocketObj->connID : 0;

		//����ײ��¼�
		pIocpApi->EventSwicth(dwConnID,pSocketObj,pBufferObj,dwBytes,dwErrorCode);
	}
	//�õ���pobjȻ�������obj��״̬����������ײ��¼�---
    return  0;  
}

//Ĭ��ԤͶ��acceptΪ50����
BOOL COnvifIocp::EventPostAccept(SOCKET hListenFd,ULONG_PTR pBackObj,DWORD dwAcceptNum)
{
	ASSERT(hListenFd != INVALID_SOCKET);
	//������Ҫ����Ԥaccept��������ܡ�
	if(m_bIsServer && m_eState != EONVIFIOCPSTATE_ACCEPT_PREPARED)
	{
	
		int nRet = (int)::CreateIoCompletionPort((HANDLE)hListenFd, m_hCompPort, (ULONG_PTR)hListenFd, 0);
		if(!nRet)
		{
			LOG_ERROR<<"EventPostAccept CreateIoCompletionPort����" << WSAGetLastError();
			return FALSE;
		}

        if(dwAcceptNum == 0 || dwAcceptNum > ONVIFIOCP_DEFAULT_ACCEPT_NUM_MAX)
        {
            LOG_ERROR << "Prepare accept number is bad number:" << dwAcceptNum;
            return FALSE;
        }

		//Ԥ�õ�accept�������ϲ��ṩ
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

	//������Ҫ�ϲ�ȥ��������ŷaccept������Ԥaccept
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
