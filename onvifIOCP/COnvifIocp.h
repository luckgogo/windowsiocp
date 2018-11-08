/************************************************************************/
/* 实现windows iocp框架                                                                    */
/************************************************************************/
#pragma once
#include "glog/logging.h"
#include "OnvifObj.h"
#include <process.h>
#include <vector>
#include <atlstr.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <process.h>
#include "onvifiocpset.h"



#define LOG_ERROR LOG(INFO)
#define LOG_TRACE LOG(INFO)
#define LOG_DEBUG LOG(INFO)
#define LOG_INFO  LOG(INFO)
#define DLOG_TRACE LOG(INFO) << __PRETTY_FUNCTION__ << " this=" << this << " "

//prepare accept number
#define ONVIFIOCP_DEFAULT_ACCEPT_NUM (50)
#define ONVIFIOCP_DEFAULT_ACCEPT_NUM_MAX (1024)
#define ONVIFIOCP_DEFAULT_CONNECT_MAX (300)


//typedef void (*PFunOnvifIocpLog)(const char *pLogStr);
typedef enum
{
	EONVIFIOCP_SUCCESS = 0,
    EONVIFIOCP_HASSTARTED,
	EONVIFIOCP_PARAMETER_NOT_NULL
} EOnvifIocpError;

typedef enum
{
	EONVIFIOCPSTATE_BIRTH = 0,
	EONVIFIOCPSTATE_PREPARED,
	EONVIFIOCPSTATE_ACCEPT_PREPARED,
} EOnvifIocpState;


//IOCP buffer状态 随着网络事件变迁
typedef enum
{
	EONVIFIOCP_BUFFER_STATE_UNKNOWN	= 0,	// Unknown
	EONVIFIOCP_BUFFER_STATE_ACCEPT	= 1,	// Acccept
	EONVIFIOCP_BUFFER_STATE_CONNECT	= 2,	// Connect
	EONVIFIOCP_BUFFER_STATE_SEND	= 3,	// Send
	EONVIFIOCP_BUFFER_STATE_RECEIVE	= 4,	// Receive
	EONVIFIOCP_BUFFER_STATE_CLOSE	= 5,	// Close
} EOnvifBufferObjState;

/*
	动作回调返回值定义
*/
#define Action_Continue (int)(1)



//只支持ipv4，后续需要ipv6的再改造
typedef struct Onvifiocp_Sockaddr
{
	union
	{
		ADDRESS_FAMILY	family;
		SOCKADDR		addr;
		SOCKADDR_IN		addr4;
		SOCKADDR_IN6	addr6;
	};

	inline BOOL IsIPv4()			const	{return family == AF_INET;}
	inline USHORT Port()			const	{return ntohs(addr4.sin_port);}

	inline Onvifiocp_Sockaddr& operator =(const Onvifiocp_Sockaddr& cpyaddr)
	{
		if(this != &cpyaddr)
		{
			memcpy(this, &cpyaddr,sizeof(SOCKADDR_IN));
		}
		return *this;
	}
}OnvifiocpSockaddr,*OnvifiocppSockaddr;


typedef ULONG_PTR	CONNID;

class CNotifyManager;
template<class T> struct TBufferObjListT;
struct TSocketObj;
struct TBufferObj;

class COnvifIocp 
{

private: 
	HANDLE m_hCompPort;
	BOOL m_bIsServer;
	//PFunOnvifIocpLog m_pFunLog;
	EOnvifIocpState m_eState;
    const DWORD m_dwTaskNum;
	const DWORD m_dwMaxConnect;
	CPrivateHeapImpl m_PrivHeap;
	CNotifyManager *m_pNotify;
    std::vector<HANDLE>	m_vtTaskThreads;
	


public:
	HANDLE GetComPort()
	{
		return m_hCompPort;
	}

	COnvifIocp(BOOL isServer,const DWORD dwTaskNum,CNotifyManager *pNotify):
	    m_bIsServer(isServer),m_hCompPort(nullptr),
		m_eState(EONVIFIOCPSTATE_BIRTH),
        m_dwTaskNum(dwTaskNum),
		m_dwMaxConnect(ONVIFIOCP_DEFAULT_CONNECT_MAX),
		m_pNotify(pNotify)
	{
		m_pfnAcceptEx = nullptr;
		m_pfnGetAcceptExSockaddrs  = nullptr;
	}

