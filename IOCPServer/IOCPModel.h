#pragma once
#include <winsock2.h>
#include <iostream>
#include <vector>
#include <string>
#include <string.h>
#include <assert.h>
#include <MSWSock.h>
using namespace std;
#pragma comment(lib,"ws2_32.lib")

#define MAX_POST_ACCEPT 10     //初始投递Accept请求的个数
#define MAX_BUFFLEN   1024     //缓冲区最大长度
#define DEFAULT_PORT  9990     //默认端口
#define THREAD_PER_PROCESSOR 2 //一个处理器对应线程的数量
#define EXIT_CODE NULL         //退出码


#define RELEASE_SOCKET(x) {if((x)!=INVALID_SOCKET){closesocket(x);(x)=INVALID_SOCKET;}}
#define RELEASE_HANDLE(x) {if((x)!=INVALID_HANDLE_VALUE&&(x)!=NULL){CloseHandle(x);(x)=NULL;}}
#define RELEASE(x) {if((x)!=nullptr){delete (x);(x)=nullptr;}}




typedef enum _OPERATION_TYPE
{
	ACCEPT,                  //有新的客户端连接
	RECV,                    //有数据从客户端过来
	SEND,                    //发送数据到客户端
	INITIALIZE               //初始化
}OPERATION_TYPE;

//单IO数据
typedef struct _PER_IO_CONTEXT
{
	OVERLAPPED m_overLapped;          //重叠IO
	OPERATION_TYPE m_type;            //操作类型
	SOCKET m_socket;                  //socket
	WSABUF m_wsaBuf;                  //字符缓冲区
	char m_buffer[MAX_BUFFLEN];       

	_PER_IO_CONTEXT()
	{
		memset(&m_overLapped, 0, sizeof(m_overLapped));
		m_type = INITIALIZE;
		m_socket = INVALID_SOCKET;
		memset(m_buffer, 0, sizeof(m_buffer));
		m_wsaBuf.buf = m_buffer;
		m_wsaBuf.len = MAX_BUFFLEN;
	}

	//重置buf
	void ResetBuf()
	{
		memset(m_buffer, 0, sizeof(m_buffer));
	}

}PER_IO_CONTEXT,*PPER_IO_CONTEXT;


//单句柄数据
typedef struct _PER_SOCKET_CONTEXT
{
	SOCKET m_socket;                           //socket
	SOCKADDR_IN m_clientAddr;                  //客户端地址信息
	vector<PPER_IO_CONTEXT>m_IOContextList;    //装socket上的单io数据

	//初始化
	_PER_SOCKET_CONTEXT()
	{
		m_socket = INVALID_SOCKET;  
		memset(&m_clientAddr, 0, sizeof(m_clientAddr));
	}

	~_PER_SOCKET_CONTEXT()
	{
		if (INVALID_SOCKET != m_socket)
		{
			closesocket(m_socket);
			m_socket = INVALID_SOCKET;
		}
		//释放所有单io数据
		for (int i = 0; i < m_IOContextList.size(); i++)
		{
			delete m_IOContextList[i];
		}
		m_IOContextList.clear();
	}

	//得到一个新的io数据结构并返回
	PPER_IO_CONTEXT GetNewIOContext()
	{
		PPER_IO_CONTEXT p = new PER_IO_CONTEXT;
		m_IOContextList.push_back(p);
		return p;
	}

	//移除掉某一个io数据结构
	void RemoveContext(PPER_IO_CONTEXT p)
	{
		assert(p != nullptr);
		for (auto i = m_IOContextList.begin(); i != m_IOContextList.end();)
		{
			if (*i == p)
			{
				delete (*i);
				(*i) = p = nullptr;
				i = m_IOContextList.erase(i);
				break;
			}
			i++;
		}
	}


}PER_SOCKET_CONTEXT,*PPER_SOCKET_CONTEXT;


class CIOCPModel;
//定义工作者线程参数
typedef struct _THREADPARARM_WORKER
{
	int m_noThread;                         //线程号
	CIOCPModel *m_IOCPModel;                //指向类的指针

}THREADPARAM_WORKER,*PTHREADPARAM_WORKER;




//iocpModel
class CIOCPModel
{
public:
	CIOCPModel();
	~CIOCPModel();
private:
	int m_nPort;                                                //服务器端口
	SOCKADDR_IN m_serverAddr;                                  //服务器ip地址
	int m_numThreads;                                           //线程个数
	HANDLE m_hIOCP;                                             //完成端口句柄
	HANDLE m_hQuitEvent;                                        //推出事件句柄
	HANDLE * m_phWorkerThreads;                                 //工作者线程句柄指针
	CRITICAL_SECTION m_csContextList;                           //线程同步互斥量
	vector<PPER_SOCKET_CONTEXT>m_clientSocketContextArray;      //所有客户端的SocketContext信息
	PPER_SOCKET_CONTEXT m_pListenContext;                       //用于监听
	LPFN_ACCEPTEX                m_lpfnAcceptEx;                // AcceptEx 和 GetAcceptExSockaddrs 的函数指针，用于调用这两个扩展函数
	LPFN_GETACCEPTEXSOCKADDRS    m_lpfnGetAcceptExSockAddrs;

	static DWORD WINAPI WorkerThreadFun(LPVOID lpParam);        //线程函数

	bool LoadSocketLib();                                       //加载套接字库
	void UnloadSocketLib() { WSACleanup(); }                    //卸载套接字库
	bool InitIOCP();                                            //初始化完成端口
	bool InitSocket();                                          //初始化socket
	bool InitWorkerThread();                                    //初始化工作者线程
	void DeInit();                                              //最后全部释放掉


	bool PostAccept(PPER_IO_CONTEXT p);                         //投递accept io请求
	bool PostRecv(PPER_IO_CONTEXT p);                           //投递recv io请求
	bool PostSend(PPER_IO_CONTEXT p);                           //投递send io请求
	                                                            //分别处理三种请求
	bool DoAccept(PPER_SOCKET_CONTEXT pSocketContext,PPER_IO_CONTEXT pIoContext);
	bool DoSend(PPER_SOCKET_CONTEXT pSocketContext, PPER_IO_CONTEXT pIoContext);
	bool DoRecv(PPER_SOCKET_CONTEXT pSocketContext, PPER_IO_CONTEXT pIoContext);
	                                                    
	
	void AddToSocketContextList(PPER_SOCKET_CONTEXT p);         //加入到socketcontext中去 统一管理
	void RemoveSocketContext(PPER_SOCKET_CONTEXT p);            //从socketcontext中删掉
	void ClearSocketContext();                                  //清除掉所有socketcontext的内容

	bool IsSocketAlive(SOCKET s);                               //确认客户端是不是异常退出了
	bool SolveHandleError(PPER_SOCKET_CONTEXT pSockeContext,const DWORD& dwErr);
	                                                            //处理完成端口上的错误
	
public:
	bool StartServer();                                         //启动服务器
	void StopServer();                                          //关闭服务器                      
};

