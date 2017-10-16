#include "stdafx.h"
#include "IOCPModel.h"


CIOCPModel::CIOCPModel()
	:m_nPort(DEFAULT_PORT),
	 m_numThreads(0),
	 m_hIOCP(NULL),
	 m_hQuitEvent(NULL),
	 m_phWorkerThreads(nullptr),
	 m_pListenContext(nullptr),
	 m_lpfnAcceptEx(nullptr),
	 m_lpfnGetAcceptExSockAddrs(nullptr)
{
	//变量初始化
	//初始化线程互斥量
	InitializeCriticalSection(&m_csContextList);
	//建立线程退出事件 默认无信号  不默认重置信号状态
	m_hQuitEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	//设置服务器地址信息
	m_serverAddr.sin_addr.S_un.S_addr = htonl(INADDR_ANY);
	m_serverAddr.sin_family = AF_INET;
	m_serverAddr.sin_port = DEFAULT_PORT;

	//函数
	Init();
}


CIOCPModel::~CIOCPModel()
{
	DeInit();
}

DWORD WINAPI CIOCPModel::WorkerThreadFun(LPVOID lpParam)
{
	//获取参数
	THREADPARAM_WORKER *pParam = (THREADPARAM_WORKER *)lpParam;
	CIOCPModel * pIOCPModel = (CIOCPModel *)pParam->m_IOCPModel;
	int nThreadNo = pParam->m_noThread;

	printf("工作者线程启动，ID：%d\n", nThreadNo);


	return 0;
}

bool CIOCPModel::LoadSocketLab()
{
	WSADATA wsaData;
	//出现错误
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != NO_ERROR)
	{
		printf("初始化winsock 2.2失败\n");
		return false;
	}
	return true;
}

bool CIOCPModel::Init()
{
	bool retVal = LoadSocketLab();
	printf("%d", retVal);
	if (!retVal)return false;
	retVal = InitIOCP();
	printf("%d", retVal);
	if (!retVal)return false;
	retVal = InitWorkerThread();
	printf("%d", retVal);
	if (!retVal)return false;
	retVal = InitSocket();
	printf("%d", retVal);
	if (!retVal)return false;
	return true;
}



bool CIOCPModel::InitIOCP()
{
	m_hIOCP = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
	if (NULL==m_hIOCP)
	{
		printf("建立完成端口失败！错误代码：%d\n", WSAGetLastError());
		return false;
	}
	return true;
}