	SOCKET  CreateSocket(TParCreateSocket& ScoketPar);
	int ManualCloseSocket(SOCKET sock, int iShutdownFlag = 0xFF, BOOL bGraceful=TRUE, BOOL bReuseAddress=FALSE);
    BOOL CreateTaskThreads();
	static unsigned int __stdcall TaskProc(LPVOID pv);
    EOnvifIocpError Start();
	void CloseConnectSocket(TSocketObj *pSocktObj);
	void AddToInactiveSocketObj(TSocketObj *pSocktObj);

public:
	BOOL EventPostAccept(SOCKET hListenFd,ULONG_PTR pBackObj,DWORD dwAcceptNum=ONVIFIOCP_DEFAULT_ACCEPT_NUM);
	

private:
	// IOCP 事件
	enum EnIocpEevent
	{
		ONVIFIOCP_EV_EXIT		= 0x00000000,	// 退出程序
		ONVIFIOCP_EV_ACCEPT		= 0xFFFFFFF1,	// 接受连接
		ONVIFIOCP_EV_DISCONNECT	= 0xFFFFFFF2,	// 断开连接
		ONVIFIOCP_EV_SEND		= 0xFFFFFFF3	// 发送数据
	};

    //框架事件交换机
	void COnvifIocp::EventSwicth(OVERLAPPED* pOverlapped,DWORD dwBytes, ULONG_PTR pBackObj);
	//网络事件交换机
	void EventSwicth(CONNID ptrConnid,TSocketObj *pSocketObj,TBufferObj *pBufferObj,DWORD dwBytes,DWORD dwErrorCode);

	BOOL COnvifIocp::ActionPostAccept(SOCKET hListenFd);
	void COnvifIocp::DoAcceptState(TBufferObj *pBufferObj,TSocketObj *pSocketObj);
	int COnvifIocp::ActionPostRecv(TBufferObj *pBufferObj,TSocketObj *pSocketObj,BOOL bPost);
	void COnvifIocp::DoRecvState(TBufferObj *pBufferObj,TSocketObj *pSocketObj);

	BOOL InitCpmpletePort()
	{
        if(m_hCompPort == nullptr)
		{
		    m_hCompPort	= ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
		}

	    if(m_hCompPort == nullptr)
		{
			LOG_DEBUG << " InitCpmpletePort error" << __FUNCTION__ << ::GetLastError();
		}

		return (m_hCompPort != nullptr);
	}


private:
	//buffer相关成员
	CNodePoolT<TBufferObj> m_BufferObjPool;
	CRingCache2<TSocketObj, CONNID, true> m_bfActiveSocktObj;
    CRingPool<TSocketObj> m_bfInactiveSocktObj;


	TSocketObj*	COnvifIocp::GetNewSocketObj(CONNID dwConnID, SOCKET soClient);
	TSocketObj* COnvifIocp::CreateSocketObj();
private:
	//windows 扩展函数指针
	LPFN_ACCEPTEX		m_pfnAcceptEx;
	LPFN_GETACCEPTEXSOCKADDRS	m_pfnGetAcceptExSockaddrs;
};

//通知管理类
class CNotifyManager
{
public:
	typedef enum{
		ENOTIFY_ACTIOON_RECV,
		ENOTIFY_ACTION_CLOSE
	}EActionType;

	typedef enum{
		ENOTIFY_ACCEPT_OK=0,
		ENOTIFY_ACCEPT_FAIL,
		ENOTIFY_PREPARE_ACCEPT_NEW_CONNECT,

		ENOTIFY_PREPARE_RECV,
		ENOTIFY_RECVED,

		ENOTIFY_ERROR_MSG,
		ENOTIFY_ERROR_UNKNOW
	}ENotifyType; 


	int ActionSwitch(EActionType eAction,COnvifIocp *pApi,BYTE *pBuffer,DWORD dwLen)
	{
		switch(eAction)
		{
			case ENOTIFY_ACTIOON_RECV:
				doActionRecv(pApi,pBuffer,dwLen);
				break;

			case ENOTIFY_ACTION_CLOSE:
				doActionBeforeClose(pApi,pBuffer,dwLen);
				break;


			default:
				break;
		}
		return 0;
	}

private:

