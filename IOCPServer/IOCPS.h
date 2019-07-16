// IOCPS.h: interface for the IOCPS class. V 1.15
//
//////////////////////////////////////////////////////////////////////

#if !defined(AFX_IOCPS_H__4D63F25E_B852_46D7_9A42_CF060F5E544D__INCLUDED_)
#define AFX_IOCPS_H__4D63F25E_B852_46D7_9A42_CF060F5E544D__INCLUDED_

#if _MSC_VER > 1000
#pragma once
#endif // _MSC_VER > 1000

#define IOCPSERVERVERSION "IOCP Server/Client system written by Amin Gholiha. Copyright (C) 2005"
#define TRANSFERFILEFUNCTIONALITY  // to use filetransfer (transmitfile function) 

/*
*  Add this if you want to be able to block certain IP address or
*  just allow one connection per IP.
*/
#define SIMPLESECURITY

#include <vector>
#include <list>
#include <queue>
#include <map>
using namespace std;

#include "IOCPBuffer.h"
#include "Lock.h"

/*
* Type of operations.
*/
enum IOType // status code
{
    IOInitialize,            // The client just connected
    IOZeroByteRead,          // Read zero Byte from client (dummy for avoiding The System Blocking error)
    IOZeroReadCompleted,     // Read Zero Byte completed. (se IOZeroByteRead)
    IORead,                  // Read from the client
    IOReadCompleted,         // Read completed
    IOWrite,                 // Write to the Client
    IOWriteCompleted,        // Write Completed
    IOTransmitFileCompleted, // TransmitFileCompleted
    IOPostedPackage,         // Used to post Packages into IOCP port: usually call PostQueuedCompletionStatus later
};

static const char* IOTypeString[] = 
{
    "IOInitialize",
    "IOZeroByteRead",
    "IOZeroReadCompleted",
    "IORead",
    "IOReadCompleted",
    "IOWrite",
    "IOWriteCompleted",
    "IOTransmitFileCompleted",
    "IOPostedPackage",
    "unknown" // 9
};

/*
* Type of Jobs.
*/
enum JobType // the first parameter for CreatePackage(BYTE type, …)
{
    Job_SendText2Client,   // 0
    Job_SendFileInfo,      // 1
    Job_StartFileTransfer, // 2
    Job_AbortFileTransfer  // 3
};

/*
* This is what We put in the JobQueue
*/
struct JobItem
{
    JobType m_command;
    unsigned int m_ClientID;
    string m_Data;
};

// Sequence Priority Queue
typedef priority_queue<CIOCPBuffer*, vector<CIOCPBuffer*>, CIOCPBuffer::SequencePriority> ReorderQueue;

/*
* This struct is used to past around some information about the client.
*/
// per handle data: wrapper of SOCKET
struct ClientContext
{
    SOCKET			 m_Socket;              // The Connection socket.
    Lock             m_ContextLock;         // The lock used to update and read variabels.
    unsigned int	 m_ID;                  // Reserved for DisconnectClient if needed.
    int				 m_nNumberOfPendlingIO; // Very Important variable used with ReleaseClientContext. (Avoids Access Violation)
    //
    // Send in order variables.
    //
    unsigned int     m_SendSequenceNumber;
    unsigned int	 m_CurrentSendSequenceNumber;
    ReorderQueue     m_SendReorderQueue;    // send buffers out of order
    //
    // Read in order variables
    //
    unsigned int	 m_ReadSequenceNumber;
    unsigned int	 m_CurrentReadSequenceNumber;
    ReorderQueue	 m_ReadReorderQueue;    // read buffers out of order

    //
    // File transfer stuff.
    //
#ifdef TRANSFERFILEFUNCTIONALITY
    // CFile m_File;
    FILE* m_File;
    string m_sFileName;
    unsigned int m_iMaxFileBytes; // file size
    unsigned int m_iFileBytes;    // already sent/received
    BOOL m_bFileSendMode;         // file sender
    BOOL m_bFileReceivedMode;     // file receiver
#endif

    // Package Overlapped Buffer..
    // Used to get a complete package when we have several pending reads.
    // for assemble
    CIOCPBuffer* m_pBuffOverlappedPackage;

