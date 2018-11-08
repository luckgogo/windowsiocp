#pragma once
/************************************************************************/
/* ONVIFIOCP 配置                                                                     */
/************************************************************************/
#include <windows.h>
//创建socket配置
struct TParCreateSocket
{

	//TCP 默认心跳包间隔 
#define ONVIFIOCP_TCP_KEEPALIVE_TIME				(30 * 1000)

	// TCP 默认心跳确认包检测间隔 
#define ONVIFIOCP_TCP_KEEPALIVE_INTERVAL			(10 * 1000)

  //TCP Server 默认 Listen 队列大小
#define ONVIFIOCP_TCP_SERVER_SOCKET_LISTEN_QUEUE	0x7fffffff

	//默认server端地址 和端口
#define ONVIFIOCP_TCP_BIND_ADDR _T("0.0.0.0")
#define ONVIFIOCP_TCP_BIND_PORT (USHORT)(8088)


	
	BOOL isServer;
	DWORD dwKeepAliveTime;
	DWORD dwKeepAliveInterval;

	//server端
	DWORD dwSocketListenQueue;
	LPCTSTR lpszBindAddress;
	USHORT  usPort;


	TParCreateSocket()
	{
		isServer = TRUE;
		dwKeepAliveTime     = ONVIFIOCP_TCP_KEEPALIVE_TIME;
		dwKeepAliveInterval = ONVIFIOCP_TCP_KEEPALIVE_INTERVAL;
		dwSocketListenQueue = ONVIFIOCP_TCP_SERVER_SOCKET_LISTEN_QUEUE;
		lpszBindAddress     = CString(ONVIFIOCP_TCP_BIND_ADDR);
		usPort              = ONVIFIOCP_TCP_BIND_PORT;

	}

	//暂时使用默认值，顾参数设置方法以后再写。
	//设置参数
	inline void SetKeepAliveTime(DWORD dwValue)
	{
		//检查参数然后赋值
		dwKeepAliveTime = dwValue;
	}
};