	void NotifySwitch(ENotifyType eNotify,char *pMsg)
	{
		if(eNotify >= ENOTIFY_ACCEPT_OK && eNotify <= ENOTIFY_ERROR_UNKNOW)
		{
		
		}
		else
		{
			return;
		}

		switch (eNotify)
		{
			case ENOTIFY_ACCEPT_OK:
				doNotifyAcceptOk(pMsg);
				break;

			case ENOTIFY_ACCEPT_FAIL:
				doNotifyAcceptFail(pMsg);
				break;

			case ENOTIFY_PREPARE_ACCEPT_NEW_CONNECT:
				doNotifyPrepareAcceptNewConnect(pMsg);
				break;
				 	
			case ENOTIFY_PREPARE_RECV:
				doNotifyPrepareRecv(pMsg);
				break;

			case ENOTIFY_RECVED:
				doNotifyRecved(pMsg);
			default:
				break;

		}
	}

public:
	//notify
	void Publisher(ENotifyType eType,char* pchrMsg){return NotifySwitch(eType,pchrMsg);}
	virtual void doNotifyAcceptOk(char *pMsg) {LOG_ERROR<<" Publish NotifyAcceptOk"<< pMsg;return;}
	virtual void doNotifyAcceptFail(char *pMsg){LOG_ERROR<<" Publish NotifyAcceptFail"<< pMsg;return;}
	virtual void doNotifyPrepareRecv(char *pMsg){LOG_ERROR<<" Publish PrepareRecv"<< pMsg;return;}
	virtual void doNotifyPrepareAcceptNewConnect(char *pMsg){LOG_ERROR<<" Publish Prepare Accept a New Connect"<< pMsg;return;}
	virtual void doNotifyRecved(char *pMsg){LOG_ERROR<<" Publish Prepare Recved:"<< pMsg;return;}

	//action 
	virtual void doActionRecv(COnvifIocp *pApi,BYTE *pBuffer,DWORD dwLen) {LOG_ERROR<<"doActionRecv " << dwLen <<" bytes -->" << pBuffer;return;}
	virtual void doActionBeforeClose(COnvifIocp *pApi,BYTE *pBuffer,DWORD dwLen) {LOG_ERROR<<"doActionRecv";return;}
};


template<class T> struct TBufferObjListT : public TSimpleList<T>
{
public:
	int Cat(const BYTE* pData, int length)
	{
		ASSERT(pData != nullptr && length > 0);

		int remain = length;

		while(remain > 0)
		{
			T* pItem = Back();

			if(pItem == nullptr || pItem->IsFull())
				pItem = PushBack(bfPool.PickFreeItem());

			int cat  = pItem->Cat(pData, remain);

			pData	+= cat;
			remain	-= cat;
		}

		return length;
	}

	T* PushTail(const BYTE* pData, int length)
	{
		ASSERT(pData != nullptr && length > 0 && length <= (int)bfPool.GetItemCapacity());

		T* pItem = PushBack(bfPool.PickFreeItem());
		pItem->Cat(pData, length);

		return pItem;
	}

	void Release()
	{
		bfPool.PutFreeItem(*this);
	}

public:
	TBufferObjListT(CNodePoolT<T>& pool) : bfPool(pool)
	{
	}

private:
	CNodePoolT<T>& bfPool;
};

/* socket buffer */
struct TSocketObj : public TSocketObjBase
{
	SOCKET		socket;
	CONNID		connID;
	OnvifiocpSockaddr	remoteAddr;

	ATL::CStringA	host;
	TBufferObjListT<TBufferObj>	m_BuffList;

	TSocketObj(CNodePoolT<TBufferObj>& bfPool)
		: m_BuffList(bfPool)
	{

	}

	static void Release(TSocketObj* pSocketObj)
	{
		__super::Release(pSocketObj);

		pSocketObj->m_BuffList.Release();
	}

	void Reset(CONNID dwConnID, SOCKET soClient)
	{
		Reset(dwConnID);
		host.Empty();

		socket = soClient;
	}

	BOOL GetRemoteHost(LPCSTR* lpszHost, USHORT* pusPort = nullptr)
	{
		*lpszHost = host;

		if(pusPort)
			*pusPort = remoteAddr.Port();

		return (*lpszHost != nullptr && (*lpszHost)[0] != 0);
	}

	void Reset(CONNID dwConnid)
	{
		TSocketObjBase::Reset();
		connID = dwConnid;
	}
};

//具体化 提供给iocp框架使用
struct TBufferObj : public TBufferObjBase<TBufferObj>
{
	static SOCKET hlistenFd;
	SOCKET hClientFd;
	DWORD dwRecvLen;
	EOnvifBufferObjState eState;//这里存储EOnvifBufferObjState

	BOOL ListenFdIsSet(){return hlistenFd != 0;}
	void SetListenFd(SOCKET fd){if(fd > 0)hlistenFd = fd;}
};

SOCKET TBufferObj::hlistenFd = 0;