    // Extra info you can put whatever you want here..
    string m_sReceived;
    int m_iNumberOfReceivedMsg;
    BOOL m_bUpdate;

    // tcp默认维持2小时，KeepAlive心跳检测连接状态
    // timeout事件，超过给定时间无I/O，则断开连接    
    HANDLE hAlive;   // 活动事件
    HANDLE hTimeOut; // 等待超时事件
};

typedef vector<HANDLE>              ThreadVector;
typedef list<u_long>                IPList;        // <sin_addr.S_un.S_addr>

typedef vector<ClientContext*>      ContextVector; // context pool
typedef vector<CIOCPBuffer*>        BufferVector;  // buffer pool

typedef map<SOCKET, ClientContext*> ContextMap;
typedef list<CIOCPBuffer*>          BufferList;
typedef queue<JobItem*>             JobQueue;

class IOCPS
{
public:
    IOCPS();
    virtual ~IOCPS();

    /*
    * (1)Start and Stop
    */
    // Starts the server.
        // invokes CreateCompletionPort()\SetupLisetner()\SetupIOWorkers()\SetWorkers() to initialize server
        // invokded by Start();
    BOOL Startup();
    // Starts the server
        // invokes Startup();
        // the parameters are customized
    BOOL Start(int nPort=999, int iMaxNumConnections=1201, 
        int iMaxIOWorkers=1, int nOfWorkers=0, 
        int iMaxNumberOfFreeBuffer=100, 
        int iMaxNumberOfFreeContext=50, 
        BOOL bOrderedSend=TRUE, BOOL bOrderedRead=TRUE, 
        int iNumberOfPendlingReads=5);
    // Returns TRUE if The server/client are started.
    BOOL IsStarted();
    // Enable SYN-Flood protection in registry: DDOS
    BOOL XPNTSYNFloodProtection(int iValue=0, 
        int iTcpMaxHalfOpen=100, int iTcpMaxHalfOpenRetried=80, 
        int iTcpMaxPortsExhausted=5, int iTcpMaxConnectResponseRetransmissions=3);
    // ShutDowns The Server.
        // invokes ShutDownWorkers()\DisconnectAll()\ShutDownIOWorkers()\CloseHandle(m_hCompletionPort)\FreeClientContext()\FreeBuffers()
    void ShutDown();

    /*
    * (2)Connection management
    */
    // Gets number of connections(include incoming and outgoing?)
    int GetNumberOfConnections();
    // Finds a clien in the Client context Hashmap (NOT THREAD SAFE)..
    ClientContext* FindClient(unsigned iClient);

    // Disconnect A client.
    void DisconnectClient(unsigned int iID);
    // Disconnects all the clients.
    void DisconnectAll();

    // Connects to A IP Adress, run as client mode here
    BOOL Connect(const string& strIPAddr, int nPort);

    /*
    * (3)Post send operation
    *    IOCPS maintains a read loop internally, so only sending interface public here.
    */
    // invokes ASend(ClientContext *pContext, CIOCPBuffer *pOverlapBuff);
    BOOL ASend(int ClientId, CIOCPBuffer* pOverlapBuff);
    // Send the Data in pBuff to all the clients.
    BOOL ASendToAll(CIOCPBuffer* pBuff);

    // Functions used to post request into IOCP (simulate received packages)
    // invokes PostPackage(ClientContext* pContext, CIOCPBuffer* pOverlapBuff);
    BOOL PostPackage(int iClientId, CIOCPBuffer* pOverlapBuff);

    /*
    * (4)Worker and Job
    */
    // Sets the number of Workers can be called anytime.
        // invokes AfxBeginThread(IOCPS::WorkerThreadProc) if necessary
    BOOL SetWorkers(int nThreads);

    // Get a Job.
    JobItem* GetJob();
    // Adds a job to the queue.
    BOOL AddJob(JobItem* pJob);
    // Clear the Job from the heap.
    inline void FreeJob(JobItem* pJob);
    // Called to do some work.
    virtual inline void ProcessJob(JobItem* pJob, IOCPS* pServer);

