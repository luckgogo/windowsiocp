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

		//׼��buffer,ʹ��Ĭ�ϵĳߴ�
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

//����ӿ����ں����Ż������Ƕ���ġ�
TSocketObj*	COnvifIocp::GetNewSocketObj(CONNID dwConnID, SOCKET soClient)
{
	//������������û��
	DWORD dwIndex;
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
	pNotify->Publisher(CNotifyManager::ENOTIFY_PREPARE_RECVED,"�Ѿ����յ���������");
	//ֱ���Ƹ��ϲ�? �Ȳ�����
	return;
}

void COnvifIocp::DoAcceptState(TBufferObj *pBufferObj,TSocketObj *pSocketObj)
{
	//����֪ͨ�ϲ㣬׼�����������ӣ����ϲ�����Ƿ��������ɶ˿��ύaccept
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

	//�õ����Ӳ���
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
	
	//׼������������
	CONNID dwConnID = 0;
	SOCKET hSocket	= pBufferObj->hClientFd;
	BOOL isOK = TRUE;
	do{
		//��������һ������id������Ĵ�����ҪС�Ľ��� ----������Ҫ�Ż��ĵ�
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
   pSocketObj->remoteAddr = pRemoteAddr;
   pSocketObj->connTime	  = ::timeGetTime();
   pSocketObj->activeTime = pSocketObj->connTime;

   //��������hash����ʽ�������ӡ�
   m_bfActiveSocktObj.ReleaseLock(dwConnID, pSocketObj);
      
   //׼������ɶ˿�Ͷ�ݽ�����������
   ::CreateIoCompletionPort((HANDLE)hSocket, m_hCompPort, (ULONG_PTR)pSocketObj, 0);

   //Ͷ��ǰ֪ͨӦ�ò�
   pNotify->Publisher(CNotifyManager::ENOTIFY_PREPARE_RECV,"prepare recv");

   //Ͷ��
   ActionPostRecv(pBufferObj,pSocketObj);
 
   return;
}

//server������������ܱ�¶���ϲ���ֱ�ӵ���
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
	pBufferObj->eState = EONVIFIOCP_STATE_ACCEPT;

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
			//Ӧ�ò���Ҫ�ͷ�SOcket
			LOG_ERROR << "AcceptEx error:" << ::WSAGetLastError();
			return FALSE;
		}
	}while(0);
	return TRUE;
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

		//Onvif iocp�¼�����
		if(pOverlapped == NULL)
		{
			EventSwicth(pOverlapped, dwBytes, (ULONG_PTR)pSocketObj);
			continue;
		}
	
		//�õ����ƿ�
		pBufferObj = CONTAINING_RECORD(pOverlapped, TBufferObj, ov);
		dwConnID = pBufferObj->eState !=  EONVIFIOCP_BUFFER_STATE_ACCEPT? pSocketObj->connID : 0;

		//����ײ��¼�
		EventSwicth(dwConnID,pSocketObj,pBufferObj,dwBytes,dwErrorCode);
	}
	//�õ���pobjȻ�������obj��״̬����������ײ��¼�---
    return  0;  
}

//Ĭ��ԤͶ��acceptΪ50����
BOOL COnvifIocp::EventPostAccept(SOCKET hListenFd,ULONG_PTR pBackObj,DWORD dwAcceptNum)
{
	//������Ҫ����Ԥaccept��������ܡ�
	if(m_bIsServer && m_eState != EONVIFIOCPSTATE_ACCEPT_PREPARED &&
	 ::CreateIoCompletionPort((HANDLE)hListenFd, m_hCompPort, (ULONG_PTR)hListenFd, 0))
	{
        if(dwAcceptNum == 0 || dwAcceptNum > ONVIFIOCP_DEFAULT_ACCEPT_NUM_MAX)
        {
            LOG_ERROR << "Prepare accept number is bad number:" << dwAcceptNum;
            return FALSE;
        }

		//Ԥ�õ�accept�������ϲ��ṩ
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

	//������Ҫ�ϲ�ȥ��������ŷaccept������Ԥaccept
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
