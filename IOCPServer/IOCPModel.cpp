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
	

	//初始化套接字库
	LoadSocketLib();

}


CIOCPModel::~CIOCPModel()
{
	StopServer();
}

//线程函数
DWORD WINAPI CIOCPModel::WorkerThreadFun(LPVOID lpParam)
{
	//获取参数
	THREADPARAM_WORKER *pParam = (THREADPARAM_WORKER *)lpParam;
	CIOCPModel * pIOCPModel = (CIOCPModel *)pParam->m_IOCPModel;
	int nThreadNo = pParam->m_noThread;

	printf("工作者线程启动，ID：%d\n", nThreadNo);
	
	OVERLAPPED *ol = nullptr;
	PPER_SOCKET_CONTEXT pSocketContext = nullptr;
	DWORD dwBytestransferred = 0;

	//等待事件退出
	while (WAIT_OBJECT_0 != WaitForSingleObject(pIOCPModel->m_hQuitEvent, 0))
	{
		bool retVal = GetQueuedCompletionStatus(pIOCPModel->m_hIOCP,&dwBytestransferred,
			(PULONG_PTR)&pSocketContext,&ol,INFINITE);
		if (EXIT_CODE == (DWORD)pSocketContext)
		{
			break;
		}
		//出现了错误  处理错误
		else if (!retVal)
		{
			DWORD dwErr = GetLastError();
			if (!pIOCPModel->SolveHandleError(pSocketContext, dwErr))
			{
				break;
			}
		}
		//开始处理请求
		else 
		{
			//寻找以ol开头的per_io_context的单io数据
			PPER_IO_CONTEXT pIoContext = CONTAINING_RECORD(ol, PER_IO_CONTEXT, m_overLapped);
			//客户端断开了
			if ((0 == dwBytestransferred) && (SEND == pIoContext->m_type || RECV == pIoContext->m_type))
			{
				//输出断开的客户端的信息
				printf("客户端 %s:%d断开连接!\n", inet_ntoa(pSocketContext->m_clientAddr.sin_addr), 
					ntohs(pSocketContext->m_clientAddr.sin_port));
				pIOCPModel->RemoveSocketContext(pSocketContext);
			}
			else 
			{
				//分别处理三种操作请求
				switch (pIoContext->m_type)
				{
				case ACCEPT:
					{
						pIOCPModel->DoAccept(pSocketContext,pIoContext);
					}
					break;
				case SEND:
					{
						pIOCPModel->DoSend(pSocketContext, pIoContext);
					}
					break;
				case RECV:
					{
						pIOCPModel->DoRecv(pSocketContext, pIoContext);
					}
					break;
				default:
					printf("WorkThread中的 pIoContext->m_OpType 参数异常.\n");
					break;
				}
			}
		}
	}
	printf("工作者线程 %d 号退出！\n", nThreadNo);
	Sleep(10);
	RELEASE(pParam);
	return 0;
}

bool CIOCPModel::LoadSocketLib()
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
	if (NULL == CreateIoCompletionPort((HANDLE)m_pListenContext->m_socket, m_hIOCP, (DWORD)m_pListenContext,0))
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
	Sleep(10);
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

//注意 这是所有进程通用的资源 所以必须设一个同步互斥量
void CIOCPModel::AddToSocketContextList(PPER_SOCKET_CONTEXT p)
{
	EnterCriticalSection(&m_csContextList);
	m_clientSocketContextArray.push_back(p);
	LeaveCriticalSection(&m_csContextList);
}

void CIOCPModel::RemoveSocketContext(PPER_SOCKET_CONTEXT p)
{
	EnterCriticalSection(&m_csContextList);
	for (auto i = m_clientSocketContextArray.begin(); i != m_clientSocketContextArray.end();)
	{
		if (p == (*i))
		{
			RELEASE((*i));
			i = m_clientSocketContextArray.erase(i);
			break;
		}
		i++;
	}
	LeaveCriticalSection(&m_csContextList);
}

void CIOCPModel::ClearSocketContext()
{
	for (int i = 0; i < m_clientSocketContextArray.size(); i++)
	{
		delete m_clientSocketContextArray[i];
	}
	m_clientSocketContextArray.clear();
}

bool CIOCPModel::SolveHandleError(PPER_SOCKET_CONTEXT pSockeContext, const DWORD & dwErr)
{
	//超时
	if (WAIT_TIMEOUT == dwErr)
	{
		//确认客户端是不是异常退出了
		if (!IsSocketAlive(pSockeContext->m_socket))
		{
			printf("客户端异常退出!\n");
			RemoveSocketContext(pSockeContext);
			return true;
		}
		else
		{
			printf("网络超时！重试中......\n");
			return true;
		}
	}
	//客户端异常退出
	else if (ERROR_NETNAME_DELETED == dwErr)
	{
		printf("客户端异常退出!\n");
		RemoveSocketContext(pSockeContext);
		return true;
	}
	printf("完成端口发生错误！线程退出，错误码：%d\n",dwErr);
	return false;
}