    /*
    * (5)Sockaddr and Hostent helper
    */
    // get local ip.
    string GetLocalIP();
    // get remote ip.
    string GetRemoteIP(SOCKET socket);
    // get local host(ip:port)
    string GetLocalHost(SOCKET socket);
    // get remote host(ip:port)
    string GetRemoteHost(SOCKET socket);
    // get host
    string GetHost(SOCKET socket);

#if defined TRANSFERFILEFUNCTIONALITY
    // DO an Transmitfile.
    // invokes StartSendFile(ClientContext *pContext);
    BOOL StartSendFile(SOCKET clientSocket);
    // invokes PrepareSendFile(ClientContext *pContext, …)
    BOOL PrepareSendFile(SOCKET clientSocket, string Filename);
    // invokes DisableSendFile(ClientContext *pContext);
    BOOL DisableSendFile(SOCKET clientSocket);
    // invokes PrepareReceiveFile(ClientContext *pContext, …)
    BOOL PrepareReceiveFile(SOCKET clientSocket, LPCTSTR lpszFilename, DWORD dwFileSize);
    // invokes DisableReceiveFile(ClientContext *pContext);
    BOOL DisableReceiveFile(SOCKET clientSocket);
#endif

    // Debug Helper: work with debug version
    void TRACERT(const char* fmt, ...);                        // OutputDebugString: debug info
    void ReportError(DWORD dwError, const char* szFunName=""); // OutputDebugString: Know Error
    void ReportError(const char* szFunName="");                // OutputDebugString: GetLastError
    void ReportWSAError(const char* szFunName="");             // OutputDebugString: WSAGetLastError

    //////////////////////////////////////////////////////////////////////////
    // public data
    //////////////////////////////////////////////////////////////////////////
    // We put all the Context (Open connections) into this String2Pointer HashMap.    
    ContextMap m_ContextMap;
    Lock m_ContextMapLock; // guardian

protected:
    // Creates a new buffer or returns a buffer from the FreeBufferList and configure the buffer.
    CIOCPBuffer* AllocateBuffer(int nType);
    // deletes the buffer or just put it in the FreeBufferList to optimze performance.
    BOOL ReleaseBuffer(CIOCPBuffer* pBuff);
    // Do a Asyncorn Send. Never call this function outside of Notifyxxxx(...) functions.
    // invoked by ASend(int ClientId, CIOCPBuffer* pOverlapBuff);
    BOOL ASend(ClientContext* pContext, CIOCPBuffer* pOverlapBuff);

    // Log Helper: invovled with a text editor or file
    virtual void AppendLog(const char* fmt, ...);

    // Called when a new connection have been established..
        // invoked by OnInitialize()
    virtual void NotifyNewConnection(ClientContext* pcontext);
    // A client have Disconnected.
        // invoked by AbortiveClose()
    virtual void NotifyDisconnectedClient(ClientContext* pContext);

    // Called when a empty ClientContext structure are allocated.
        // invoked by AllocateContext()
    virtual void NotifyNewClientContext(ClientContext* pContext);
    // An ClientContext is going to be deleted insert more cleanup code if nesseary.
        // invoked by ReleaseClientContext()
    virtual void NotifyContextRelease(ClientContext* pContext);

    // A Package have arrived.
        // invoked by ProcessPackage() and OnPostedPackage()
    virtual inline void NotifyReceivedPackage(CIOCPBuffer* pOverlapBuff, int nSize, ClientContext* pContext);
    // An Write have been Completed
        // invoked by OnWriteCompleted()
    virtual inline void NotifyWriteCompleted(ClientContext* pContext, DWORD dwIoSize, CIOCPBuffer* pOverlapBuff);
    // File send/Transefer Completed.
        // invoked by AddToFile(receive mode) and OnTransmitFileCompleted(send mode)
    virtual void NotifyFileCompleted(ClientContext* pcontext);

    //////////////////////////////////////////////////////////////////////////
    // protected data
    //////////////////////////////////////////////////////////////////////////
    volatile int m_NumberOfActiveConnections;

private:
    // Listener, IOWorker, Worker.
    // listener servo
    static UINT ListenerThreadProc(LPVOID pParam);
    // i/o dispatcher(IOWorker)
    static UINT IOWorkerThreadProc(LPVOID pParam);
    // i/o handler(Worker)
    static UINT WorkerThreadProc(LPVOID pParam);

