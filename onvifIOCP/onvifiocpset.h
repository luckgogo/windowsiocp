#pragma once
/************************************************************************/
/* ONVIFIOCP ����                                                                     */
/************************************************************************/
#include <windows.h>
//����socket����
struct TParCreateSocket
{

	//TCP Ĭ����������� 
#define ONVIFIOCP_TCP_KEEPALIVE_TIME				(30 * 1000)

	// TCP Ĭ������ȷ�ϰ������ 
#define ONVIFIOCP_TCP_KEEPALIVE_INTERVAL			(10 * 1000)

  //TCP Server Ĭ�� Listen ���д�С
#define ONVIFIOCP_TCP_SERVER_SOCKET_LISTEN_QUEUE	0x7fffffff

	//Ĭ��server�˵�ַ �Ͷ˿�
#define ONVIFIOCP_TCP_BIND_ADDR _T("0.0.0.0")
#define ONVIFIOCP_TCP_BIND_PORT (USHORT)(8088)


	
	BOOL isServer;
	DWORD dwKeepAliveTime;
	DWORD dwKeepAliveInterval;

	//server��
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

	//��ʱʹ��Ĭ��ֵ���˲������÷����Ժ���д��
	//���ò���
	inline void SetKeepAliveTime(DWORD dwValue)
	{
		//������Ȼ��ֵ
		dwKeepAliveTime = dwValue;
	}
};