bool CIOCPModel::InitSocket()
{
	// AcceptEx 和 GetAcceptExSockaddrs 的GUID，用于导出函数指针
	GUID GuidAcceptEx = WSAID_ACCEPTEX;
	GUID GuidGetAcceptExSockAddrs = WSAID_GETACCEPTEXSOCKADDRS;

	// 生成用于监听的Socket的信息
	m_pListenContext = new PER_SOCKET_CONTEXT;

	//注意 需要用wsasocket建立
	m_pListenContext->m_socket = WSASocket(AF_INET, SOCK_STREAM,0, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (INVALID_SOCKET == m_pListenContext->m_socket)
	{
		printf("初始化socket失败！错误码：%d\n", WSAGetLastError());
		return false;
	}
	//绑定到服务器地址
	if (SOCKET_ERROR==bind(m_pListenContext->m_socket, (sockaddr *)&m_serverAddr, sizeof(m_serverAddr)))
	{
		printf("bind()函数执行错误！\n");
		RELEASE_SOCKET(m_pListenContext->m_socket);
		return false;
	}
	//绑定至完成端口
	if (NULL == CreateIoCompletionPort((HANDLE)m_pListenContext->m_socket, m_hIOCP, (DWORD)m_pListenContext, 0))
	{
		printf("绑定listen socket至完成端口失败！错误代码：%d\n", WSAGetLastError());
		RELEASE_SOCKET(m_pListenContext->m_socket);
		return false;
	}
	//开始监听
	if (SOCKET_ERROR == listen(m_pListenContext->m_socket, 10))
	{
		printf("listen()函数执行失败！设置监听失败！错误代码：%d\n", WSAGetLastError());
		RELEASE_SOCKET(m_pListenContext->m_socket);
		return false;
	}


	// 使用AcceptEx函数，因为这个是属于WinSock2规范之外的微软另外提供的扩展函数
	// 所以需要额外获取一下函数的指针，
	// 获取AcceptEx函数指针
	DWORD dwBytes = 0;
	if (SOCKET_ERROR == WSAIoctl(
		m_pListenContext->m_socket,
		SIO_GET_EXTENSION_FUNCTION_POINTER,
		&GuidAcceptEx,
		sizeof(GuidAcceptEx),
		&m_lpfnAcceptEx,
		sizeof(m_lpfnAcceptEx),
		&dwBytes,
		NULL,
		NULL))
	{
		DeInit();
		return false;
	}

	// 获取GetAcceptExSockAddrs函数指针，也是同理
	if (SOCKET_ERROR == WSAIoctl(
		m_pListenContext->m_socket,
		SIO_GET_EXTENSION_FUNCTION_POINTER,
		&GuidGetAcceptExSockAddrs,
		sizeof(GuidGetAcceptExSockAddrs),
		&m_lpfnGetAcceptExSockAddrs,
		sizeof(m_lpfnGetAcceptExSockAddrs),
		&dwBytes,
		NULL,
		NULL))
	{
		DeInit();
		return false;
	}

	//为acceptex准备参数，然后投递io请求
	for (int i = 0; i < MAX_POST_ACCEPT; i++)
	{
		//新建一个io_context
		PPER_IO_CONTEXT p = m_pListenContext->GetNewIOContext();
		if (false == PostAccept(p))
		{
			m_pListenContext->RemoveContext(p);
			return false;
		}
	}

	return true;
}

bool CIOCPModel::InitWorkerThread()
{
	//获得处理器数量
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	int numOfProcessors = si.dwNumberOfProcessors;
	m_numThreads = THREAD_PER_PROCESSOR*numOfProcessors;
	//初始化线程
	m_phWorkerThreads = new HANDLE[m_numThreads];
	DWORD nWorkerID;
	for (int i = 0; i < m_numThreads; i++)
	{
		PTHREADPARAM_WORKER param = new THREADPARAM_WORKER;
		param->m_IOCPModel = this;
		param->m_noThread = i + 1;
		m_phWorkerThreads[i] = CreateThread(0, 0, WorkerThreadFun, (LPVOID)param, 0, &nWorkerID);
	}
	Sleep(1);
	printf("建立工作者线程 %d个\n", m_numThreads);


	return true;
}

void CIOCPModel::DeInit()
{
	//删除线程互斥量
	DeleteCriticalSection(&m_csContextList);
	//释放iocp端口句柄
	RELEASE_HANDLE(m_hIOCP);
	//关闭事件
	RELEASE_HANDLE(m_hQuitEvent);
	//关闭释放线程
	for (int i = 0; i < m_numThreads; i++)
	{
		RELEASE_HANDLE(m_phWorkerThreads[i]);
	}
	//删除
	delete[] m_phWorkerThreads;

	printf("释放资源完毕！\n");
}

bool CIOCPModel::PostAccept(PPER_IO_CONTEXT p)
{
	assert(INVALID_SOCKET != m_pListenContext->m_socket);
	

	DWORD dwbytes = 0;
	p->m_type = ACCEPT;
	OVERLAPPED *olp = &p->m_overLapped;
	WSABUF *wb = &p->m_wsaBuf;

	//同时为以后新连入的客户端准备好socket 这是与accept最大的区别
	p->m_socket = WSASocket(AF_INET,SOCK_STREAM,0,NULL,0,WSA_FLAG_OVERLAPPED);
	if (INVALID_SOCKET == p->m_socket)
	{
		printf("创建用于accept的socket失败!错误码：%\n", WSAGetLastError());
		return false;
	}


	//投递
	if (false == m_lpfnAcceptEx(m_pListenContext->m_socket, p->m_socket, wb->buf, wb->len - 2 * (sizeof(sockaddr_in) + 16),
		sizeof(sockaddr_in) + 16, sizeof(sockaddr_in) + 16, &dwbytes, olp))
	{
		if (WSA_IO_PENDING != WSAGetLastError())
		{
			printf("投递失败！错误码：%\n", WSAGetLastError());
			return false;
		}
	}

	return true;
}

bool CIOCPModel::PostRecv(PPER_IO_CONTEXT p)
{
	//重置
	p->ResetBuf();

	//初始化变量
	DWORD dwFlags = 0;
	DWORD dwBytes = 0;
	WSABUF *wb = &p->m_wsaBuf;
	OVERLAPPED *ol = &p->m_overLapped;

	int retVal = WSARecv(p->m_socket, wb, 1, &dwBytes, &dwBytes, ol, NULL);
	if (retVal == SOCKET_ERROR&&WSAGetLastError() != WSA_IO_PENDING)
	{
		printf("投递recv失败! 错误码：%d\n ", WSAGetLastError());
		return false;
	}
	return true;
}

bool CIOCPModel::PostSend(PPER_IO_CONTEXT p)
{
	return true;
}