    // Starts The Connection Listener Thread.
        // invokes WSASocket()\WSACreateEvent()\WSAEventSelect()
        // invokes bind()\listen()\AfxBeginThread((IOCPS::ListenerThreadProc)
    BOOL SetupListener();
    // Starts The IOCP Workers.
        // invokes AfxBeginThread(IOCPS::IOWorkerThreadProc)
    BOOL SetupIOWorkers();
    // Closes The IO Workers
    inline void ShutDownIOWorkers();
    // get a pointer to the worker given the worker ID. (Warning DWORD->Word conversion Error ? )
    // CWinThread* GetWorker(WORD WorkerID); // see SetWorkers()
    HANDLE GetWorker(/*WORD WorkerID*/); // see SetWorkers()
    // Closes The Worker Threads
    inline void ShutDownWorkers();

    // Creates a CreateCompletionPort
    inline BOOL CreateCompletionPort();
    // Used to bin sockets to Completionport.
    inline BOOL AssociateSocketWithCompletionPort(SOCKET socket, HANDLE hCompletionPort, DWORD dwCompletionKey);
    // Makes tha last peperation for an connection so IOWORKER can start to work with it.
        // invokes AllocateContext()\AddClientContext()\AssociateSocketWithCompletionPort
        // invokes EnterIOLoop()\AllocateBuffer(IOInitialize)\PostQueuedCompletionStatus to fire up the iocp dispatcher
        // invoked by Connect() and when ListnerThreadProc's WSAAccept() return
    inline BOOL AssociateIncomingClientWithContext(SOCKET clientSocket);

    // Allocates a ClientContext and return a pointer ot it.
    inline ClientContext* AllocateContext();
    // Used to clean up the Send and receive hash map.
    // Deletes the ClientContext or just put it in the FreeClientContext list.
    inline BOOL ReleaseClientContext(ClientContext *pContext);
    // Clears the memory of the ClientContext (Also disconnects)
    inline void FreeClientContext();

    // Add a client Context to hashMap,.
    inline BOOL AddClientContext(ClientContext* mp);
    // Disconnects A client.
    inline void DisconnectClient(ClientContext* pContext, BOOL bGraceful=FALSE);
    // Aborts A socket without removing it from contextmap.
    inline void AbortiveClose(ClientContext* mp);

    // Unlocks the memory used by the overlapped IO, to avoid WSAENOBUFS problem.
    inline BOOL AZeroByteRead(ClientContext* pContext, CIOCPBuffer* pOverlapBuff);
    // Do a Asyncorn Read.
    inline BOOL ARead(ClientContext* pContext, CIOCPBuffer* pOverlapBuff=NULL);

    //  Splits a buffer into two. Used to handle halffinished received messages.
    inline CIOCPBuffer* SplitBuffer(CIOCPBuffer* pBuff, UINT nSize);
    // Adds nSize bytes to buffer and flush the other buffer.
    inline BOOL AddAndFlush(CIOCPBuffer* pFromBuff, CIOCPBuffer* pToBuff, UINT nSize);

    // clear the memory of the buffers. Should only be called when no pendling operations are in use.
    inline void FreeBuffers();
    // Release buffers.
    inline void ReleaseBufferReorderQueue(ReorderQueue* rq);

    // increase current send sequence number 
    inline void IncreaseSendSequenceNumber(ClientContext* pContext);
    // increase current read sequence number
    inline void IncreaseReadSequenceNumber(ClientContext* pContext);

    // Used to avoid inorder package
        // invokes SetSequenceNumber()
        // invoked by ASend() and PrepareSendFile()
    inline void SetSendSequenceNumber(ClientContext* pContext, CIOCPBuffer* pBuff);
    // Used to avoid inorder Read packages
        // invokes SetSequenceNumber()
        // invoked by OnRead()
    inline BOOL MakeOrderdRead(ClientContext* pContext, CIOCPBuffer* pBuff);

    // increase i/o pending number, used to avoid access violation..
    inline void ExitIOLoop(ClientContext* pContext, int type);
    // decrease i/o pending number, used to avoid access violation..
    inline void EnterIOLoop(ClientContext* pContext, int type);