bool CIOCPModel::StartServer()
{
	//初始化线程互斥量
	InitializeCriticalSection(&m_csContextList);
	//建立线程退出事件 默认无信号  不默认重置信号状态
	m_hQuitEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (NULL == m_hQuitEvent)
	{
		printf("建立线程退出事件失败！\n");
		return false;
	}
	//设置服务器地址信息
	m_serverAddr.sin_addr.S_un.S_addr = htonl(INADDR_ANY);
	m_serverAddr.sin_family = AF_INET;
	m_serverAddr.sin_port = htons(m_nPort);

	if (false == InitIOCP())
	{
		printf("初始化IOCP失败！\n");
		return false;
	}
	else printf("ICOP初始化完成！\n");
	if (false == InitWorkerThread())
	{
		printf("初始化线程失败！\n");
		return false;
	}
	else printf("线程初始化完成！\n");
	if (false == InitSocket())
	{
		printf("socket初始化失败！\n");
		return false;
	}
	else printf("socket初始化完成！\n");
	return true;
}

void CIOCPModel::StopServer()
{
	if (nullptr != m_pListenContext&&INVALID_SOCKET != m_pListenContext->m_socket)
	{
		//激活线程退出事件
		SetEvent(m_hQuitEvent);
		//通知所有的完成端口操作退出
		for (int i = 0; i < m_numThreads; i++)
		{
			PostQueuedCompletionStatus(m_hIOCP,0,(DWORD)EXIT_CODE,NULL);
		}
		//等待所有进程结束
		WaitForMultipleObjects(m_numThreads,m_phWorkerThreads,true,INFINITE);

		//清除所有客户端信息
		ClearSocketContext();

		printf("停止监听！\n");
		DeInit();
		UnloadSocketLib();
	}
}

//客户端如果异常退出的话（例如客户端崩溃或者拔掉网线之类的）服务端是无法收到客户端断开的通知的
bool CIOCPModel::IsSocketAlive(SOCKET s)
{
	int nBytesSend = send(s,"",0,0);
	if (-1 == nBytesSend)return false;
	return true;
}

bool CIOCPModel::PostAccept(PPER_IO_CONTEXT p)
{
	assert(INVALID_SOCKET != m_pListenContext->m_socket);
	p->ResetBuf();

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

bool CIOCPModel::DoAccept(PPER_SOCKET_CONTEXT pSocketContext, PPER_IO_CONTEXT pIoContext)
{
	sockaddr_in *localAddr = nullptr;
	sockaddr_in *remoteAddr = nullptr;
	int remoteLen = sizeof(sockaddr_in);
	int localLen = sizeof(sockaddr_in);
	//不但可以取得客户端和本地端的地址信息，还能顺便取出客户端发来的第一组数据
	m_lpfnGetAcceptExSockAddrs(pIoContext->m_wsaBuf.buf,
		pIoContext->m_wsaBuf.len-2*(sizeof(sockaddr_in)+16),
		sizeof(sockaddr_in) + 16, sizeof(sockaddr_in) + 16,
		(LPSOCKADDR *)&localAddr,&localLen,(LPSOCKADDR *)&remoteAddr,&remoteLen);
	printf("客户端 %s:%d 连入\n", inet_ntoa(remoteAddr->sin_addr), ntohs(remoteAddr->sin_port));
	printf("客户端 %s:%d 信息:%s\n", inet_ntoa(remoteAddr->sin_addr), ntohs(remoteAddr->sin_port), pIoContext->m_wsaBuf.buf);

	//创建新的客户端context
	PPER_SOCKET_CONTEXT pNewSocketContext = new PER_SOCKET_CONTEXT;
	pNewSocketContext->m_socket = pIoContext->m_socket;
	pNewSocketContext->m_clientAddr = *remoteAddr;
	//将新的socket绑定到完成端口上
	HANDLE retVal = CreateIoCompletionPort((HANDLE)pNewSocketContext->m_socket, m_hIOCP, (DWORD)pNewSocketContext, 0);
	if (NULL == retVal)
	{
		RELEASE(pNewSocketContext);
		printf("执行CreateIoCompletionPort()出现错误.错误代码：%d", GetLastError());
		return false;
	}
	//创建新客户端下的io数据
	PPER_IO_CONTEXT pNewIoContext = pNewSocketContext->GetNewIOContext();
	pNewIoContext->m_type = RECV;
	pNewIoContext->m_socket = pNewSocketContext->m_socket;

	//开始投递
	if (false == PostRecv(pNewIoContext))
	{
		pNewSocketContext->RemoveContext(pNewIoContext);
		return false;
	}

	//投递成功 则将新的socket加入到socketcontext中去 统一管理
	m_clientSocketContextArray.push_back(pNewSocketContext);

	//继续在原socket上投递accept请求
	return PostAccept(pIoContext);
}

bool CIOCPModel::DoSend(PPER_SOCKET_CONTEXT pSocketContext, PPER_IO_CONTEXT pIoContext)
{
	return true;
}

bool CIOCPModel::DoRecv(PPER_SOCKET_CONTEXT pSocketContext, PPER_IO_CONTEXT pIoContext)
{
	sockaddr_in *clientAddr = &pSocketContext->m_clientAddr;
	printf("收到 %s:%d  信息:%s\n",inet_ntoa(clientAddr->sin_addr),
		ntohs(clientAddr->sin_port),pIoContext->m_wsaBuf.buf);
	PostRecv(pIoContext);
	return true;
}