    // Used to avoid inorder packages (if you are useing more than one I/O Worker Thread)
        // invoked by OnWrite() (not OnWriteCompleted()) to assign the send sequence.
    inline CIOCPBuffer* GetNextSendBuffer(ClientContext* pContext, CIOCPBuffer* pBuff=NULL);
    // Used to avoid inorder packages (if you are useing more than one I/O Worker Thread)
        // invoked by OnReadCompleted() to check if the completed recv is expected sequential.
    inline CIOCPBuffer* GetNextReadBuffer(ClientContext* pContext, CIOCPBuffer* pBuff=NULL);

    // Used by IO Workers, use zero read to avoid SYSTEM Blocking Bug.
    inline void OnInitialize(ClientContext* pContext, DWORD dwIoSize,CIOCPBuffer *pOverlapBuff=NULL);
    inline BOOL OnZeroByteRead(ClientContext* pContext, CIOCPBuffer* pOverlapBuff=NULL);
    inline void OnZeroByteReadCompleted(ClientContext* pContext, DWORD dwIoSize, CIOCPBuffer* pOverlapBuff=NULL);
    inline BOOL OnRead(ClientContext* pContext, CIOCPBuffer* pOverlapBuff=NULL);
    inline void OnReadCompleted(ClientContext* pContext, DWORD dwIoSize, CIOCPBuffer* pOverlapBuff=NULL);
    inline BOOL OnWrite(ClientContext* pContext, DWORD dwIoSize, CIOCPBuffer* pOverlapBuff);
    inline void OnWriteCompleted(ClientContext* pContext, DWORD dwIoSize, CIOCPBuffer* pOverlapBuff);

    // Functions used to post request into IOCP (simulate received packages)
    // invoked by PostPackage(int iClientId, CIOCPBuffer* pOverlapBuff);
    BOOL PostPackage(ClientContext* pContext, CIOCPBuffer* pOverlapBuff);
    inline void OnPostedPackage(ClientContext* pContext, CIOCPBuffer* pOverlapBuff);

    // Process the internal messages.
        // invoked by IOWorkerThreadProc() to dispatch i/o
    inline void ProcessIOMessage(CIOCPBuffer* pOverlapBuff, ClientContext* pContext, DWORD dwSize);
    // Process received Packages
        // invoked by OnReadCompleted() and AddToFile(file receive mode)
        // assemble(AddAndFlush()) and notify to make custom protocol analyzing(NotifyReceivedPackage())
    inline void ProcessPackage(ClientContext* pContext, DWORD dwIoSize, CIOCPBuffer* pOverlapBuff);

#if defined TRANSFERFILEFUNCTIONALITY
    // 
    inline void AddToFile(ClientContext* pContext, DWORD dwIoSize, CIOCPBuffer* pOverlapBuff);
    // DO an Transmitfile.
    inline BOOL StartSendFile(ClientContext* pContext);
    // Perpared for file send
    inline BOOL PrepareSendFile(ClientContext* pContext, LPCTSTR lpszFilename);
    // Disables file send
    inline BOOL DisableSendFile(ClientContext* pContext);
    // Perpares for file receive
    inline BOOL PrepareReceiveFile(ClientContext* pContext, LPCTSTR lpszFilename, DWORD dwFileSize);
    // Disables file receive.
    inline BOOL DisableReceiveFile(ClientContext* pContext);
    // 
    inline void OnTransmitFileCompleted(ClientContext* pContext, CIOCPBuffer* pOverlapBuff);
#endif

#ifdef SIMPLESECURITY
public:
    void OneIPPerConnection(BOOL bVal=TRUE);

protected:
    static int CALLBACK ConnectAcceptCondition(IN LPWSABUF lpCallerId,
        IN LPWSABUF lpCallerData,
        IN OUT LPQOS lpSQOS,
        IN OUT LPQOS lpGQOS,
        IN LPWSABUF lpCalleeId,
        OUT LPWSABUF lpCalleeData,
        OUT GROUP FAR *g,
        IN DWORD dwCallbackData);

    inline BOOL IsAlreadyConnected(sockaddr_in* pCaller);
    // inline void DisconnectIfBanned(SOCKET &Socket);
    inline BOOL IsInBannedList(sockaddr_in* pCaller);
    // Disconnect immediately  if the incoming IP already exist.
    // inline void DisconnectIfIPExist(SOCKET &Socket);
    inline void AddToBanList(SOCKET &Socket);

    void ClearBanList();

    Lock m_OneIPPerConnectionLock;
    IPList m_OneIPPerConnectionList;
    Lock m_BanIPLock;
    IPList m_BanIPList;

private:

#endif

    // Helper: Error Convertion
    string ErrorCode2Text(DWORD dwError, const char* szFunName="");
    // time out
    static void CALLBACK TimeOutCallback(PVOID lpParameter, BOOLEAN TimerOrWaitFired);

    //////////////////////////////////////////////////////////////////////////
    // private data
    //////////////////////////////////////////////////////////////////////////
    /*
    * (1)listening procedure
    */
    // listen socket
    SOCKET m_sockListen;
    // listen port number(-1:client mode)
    int m_nListenPort;
    // listen thread
    // CWinThread* m_pListenThread; // 不适用CWinThread
    HANDLE m_pListenThread;
    // WSAEVENT to handle accepting
    HANDLE m_hAcceptEvent;
    // Set FALSE to signal no more accepting connections..
    volatile BOOL m_bAcceptConnections;
    // Maximum Number of Connections.
    int m_iMaxNumConnections;
    // One ipPerConnection
    BOOL m_bOneIPPerConnection;

    /*
    * (2)i/o properties
    */
    // completion port to dispatch i/o completion notification
    HANDLE m_hCompletionPort;
    // Make the recvs in order
    BOOL m_bReadInOrder;
    // Make the sends in order.
    BOOL m_bSendInOrder;
    // Number of intial pendling WSARecv for read, used for performance
    int m_iNumberOfPendlingReads;

    /*
    * (3)per handle data
    */
    // Occupied context map(not list), see ContextMap m_ContextMap;

    // Creating Context and sockets are expencive therefor we have this list.
    // All the dead connections are placed in this list for reuse.
    ContextVector m_FreeContextVector; // CPtrList m_FreeContextList;
    Lock m_FreeContextVectorLock; // guardian
    int m_iMaxNumberOfFreeContext; // 0 unlimited

    /*
    * (4)per i/o data
    */
    // Occupied buffer list.. (Buffers that is currently used)
    BufferList m_BufferList;
    Lock m_BufferListLock; // guardian

    // Allocating buffers frequently will cost much and will cause fragments therefor we have this list.
    // All the processed i/o buffers are placed int this list for reuse.
    BufferVector m_FreeBufferVector;
    Lock m_FreeBufferVectorLock; // guardian
    int m_iMaxNumberOfFreeBuffer;

    /*
    * (5)worker thread pool
    */
    // <1>i/o dispatcher
    // Number of IOWorker permitted.
    int m_iMaxIOWorkers;
    // Number of IOWorkers running
    int m_nIOWorkers;
    // IO Worker Thread list.
    // CPtrList m_IOWorkerList; // containing <CWinThread*>
    ThreadVector m_IOWorkerVector;
    // <2>i/o handler
    // We save our workers here.
    // Number of Workers running intitially.
    int m_nOfWorkers;
    // <threadID, CWinThread*>
    // CMapWordToPtr m_WorkerThreadMap;
    ThreadVector m_WorkerVector;
    Lock m_WorkerVectorLock; // guadian

    /*
    * (6)job item queue
    */
    // Workqueue used with the ThreadPool.
    Lock m_JobQueueLock;
    JobQueue m_JobQueue; // containing <JobItem*>
    // Signals FALSE to signal no more Jobs.
    volatile BOOL m_bAcceptJobs;

    /*
    * (7)global properties
    */
    // Contains the result of winsock init.
    int m_iWSAInitResult;
    // The server version
    string m_sServerVersion;
    // Start status
    BOOL m_bServerStarted;
    // Set TRUE to signal system shutdown
    volatile BOOL m_bShutDown;
    
    // trace CRITICAL_SECTION
    Lock TraceLock;
    // time out
    unsigned long m_flagQueue;
    unsigned int nMaxTimeOut;
};

#endif // !defined(AFX_MYIOCPSERVER_H__4D63F25E_B852_46D7_9A42_CF060F5E544D__INCLUDED_)