// IOCPS.cpp: implementation of the IOCPS class.
//
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "IOCPS.h"

#include <io.h> // _get_osfhandle

#include <algorithm> // find
using namespace std;

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

IOCPS *iocpserver;

IOCPS::IOCPS()
{
    WSADATA wsaData;
    m_iWSAInitResult = WSAStartup(MAKEWORD(2,2), &wsaData);

    m_sServerVersion = IOCPSERVERVERSION;
    m_bServerStarted = FALSE;
    m_bShutDown = FALSE;

    m_sockListen = NULL;
    m_nListenPort = 999;
    m_pListenThread = NULL;
    m_bAcceptConnections = TRUE;
    m_iMaxNumConnections = 1201;
    m_NumberOfActiveConnections = 0;
    m_bOneIPPerConnection = FALSE;

    m_bSendInOrder = FALSE;
    m_bReadInOrder = FALSE;
    m_iNumberOfPendlingReads = 3;

    m_iMaxNumberOfFreeContext = 2;
    m_bAcceptJobs = TRUE;
    m_nOfWorkers = 2;

    // time out
    m_flagQueue = WT_EXECUTEDEFAULT;
    WT_SET_MAX_THREADPOOL_THREADS(m_flagQueue, 2);
    nMaxTimeOut = 3*60*1000; // 3分钟还未收到响应

    iocpserver = this;
}

IOCPS::~IOCPS()
{
    ShutDown();

    if (m_iWSAInitResult == NO_ERROR)
        WSACleanup();
}

//////////////////////////////////////////////////////////////////////////
// public
//////////////////////////////////////////////////////////////////////////
BOOL IOCPS::Startup()
{
    // Some special cleanup
#if defined SIMPLESECURITY
    m_OneIPPerConnectionList.clear();
    m_BanIPList.clear();
#endif

    TRACERT(m_sServerVersion.c_str());
    AppendLog(m_sServerVersion.c_str());
    TRACERT("---------------------------------");
    AppendLog("---------------------------------");
    TRACERT("Starting system.");
    AppendLog("Starting system.");

    m_NumberOfActiveConnections = 0;

    if (m_iWSAInitResult != NO_ERROR)
    {
        ReportError(m_iWSAInitResult, "WSAStartup()");
        AppendLog("Failed to initialize Winsock 2.0.");
        return FALSE;
    }
    else // WinSock初始化成功才能继续
    {
        TRACERT("Winsock 2.0 successfully loaded.");
        AppendLog("Winsock 2.0 successfully loaded.");
    }

    BOOL bRet = TRUE; // 跟踪初始化的过程
    m_bAcceptConnections = TRUE;

    if (!m_bServerStarted)
    {
        m_bShutDown = FALSE;

        /*
        *  When using multiple pending reads (eg m_iNumberOfPendlingReads>1)
        *  with multiple IOworkers (eg m_iMaxIOWorkers>1), the order of the
        *  packages are broken, because of IOCP internal design, Furthermore there is no
        *  Performance gain to have multiple pendling reads if we use more than one IO-worker.
        *  The throughput is going up because of serveral pendling reads, but is going down because of
        *  the ordering part.
        */
        if (m_iMaxIOWorkers > 1)   // we have some sort of bug somewhere..
            m_iNumberOfPendlingReads = 1; // 多个i/o dispatcher线程，则初始投递一个WSARecv操作，读完一个再投一个

        if (m_iNumberOfPendlingReads <= 0)
            m_iNumberOfPendlingReads = 1;

        TRACERT("Max number of simultaneous connection permitted: %d", m_iMaxNumConnections);
        TRACERT("Number of automatically asynchronous pending reads: %d", m_iNumberOfPendlingReads);

        // No need to make in order read or write
        if (m_iMaxIOWorkers == 1) // 一个线程(i/o dispatcher)不存在调度乱序问题
        {
            m_bReadInOrder = FALSE;
            m_bSendInOrder = FALSE;
        }

        // If we have several Pending Reads and Several IO workers. We must read in order.
        if (m_iNumberOfPendlingReads>1 && m_iMaxIOWorkers>1)
        {
            m_bReadInOrder = TRUE;
            m_bSendInOrder = TRUE;
        }

        if (m_bSendInOrder)
        {
            TRACERT("Send ordering initialized. (Decreases the performance by ~3%)");
            AppendLog("Send ordering initialized. (Decreases the performance by ~3%)");
        }

        if (m_bReadInOrder)
        {
            TRACERT("Read ordering initialized.(Decreases the performance by ~3%)");
            AppendLog("Read ordering initialized.(Decreases the performance by ~3%)");
        }

        // The map must be empty
        m_ContextMap.clear();

        // Create the CompletionPort used by IO Worker Threads.
        bRet &= CreateCompletionPort();

        if (bRet)
        {
            TRACERT("I/O completion port successfully created.");
            AppendLog("I/O completion port successfully created.");
        }

        // Config the Listner..
        if (m_nListenPort > 0)
        {
            bRet &= SetupListener();

            if (bRet)
            {
                TRACERT("Connection listener thread successfully started.");
                AppendLog("Connection listener thread successfully started.");
            }
        }

        // Setup the IOWorkers..
        bRet &= SetupIOWorkers();

        if (bRet)
        {
            TRACERT("Successfully started %d i/o dispatcher thread.", m_nIOWorkers);
        }

        // Start the logical Workers. (SetWorkes can be callen in runtime..).
        bRet &= SetWorkers(m_nOfWorkers);

        if (bRet)
        {
            TRACERT("Successfully started %d i/o handler thread.", m_nOfWorkers);
        }

        // Accept incoming Job.
        m_bAcceptJobs = TRUE;
        m_bServerStarted = TRUE;
    }

    if (bRet)
    {
        if (m_nListenPort > 0)
        {
            TRACERT("Server successfully started.");
            AppendLog("Server successfully started.");
            TRACERT("Waiting for clients on %s:%d.", GetLocalIP().c_str(), m_nListenPort);
            AppendLog("Waiting for clients on %s:%d.", GetLocalIP().c_str(), m_nListenPort);
        }
        else // client mode
        {
            TRACERT("Client successfully started.");
            AppendLog("Client successfully started.");
        }
    }

    return bRet; // the intialization result
}

/*
* Start the Client/Server.
*/
BOOL IOCPS::Start(int nPort, int iMaxNumConnections, int iMaxIOWorkers,
                  int nOfWorkers, int iMaxNumberOfFreeBuffer, int iMaxNumberOfFreeContext,
                  BOOL bOrderedSend, BOOL bOrderedRead, int iNumberOfPendlingReads)
{
    m_bShutDown = FALSE;

    m_nListenPort = nPort;
    m_iMaxNumConnections = iMaxNumConnections;
    m_iMaxIOWorkers = iMaxIOWorkers;
    m_nOfWorkers = nOfWorkers;

    m_iMaxNumberOfFreeContext = iMaxNumberOfFreeContext;
    m_iMaxNumberOfFreeBuffer = iMaxNumberOfFreeBuffer;

    m_bReadInOrder = bOrderedRead;
    m_bSendInOrder = bOrderedSend;

    m_iNumberOfPendlingReads = iNumberOfPendlingReads;

    return Startup();
}

BOOL IOCPS::IsStarted()
{
    return m_bServerStarted;
}

/*
When you are developing server application you may what to protect your server
against SYN attacks..

The SYN flooding attack protection feature of TCP detects symptoms of SYN flooding 
and responds by reducing the time server spends on connection requests that it cannot acknowledge.

Specifically, TCP shortens the required interval between SYN-ACK (connection request acknowledgements) retransmissions. 
(TCP retransmits SYN-ACKS when they are not answered.) As a result, the allotted number of retransmissions is consumed quicker 
and the unacknowledgeable connection request is discarded faster.

The SYN attack protection is obtained by setting some values in the windows registery before you
start the server. Using this windows XP and Windows NT own protection is easy.

The registry key "SynAttackProtect" causes Transmission Control Protocol (TCP) to
adjust retransmission of SYN-ACKS. When you configure this value, the connection
responses time out more quickly in the event of a SYN attack
(a type of denial of service attack).

Below you can see the default values.

0 (default) - typical protection against SYN attacks
1 - better protection against SYN attacks that uses the advanced values below.
2 (recommended) - best protection against SYN attacks.


TcpMaxHalfOpen - default value is "100"
Determines how many connections the server can maintain in the half-open (SYN-RCVD) state 
before TCP/IP initiates SYN flooding attack protection.

TcpMaxHalfOpenRetried - default value is "80"
Determines how many connections the server can maintain in the half-open (SYN-RCVD) state even after a connection request has been retransmitted. 
If the number of exceeds the value of this entry, TCP/IP initiates SYN flooding attack protection.

TcpMaxPortsExhausted - default value is "5"
Determines how many connection requests the system can refuse before TCP/IP initiates SYN flooding attack protection. 
The system must refuse all connection requests when its reserve of open connection ports runs out.

TcpMaxConnectResponseRetransmissions - default value is "3"

Determines how many times TCP retransmits an unanswered SYN-ACK (connection request acknowledgment). 
TCP retransmits acknowledgments until they are answered or until this value expires. 
This entry is designed to minimize the effect of denial-of-service attacks (also known as SYN flooding) on the server.

The Function below manipulates these values, restart of the machine is needed to, maintain the effects!
*/
BOOL IOCPS::XPNTSYNFloodProtection(int iValue, int iTcpMaxHalfOpen, int iTcpMaxHalfOpenRetried, int iTcpMaxPortsExhausted, int iTcpMaxConnectResponseRetransmissions)
{
    string sKey_PATH = "\\SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters";
    string sKey_SynAttackProtect = "SynAttackProtect";
    string sKey_TcpMaxHalfOpen = "TcpMaxHalfOpen";
    string sKey_TcpMaxHalfOpenRetried = "TcpMaxHalfOpenRetried";
    string sKey_TcpMaxPortsExhausted = "TcpMaxPortsExhausted";
    string sKey_TcpMaxConnectResponseRetransmissions ="TcpMaxConnectResponseRetransmissions";

    HKEY hKey;
    DWORD val = 0;
    LONG r = 0;
    BOOL bRet = TRUE;

    //
    // Set the sKey_SynAttackProtect
    //
    val = iValue;

    if (RegOpenKey(HKEY_LOCAL_MACHINE, (LPCTSTR)sKey_PATH.c_str(), &hKey) != ERROR_SUCCESS)
        if (RegCreateKey(HKEY_LOCAL_MACHINE, (LPCTSTR)sKey_SynAttackProtect.c_str(), &hKey) != ERROR_SUCCESS)
            return FALSE;

    r = RegSetValueEx(hKey, (LPCTSTR)sKey_SynAttackProtect.c_str(), 0, REG_DWORD, (BYTE*)&val, sizeof(val));
    RegCloseKey(hKey);
    bRet &= (r==ERROR_SUCCESS);
    //
    // Special Parameters is used.
    //
    if (iValue == 1)
    {
        //
        // Set the sKey_SynAttackProtect
        //
        val=iTcpMaxHalfOpenRetried;

        if (RegOpenKey(HKEY_LOCAL_MACHINE, (LPCTSTR)sKey_PATH.c_str(), &hKey) != ERROR_SUCCESS)
            if (RegCreateKey(HKEY_LOCAL_MACHINE, (LPCTSTR)sKey_TcpMaxHalfOpen.c_str(), &hKey) != ERROR_SUCCESS)
                return FALSE;

        r = RegSetValueEx(hKey, (LPCTSTR)sKey_TcpMaxHalfOpen.c_str(), 0, REG_DWORD, (BYTE*)&val, sizeof(val));
        RegCloseKey(hKey);
        bRet&= (r == ERROR_SUCCESS);

        //
        // Set the sKey_TcpMaxHalfOpenRetried
        //
        val = iTcpMaxHalfOpenRetried;

        if (RegOpenKey(HKEY_LOCAL_MACHINE, (LPCTSTR)sKey_PATH.c_str(), &hKey) != ERROR_SUCCESS)
            if (RegCreateKey(HKEY_LOCAL_MACHINE, (LPCTSTR)sKey_TcpMaxHalfOpenRetried.c_str(), &hKey) != ERROR_SUCCESS)
                return FALSE;

        r = RegSetValueEx(hKey, (LPCTSTR)sKey_TcpMaxHalfOpenRetried.c_str(), 0, REG_DWORD, (BYTE*)&val, sizeof(val));
        RegCloseKey(hKey);
        bRet&= (r == ERROR_SUCCESS);

        //
        // Set the sKey_TcpMaxPortsExhausted
        //
        val=iTcpMaxPortsExhausted;

        if (RegOpenKey(HKEY_LOCAL_MACHINE, (LPCTSTR)sKey_PATH.c_str(), &hKey) != ERROR_SUCCESS)
            if (RegCreateKey(HKEY_LOCAL_MACHINE, (LPCTSTR)sKey_TcpMaxPortsExhausted.c_str(), &hKey) != ERROR_SUCCESS)
                return FALSE;

        r = RegSetValueEx(hKey, (LPCTSTR)sKey_TcpMaxPortsExhausted.c_str(), 0, REG_DWORD, (BYTE*)&val, sizeof(val));
        RegCloseKey(hKey);
        bRet &= (r == ERROR_SUCCESS);


        //
        // Set sKey_TcpMaxConnectResponseRetransmissions
        //
        val = iTcpMaxConnectResponseRetransmissions;

        if (RegOpenKey(HKEY_LOCAL_MACHINE, (LPCTSTR)sKey_PATH.c_str(), &hKey) != ERROR_SUCCESS)
            if (RegCreateKey(HKEY_LOCAL_MACHINE, (LPCTSTR)sKey_TcpMaxConnectResponseRetransmissions.c_str(), &hKey) != ERROR_SUCCESS)
                return FALSE;

        r = RegSetValueEx(hKey, (LPCTSTR)sKey_TcpMaxConnectResponseRetransmissions.c_str(), 0, REG_DWORD, (BYTE*)&val, sizeof(val));
        RegCloseKey(hKey);
        bRet &= (r == ERROR_SUCCESS);
    }

    return bRet;
}

/*
* Closes the Server and frees the memory.
* We are leaking some small amount of memory (OVERLAPPEDSTRUKTUR)
*/

// Error in Disconnect.. (some how we have to wait until the Competionport is finished with the data).
// 注意关闭时的顺序和多线程的同步问题
void IOCPS::ShutDown()
{
    if (m_bServerStarted)
    {
#if defined SIMPLESECURITY
        m_OneIPPerConnectionLock.On();
        m_OneIPPerConnectionList.clear();
        m_OneIPPerConnectionLock.Off();

        m_BanIPLock.On();
        m_BanIPList.clear();
        m_BanIPLock.Off();
#endif

        TRACERT("Shutdown initialized.");
        AppendLog("Shutdown initialized.");

        m_bAcceptConnections = FALSE;

        TRACERT("Sending shutdown signal to logical worker threads.");
        AppendLog("Sending shutdown signal to logical worker threads.");
        ShutDownWorkers();

        // We Let the IOWorker Take care of the last packets and die.
        TRACERT("Disconnecting all connections.");
        AppendLog("Disconnecting all connections.");
        DisconnectAll();

        TRACERT("Sending shutdown signal to i/o worker threads.");
        AppendLog("Sending shutdown signal to i/o worker threads.");
        m_bShutDown = TRUE;
        ShutDownIOWorkers();

        TRACERT("Closing the completion port.");
        AppendLog("Closing the completion port.");
        // Close the CompletionPort and stop any more requests (Stops the Listner)
        CloseHandle(m_hCompletionPort);

        if (m_nListenPort > 0)
        {
            AppendLog("Closing listener thread.");
            WSACloseEvent(m_hAcceptEvent);
            closesocket(m_sockListen);
            m_sockListen = INVALID_SOCKET;
        }

        AppendLog("Deallocate memory of client contexts.");
        FreeClientContext();
        AppendLog("Deallocate memory of overlapped buffers.");
        FreeBuffers();

        m_bServerStarted = FALSE;
    }
}


/*
* Return The number of Connections.
*/
int IOCPS::GetNumberOfConnections()
{
    int iRet = 0;

    m_ContextMapLock.On();
    iRet = m_NumberOfActiveConnections;
    m_ContextMapLock.Off();

    return iRet;
}

/*
* Returns a Client in ContextMap
* (OBS! NOT THREAD SAFE)
* Always call this function when you have lock the ClientContext:
* m_ContextLock.On();
* // Some Code...
* pContext = FindClient(iID);
* // Some Code..
* m_ContextLock.Off();
*/
ClientContext* IOCPS::FindClient(unsigned int iClient)
{
    if (iClient <= 0)
        return NULL;

    ClientContext* pCC = NULL;

    ContextMap::iterator iter = m_ContextMap.find(iClient);
    if (iter != m_ContextMap.end())
    {
        pCC = iter->second;
    }

    return pCC;
}

/*
* Disconnects the Client.
*/
void IOCPS::DisconnectClient(ClientContext* pContext, BOOL bGraceful)
{
    if (pContext)
    {
        pContext->m_ContextLock.On();
        BOOL bDisconnect = pContext->m_Socket!=INVALID_SOCKET;
        pContext->m_ContextLock.Off();

        // If we have an active socket then close it.
        if (bDisconnect)
        {
            //
            // Remove it From m_ContextMap.
            //
            m_ContextMapLock.On();
            BOOL bRet = FALSE;

            // Remove it from the m_ContextMapLock
            ContextMap::iterator iter = m_ContextMap.find(pContext->m_Socket);
            if (iter != m_ContextMap.end())
            {
                bRet = m_ContextMap.erase(pContext->m_Socket); // just remove reference from the map, but the object itself is still active

                if (bRet)
                    m_NumberOfActiveConnections--;
            }

            m_ContextMapLock.Off();

            TRACERT("Disconnect with %s.", GetHost(pContext->m_Socket).c_str());
            AppendLog("Disconnect with %s.", GetHost(pContext->m_Socket).c_str());
            

            pContext->m_ContextLock.On();
            NotifyDisconnectedClient(pContext);
            pContext->m_ContextLock.Off();

#ifdef SIMPLESECURITY
            if (m_bOneIPPerConnection)
            {
                //  Remove the IP address from list
                sockaddr_in sockAddr;
                memset(&sockAddr, 0, sizeof(sockAddr));
                int nSockAddrLen = sizeof(sockAddr);
                int iResult = getpeername(pContext->m_Socket, (SOCKADDR*)&sockAddr, &nSockAddrLen);

                if (iResult != INVALID_SOCKET)
                {
                    m_OneIPPerConnectionLock.On();
                    m_OneIPPerConnectionList.remove(sockAddr.sin_addr.S_un.S_addr);
                    m_OneIPPerConnectionLock.Off();
                }
            }
#endif

            //
            // If we're supposed to abort the connection, set the linger value
            // on the socket to 0.
            //
            if (!bGraceful) // 不优雅，立即关闭
            {
                LINGER lingerStruct;
                lingerStruct.l_onoff = 1;
                lingerStruct.l_linger = 0;
                setsockopt(pContext->m_Socket, SOL_SOCKET, SO_LINGER,
                    (char*)&lingerStruct, sizeof(lingerStruct));
            }

            // Now close the socket handle. This will do an abortive or  graceful close as requested.
            CancelIo((HANDLE)pContext->m_Socket); // is this helpful?
            closesocket(pContext->m_Socket);
            pContext->m_Socket = INVALID_SOCKET;
        }

        TRACERT("%s is disconnected but not removed from context map.", GetHost(pContext->m_Socket).c_str());
    }
}

/*
* Just fakes that the client side have closed the connection..
* We leave everything to the IOWorkers to handle with Disconnectclient.
*/
void IOCPS::DisconnectAll()
{
    m_ContextMapLock.On();
    // First Delete all the objects.
    int numberofItems=m_ContextMap.size();

    ContextMap::iterator iter;
    for (iter=m_ContextMap.begin(); iter!=m_ContextMap.end(); ++iter)
    {
        DisconnectClient(iter->second);
    }
    m_ContextMapLock.Off();

#ifdef _DEBUG
    m_BufferListLock.On();
    int nSize = m_BufferList.size();

    if (nSize > 0)
    {
        TRACERT("The buffer list is not empty even though all users are gone.");
        TRACERT("   %d buffers are still in use state: ", nSize);

        BufferList::iterator iter;
        for (iter=m_BufferList.begin(); iter!=m_BufferList.end(); ++iter)
        {
            if (*iter)
            {
                TRACERT("   CIOCPBuffer[%d]: %s.", (*iter)->GetSequenceNumber(), IOTypeString[(*iter)->GetOperation()]);
            }
        }
    }
    else
    {
        TRACERT("The buffer list is empty.");
    }

    m_BufferListLock.Off();

#endif
}

/*
* Connects to a IP Adress as client mode
*/
BOOL IOCPS::Connect(const string& strIPAddr, int nPort)
{
    if (m_bShutDown)
        return FALSE;

    SOCKADDR_IN	SockAddr;
    SOCKET		clientSocket = INVALID_SOCKET;
    int			nRet = -1;
    int			nLen = -1;

    clientSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (clientSocket == INVALID_SOCKET)
    {        
        ReportWSAError("socket() invoked by Connect()");
        AppendLog("Create client socket failed.");

        return FALSE;
    }

    // Clear the SockAddr.
    memset(&SockAddr, 0, sizeof(SockAddr));

    SockAddr.sin_family = AF_INET;
    SockAddr.sin_addr.s_addr = inet_addr(strIPAddr.c_str());
    SockAddr.sin_port = htons(nPort);
    
    nRet = connect(clientSocket, (sockaddr*)&SockAddr, sizeof(SockAddr));

    if (nRet == SOCKET_ERROR && WSAGetLastError()!=WSAEWOULDBLOCK)
    {
        ReportWSAError("connect()");
        AppendLog("Fail to connect server %s:%d.", strIPAddr.c_str(), nPort);

        closesocket(clientSocket);
        clientSocket = INVALID_SOCKET;

        return FALSE;
    }
    else // 连接成功
    {
        TRACERT("Connect successfully to server %s:%d.", strIPAddr.c_str(), nPort);
        AppendLog("Connect successfully to server %s:%d.", strIPAddr.c_str(), nPort);

        return AssociateIncomingClientWithContext(clientSocket);
    }
}

BOOL IOCPS::ASend(int ClientId, CIOCPBuffer* pOverlapBuff)
{
    BOOL bRet = FALSE;
    m_ContextMapLock.On();
    ClientContext* pContext = FindClient(ClientId);

    if (!pContext)
    {
        m_ContextMapLock.Off();
        ReleaseBuffer(pOverlapBuff); // Takes Care of The buffer..
        return FALSE;
    }

    bRet = ASend(pContext, pOverlapBuff); // if ASend(pContext,..) Fails, ASend Takes Care of The buffer..
    m_ContextMapLock.Off();

    return bRet;
}

BOOL IOCPS::ASendToAll(CIOCPBuffer* pBuff)
{
    if (!pBuff)
        return FALSE;

    m_ContextMapLock.On();

    if (m_ContextMap.size() <= 0)
    {
        m_ContextMapLock.Off();
        ReleaseBuffer(pBuff);
        return FALSE;
    }

    BOOL bRet = TRUE;
    ClientContext* pContext = NULL;
    ContextMap::iterator iter;
    for (iter=m_ContextMap.begin(); iter!=m_ContextMap.end(); ++iter)
    {
        pContext = iter->second;
        if (pContext && pContext->m_Socket!=INVALID_SOCKET)
        {
            CIOCPBuffer* pOverlapBuff = AllocateBuffer(IOWrite);

            if (pOverlapBuff)
            {
                if (pOverlapBuff->AddData(pBuff->GetBuffer(), pBuff->GetUsed())) // make copies for nonpageable lock 
                {
                    if (!ASend(pContext, pOverlapBuff))
                    {
                        bRet &= FALSE;
                    }
                }
                else
                {
                    TRACERT("AddData() failed in ASendToAll().");

                    ReleaseBuffer(pOverlapBuff);
                    bRet &= FALSE;
                    break; // stop send
                }
            }
            else
            {
                TRACERT("AllocateBuffer(IOWrite) failed in ASendToAll().");

                bRet = FALSE;
                break;
            }
        }
    }

    m_ContextMapLock.Off();
    ReleaseBuffer(pBuff); // 发送完即可释放

    return bRet;
}

/*
* Functions are used to post an buffer into the IOCP port
* This functions can be used instead of the  function addJob(..) (thread worker Queue).
* The function post an buffer into IOCP. (simulates[构造] an received package)
* This function is necessary to split heavy computation operation into several
* parts. (automate machine)
*/
BOOL IOCPS::PostPackage(int iClientId, CIOCPBuffer* pOverlapBuff) // 分部协调完成过程
{
    BOOL bRet = FALSE;
    m_ContextMapLock.On();
    ClientContext* pContext = FindClient(iClientId);

    if (!pContext)
    {
        m_ContextMapLock.Off();
        ReleaseBuffer(pOverlapBuff);
        return FALSE;
    }

    bRet = PostPackage(pContext, pOverlapBuff); // if ASend(pContext,..) Fails, ASend Takes Care of The buffer..
    m_ContextMapLock.Off();

    return bRet;
}

/*
* Sets the number of Workers (NOT IOWorkers that deals with Send/Receive
*/
BOOL IOCPS::SetWorkers(int nThreads)
{
    int iNumberToKill = 0;
    int iNumberToStart = 0;

    m_WorkerVectorLock.On();
    int iNumberOfThreads = m_WorkerVector.size();
    m_WorkerVectorLock.Off();

    if (nThreads < iNumberOfThreads)
        iNumberToKill = iNumberOfThreads-nThreads; // kill some thread
    else
        iNumberToStart = nThreads-iNumberOfThreads; // start some thread

    // No interference while admin the threads.
    BOOL bAcceptJobs = m_bAcceptJobs;
    m_bAcceptJobs = FALSE;

    //
    // if nThreads is bigger than our current thread count, remove all excess threads
    //

    //
    // Kill some of the workers.
    //

    m_WorkerVectorLock.On();
    // POSITION pos = m_WorkerThreadMap.GetStartPosition();
    ThreadVector::iterator iter = m_WorkerVector.begin();
    while (iter!=m_WorkerVector.end() && iNumberToKill>0)
    {
        // WORD strKey;
        // CWinThread* pThread = NULL;
        // m_WorkerThreadMap.GetNextAssoc(pos, strKey,(void*&)pThread);
        HANDLE pThread = (HANDLE)(*iter);        

        if (pThread)
        {
            // HANDLE hThread = pThread->m_hThread;

            // notify the thread that it should die.
            // pThread->m_hThread = INVALID_HANDLE_VALUE;
            pThread = INVALID_HANDLE_VALUE;
            // now let the thread terminate itself

            //::GetExitCodeThread(hThread, &dwExit) && (dwExit != 0xdead)

            ::ResumeThread(pThread);

            DWORD dwExit = NULL;

            while (::GetExitCodeThread(pThread, &dwExit) && (dwExit!=0xdead))
            {
                ::Sleep(50); // give it a chance to finish
            }

            ::CloseHandle(pThread);
            iNumberToKill--;
            // m_WorkerThreadMap.RemoveKey(strKey);
            iter = m_WorkerVector.erase(iter);
            continue;
            // delete[] pThread;
        }
        ++iter;
    }

    m_WorkerVectorLock.Off();

    //
    // Start some Workers.
    //
    m_WorkerVectorLock.On();

    while (iNumberToStart > 0)
    {
        // CWinThread* pWorkerThread = AfxBeginThread(IOCPS::WorkerThreadProc, (void*)this,
        //    THREAD_PRIORITY_NORMAL, 0, CREATE_SUSPENDED);
        DWORD dwID;
        HANDLE pWorkerThread = CreateThread(NULL, NULL, (LPTHREAD_START_ROUTINE)(&IOCPS::WorkerThreadProc), 
            (LPVOID)this, CREATE_SUSPENDED, &dwID);

        // pWorkerThread->m_bAutoDelete = FALSE; // ?

        if (pWorkerThread)
        {
            // pWorkerThread->ResumeThread();
            ResumeThread(pWorkerThread);
            // m_WorkerThreadMap[(WORD)pWorkerThread->m_nThreadID] = (void*)pWorkerThread;
            m_WorkerVector.push_back(pWorkerThread);
            iNumberToStart--;
        }
        else
        {
            m_WorkerVectorLock.Off(); // add by fan
            return FALSE;
        }
    }

    m_WorkerVectorLock.Off();
    m_bAcceptJobs = bAcceptJobs;

    return TRUE;
}

/*
* Gets a Job from the queue.
*/
JobItem* IOCPS::GetJob()
{
    JobItem* pJob = NULL;

    m_JobQueueLock.On();

    if (!m_JobQueue.empty())
    {
        pJob = (JobItem*)m_JobQueue.front(); // the front of the queue(FIFO)
        m_JobQueue.pop();
    }

    m_JobQueueLock.Off();

    return pJob;
}

/*
* Adds a job to the job queue and wakes someone up to do the job.
*/
BOOL IOCPS::AddJob(JobItem* pJob)
{
    BOOL bRet = TRUE;

    if (m_bAcceptJobs && pJob)
    {
        m_JobQueueLock.On();
        m_JobQueue.push(pJob); // add tail, remove head(FIFO)
        m_JobQueueLock.Off();

        //
        // Wake someone up to do the job..
        //
        if (bRet)
        {
            m_WorkerVectorLock.On();
            // POSITION pos = m_WorkerThreadMap.GetStartPosition();
            ThreadVector::iterator iter = m_WorkerVector.begin();

            while (iter != m_WorkerVector.end())
            {
                // WORD strKey;
                // CWinThread* pThread = NULL;
                // m_WorkerThreadMap.GetNextAssoc(pos, strKey, (void*&)pThread);
                HANDLE pThread = NULL;
                pThread = (HANDLE)(*iter);

                if (pThread)
                {
                    if (ResumeThread(pThread) == 1) // Some one wake up.
                        break;
                }

                ++iter;
            }

            m_WorkerVectorLock.Off();
        }

        return bRet;
    }

    return FALSE;
}

inline void IOCPS::FreeJob(JobItem* pJob)
{
    if (pJob)
    {
        delete pJob;
        pJob = NULL;
    }
}

/*
* Virtual Function who Processes a Job. Used to do rare heavy computation or
* calls that blocks the calling thread for a while.
*/
void IOCPS::ProcessJob(JobItem* pJob, IOCPS* pServer)
{
}


/*
* Returns the local IP Adress..
*/
// TODO: fix the function
string IOCPS::GetLocalIP()
{
    /*
    hostent* thisHost;
    char* ip;
    thisHost = gethostbyname("");
    ip = inet_ntoa(*(struct in_addr*)*thisHost->h_addr_list);

    return string(ip);
    */

    string ip = "127.0.0.1"; // Loopback Address

    char szLocalHostName[256] = {0};
    if (gethostname(szLocalHostName, sizeof(szLocalHostName))) // SOCKET_ERROR
    {
        ReportWSAError("gethostname()");        
    }
    else // NO_ERROR
    {
        hostent* localHost = gethostbyname(szLocalHostName);
        if (localHost) // NO_ERROR
        {
            ip = string(inet_ntoa(*((PIN_ADDR)localHost->h_addr)));
        }
        else // SOCKET_ERROR
        {
            ReportWSAError("gethostbyname()");    
        }
    }

    return ip;
}

string IOCPS::GetRemoteIP(SOCKET socket)
{
    string remoteIP = "0.0.0.0"; // format

    sockaddr_in remoteSockAddr;
    memset(&remoteSockAddr, 0, sizeof(sockaddr_in));
    int nSockAddrLen = sizeof(remoteSockAddr);

    if (getpeername(socket, (SOCKADDR*)&remoteSockAddr, &nSockAddrLen)) // SOCKET_ERROR
    {
        ReportWSAError("getpeername()");
    }
    else // NO_ERROR
    {        
        remoteIP = string(inet_ntoa(remoteSockAddr.sin_addr));
    }

    return remoteIP;
}

string IOCPS::GetLocalHost(SOCKET socket)
{
    string localAddress = "ip:port"; // format

    sockaddr_in localSockAddr;
    memset(&localSockAddr, 0, sizeof(sockaddr_in));
    int nSockAddrLen = sizeof(localSockAddr);

    if (getsockname(socket, (SOCKADDR*)&localSockAddr, &nSockAddrLen)) // SOCKET_ERROR
    {
        ReportWSAError("getsockname()");
    }
    else // NO_ERROR
    {
        char host[128] = {0};
        sprintf(host, "%s:%d", inet_ntoa(localSockAddr.sin_addr), ntohs(localSockAddr.sin_port));
        localAddress = string(host);
    }

    return localAddress;
}

string IOCPS::GetRemoteHost(SOCKET socket)
{
    string remoteAddress = "ip:port"; // format

    sockaddr_in remoteSockAddr;
    memset(&remoteSockAddr, 0, sizeof(sockaddr_in));
    int nSockAddrLen = sizeof(remoteSockAddr);

    if (getpeername(socket, (SOCKADDR*)&remoteSockAddr, &nSockAddrLen)) // SOCKET_ERROR
    {
        ReportWSAError("getpeername()");
    }
    else // NO_ERROR
    {
        char host[128] = {0};
        sprintf(host, "%s:%d", inet_ntoa(remoteSockAddr.sin_addr), ntohs(remoteSockAddr.sin_port));
        remoteAddress = string(host);
    }

    return remoteAddress;
}

string IOCPS::GetHost(SOCKET socket)
{
    if (m_nListenPort > 0) // server
    {
        return GetRemoteHost(socket);
    }
    else // client
    {
        return GetLocalHost(socket);
    }
}

#if defined TRANSFERFILEFUNCTIONALITY
BOOL IOCPS::StartSendFile(SOCKET clientSocket)
{
    BOOL bRet = FALSE;

    m_ContextMapLock.On();
    ClientContext* pContext = NULL;
    pContext = FindClient(clientSocket);
    if (pContext)
        bRet = StartSendFile(pContext);
    m_ContextMapLock.Off();

    return bRet;
}

/*
* Perpares for Sendfile Transfer.
* Blocks all other kind of sends.
*/
BOOL IOCPS::PrepareSendFile(SOCKET clientSocket, string Filename)
{
    BOOL bRet = FALSE;
    m_ContextMapLock.On();
    ClientContext* pContext = NULL;
    pContext = FindClient(clientSocket);

    if (pContext)
        bRet = PrepareSendFile(pContext, (LPCTSTR)Filename.c_str());

    m_ContextMapLock.Off();

    return bRet;
}

BOOL IOCPS::DisableSendFile(SOCKET clientSocket)
{
    BOOL bRet = FALSE;

    m_ContextMapLock.On();
    ClientContext* pContext = NULL;
    pContext = FindClient(clientSocket);
    if (pContext)
        bRet = DisableSendFile(pContext);
    m_ContextMapLock.Off();

    return bRet;
}

BOOL IOCPS::PrepareReceiveFile(SOCKET clientSocket, LPCTSTR lpszFilename, DWORD dwFileSize)
{
    BOOL bRet = FALSE;

    m_ContextMapLock.On();
    ClientContext* pContext = NULL;
    pContext = FindClient(clientSocket);
    if (pContext)
        bRet = PrepareReceiveFile(pContext, lpszFilename, dwFileSize);
    m_ContextMapLock.Off();

    return bRet;
}

BOOL IOCPS::DisableReceiveFile(SOCKET clientSocket)
{
    BOOL bRet = FALSE;
    m_ContextMapLock.On();
    ClientContext* pContext = NULL;
    pContext = FindClient(clientSocket);
    if (pContext)
        bRet = DisableReceiveFile(pContext);
    m_ContextMapLock.Off();

    return bRet;
}
#endif

void IOCPS::TRACERT(const char* fmt, ...)
{
    char buf[512];

    va_list args;
    va_start(args, fmt);
    _vsnprintf(buf, 512, fmt, args); // >512截断，尾部没有'\0'
    va_end(args);

    strcat(buf, "\n");

#ifdef _DEBUG
    TraceLock.On();
    ::OutputDebugString(buf);
    TraceLock.Off();
#else
    // log
#endif
}

void IOCPS::ReportError(DWORD dwError, const char* szFunName/*=""*/)
{
    string msg = ErrorCode2Text(dwError, szFunName);
    msg += "\n";

#ifdef _DEBUG
    TraceLock.On();
    ::OutputDebugString(msg.c_str());
    TraceLock.Off();
#else
    // log
#endif
}

void IOCPS::ReportError(const char* szFunName/*=""*/)
{
    string msg = ErrorCode2Text(GetLastError(), szFunName);
    msg += "\n";

#ifdef _DEBUG
    TraceLock.On();
    ::OutputDebugString(msg.c_str());
    TraceLock.Off();
#else
    // log
#endif
}

void IOCPS::ReportWSAError(const char* szFunName/*=""*/)
{
    string msg = ErrorCode2Text(WSAGetLastError(), szFunName);
    msg += "\n";

#ifdef _DEBUG
    TraceLock.On();
    ::OutputDebugString(msg.c_str());
    TraceLock.Off();
#else
    // log
#endif
}

//////////////////////////////////////////////////////////////////////////
// protected
//////////////////////////////////////////////////////////////////////////
/*
* Allocates an unique buffer for nType operation.(from m_FreeBufferList if possible)
* The allocated buffer is placed in the m_BufferList.
*/
CIOCPBuffer* IOCPS::AllocateBuffer(int nType)
{
    CIOCPBuffer* pBuff = NULL;
    //
    // Try to Get a buffer from the freebuffer list.
    //
    m_FreeBufferVectorLock.On();
    if (!m_FreeBufferVector.empty())
    {
        pBuff = m_FreeBufferVector.back();
        m_FreeBufferVector.pop_back();
    }
    m_FreeBufferVectorLock.Off();

    // We have to create a new buffer.
    if (!pBuff)
    {
        pBuff = new CIOCPBuffer();
        if (!pBuff)
        {
            return NULL;
        }
    }

    if (pBuff)
    {
        pBuff->EmptyUsed();
        pBuff->SetOperation(nType);
        pBuff->SetSequenceNumber(0);
        // Add the buffer to the buffer list.
        m_BufferListLock.On();
        m_BufferList.push_front(pBuff);
        m_BufferListLock.Off();

        return pBuff;
    }

    // reuse or allocate failed
    return NULL;
}

/*
* ReleaseBuffer releases the buffer (put it into freebufferlist or just delete it).
*/
BOOL IOCPS::ReleaseBuffer(CIOCPBuffer* pBuff)
{
    if (!pBuff)
        return FALSE;

    // TODO: check if pBuff is in m_BufferList?
    // First Remove it from the BufferList.
    m_BufferListLock.On();
    m_BufferList.remove(pBuff);
    m_BufferListLock.Off();

    //
    // Add it to the FreeBufferList or delete it.
    //
    m_FreeBufferVectorLock.On();
    if (m_iMaxNumberOfFreeBuffer==0 || m_FreeBufferVector.size()<m_iMaxNumberOfFreeBuffer)
    {
        m_FreeBufferVector.push_back(pBuff);
    }
    else // over max threshold of free buffer list
    {
        // Delete the buffer.
        if (pBuff)
        {
            delete pBuff;
            pBuff = NULL;
        }
    }
    m_FreeBufferVectorLock.Off();

    return TRUE;
}

/*
* Makes An Asyncorn Send.
*/
BOOL IOCPS::ASend(ClientContext* pContext, CIOCPBuffer* pOverlapBuff)
{
    // TODO: assume the pOverlapBuff is valid

    if (m_bServerStarted && !m_bShutDown && pContext)
    {
        // We must be safe before we start doing things.
#ifdef TRANSFERFILEFUNCTIONALITY
        if (pContext->m_Socket!=INVALID_SOCKET && !pContext->m_bFileSendMode)
#else
        if (pContext->m_Socket != INVALID_SOCKET)
#endif
        {
            //
            // If we are sending in order
            //
            if (m_bSendInOrder)
                SetSendSequenceNumber(pContext, pOverlapBuff);

            //
            // Important!! Notifies that the socket and the structure
            // pContext have an Pending IO operation ant should not be deleted.
            // This is necessary to avoid Access violation.
            //
            pOverlapBuff->SetOperation(IOWrite);
            EnterIOLoop(pContext, IOWrite);
            BOOL bSuccess = PostQueuedCompletionStatus(m_hCompletionPort, pOverlapBuff->GetUsed(), (DWORD)pContext, &pOverlapBuff->m_ol);
            if (!bSuccess && GetLastError()!=ERROR_IO_PENDING)
            {
                ReportError("PostQueuedCompletionStatus(IOWrite)");
                ExitIOLoop(pContext, IOWrite);
                ReleaseBuffer(pOverlapBuff);
                DisconnectClient(pContext);
                ReleaseClientContext(pContext);

                return FALSE;
            }
            else // pending or complete at once
            {
                return TRUE;
            }            
        }
    }
    else
    {
        return FALSE;
    }
}

void IOCPS::AppendLog(const char* fmt, ...)
{
    
}

void IOCPS::NotifyNewConnection(ClientContext *pcontext)
{
    // TRACERT("Accept a connection from %s.", GetHost(pContext->m_Socket).c_str());
}

void IOCPS::NotifyDisconnectedClient(ClientContext *pContext)
{
    // TRACERT("Disconnect with %s.", GetHost(pContext->m_Socket).c_str());
}

void IOCPS::NotifyNewClientContext(ClientContext *pContext)
{
    TRACERT("Assign client context to %s.", GetHost(pContext->m_Socket).c_str());
}

void IOCPS::NotifyContextRelease(ClientContext *pContext)
{
    TRACERT("Release a client context.");
}

/*
*	An package is received..
*/
void IOCPS::NotifyReceivedPackage(CIOCPBuffer* pOverlapBuff, int nSize, ClientContext* pContext)
{
    if (pContext->m_Socket != INVALID_SOCKET)
    {
        TRACERT("Received a package with %d bytes from %s.", nSize, GetHost(pContext->m_Socket).c_str());
    }
}

/*
*	Called when a write is completed, this function is ofen used
*  for progressbars etc (e.g indicates how much is send in bytes)
*/
void IOCPS::NotifyWriteCompleted(ClientContext* pContext, DWORD dwIoSize, CIOCPBuffer* pOverlapBuff)
{
    TRACERT("Send %d bytes to %s.", dwIoSize, GetHost(pContext->m_Socket).c_str());
}

void IOCPS::NotifyFileCompleted(ClientContext* pcontext)
{
    // pcontext is locked here.
#ifdef TRANSFERFILEFUNCTIONALITY
    TRACERT("File transmission completed.(%s: %d of %d bytes)", pcontext->m_sFileName.c_str(), pcontext->m_iFileBytes, pcontext->m_iMaxFileBytes);
#endif
}

//////////////////////////////////////////////////////////////////////////
// private
//////////////////////////////////////////////////////////////////////////

UINT IOCPS::ListenerThreadProc(LPVOID pParam)
{
    IOCPS* pThis = reinterpret_cast<IOCPS*>(pParam);

    if (pThis)
    {
        while (!pThis->m_bShutDown) // check thread control valve
        {
            DWORD dwRet;
            dwRet = WSAWaitForMultipleEvents(1, &pThis->m_hAcceptEvent, FALSE, 100, FALSE);
            if (pThis->m_bShutDown)
                break;

            // poll continue on the accept event
            if (dwRet == WSA_WAIT_TIMEOUT)
                continue;

            //
            // watch for the network event(FD_ACCEPT)
            //
            WSANETWORKEVENTS events;
            int nRet = WSAEnumNetworkEvents(pThis->m_sockListen, pThis->m_hAcceptEvent, &events);

            if (nRet == SOCKET_ERROR)
            {
                pThis->ReportWSAError("WSAEnumNetworkEvents()");
                break;
            }

            if (events.lNetworkEvents & FD_ACCEPT)
            {
                if (events.iErrorCode[FD_ACCEPT_BIT]==0 && pThis->m_bAcceptConnections && !pThis->m_bShutDown)
                {
                    // SOCKADDR_IN	SockAddr;
                    SOCKET clientSocket = INVALID_SOCKET;
                    int nRet = -1;
                    int nLen = -1;
                    //
                    // accept the new socket descriptor
                    //
                    nLen = sizeof(SOCKADDR_IN);
#ifdef SIMPLESECURITY
                    clientSocket = WSAAccept(pThis->m_sockListen, NULL, &nLen, ConnectAcceptCondition, (DWORD)pThis);
#else
                    clientSocket = WSAAccept(pThis->m_sockListen, NULL, &nLen, 0, 0);
#endif

                    if (clientSocket == SOCKET_ERROR)
                    {                        
                        nRet = WSAGetLastError();
                        if (nRet != WSAEWOULDBLOCK)
                        {
                            pThis->ReportError(nRet, "WSAAccept()");
                        }
                        else
                        {
                            // TODO: should be more perfect!
                        }
                    }
                    else
                    {
                        pThis->TRACERT("Accept a connection from %s.", pThis->GetHost(clientSocket).c_str());
                        pThis->AppendLog("Accept a connection from %s.", pThis->GetHost(clientSocket).c_str());

                        pThis->AssociateIncomingClientWithContext(clientSocket);
                    }
                }
                else
                {
                    pThis->ReportWSAError("WSAEnumNetworkEvents(unknown)");
                    break;
                }
            }
        } // while
    }

    pThis->TRACERT("Connection listener died!");
    pThis->AppendLog("Connection listener died!");

    return 0xdead; // look at this interesting code
}

UINT IOCPS::IOWorkerThreadProc(LPVOID pParam)
{
    IOCPS* pThis = reinterpret_cast<IOCPS*>(pParam);

    if (pThis)
    {
        DWORD dwIoSize;
        ClientContext* lpClientContext;
        CIOCPBuffer* pOverlapBuff;
        bool bError = false;

        HANDLE hCompletionPort = pThis->m_hCompletionPort;
        LPOVERLAPPED lpOverlapped;
        // pOverlapPlus = CONTAINING_RECORD(lpOverlapped, MYOVERLAPPEDPLUS, m_ol);
        while (!bError)
        {
            lpClientContext = NULL;
            pOverlapBuff = NULL;

            // Get a completed IO request.
            BOOL bIORet = GetQueuedCompletionStatus(
                hCompletionPort,
                &dwIoSize,
                (LPDWORD)&lpClientContext,
                &lpOverlapped,
                INFINITE);

            // Simulate workload (for debugging, to find possible reordering)
            // Sleep(20);

            // If Something whent wrong..
            if (!bIORet)
            {
                DWORD dwIOError = GetLastError();
                if (dwIOError != WAIT_TIMEOUT) // It was not an Time out event we wait for ever (INFINITE)
                {
                    // TODO: should be more perfect!

                    pThis->ReportError(dwIOError, "GetQueuedCompletionStatus()");

                    // if we have a pointer & This is not an shut down..
                    // if (lpClientContext && pThis->m_bShutDown == false)
                    if (lpClientContext)
                    {
                        /*
                        * ERROR_NETNAME_DELETED Happens when the communication socket
                        * is cancelled and you have pendling WSASend/WSARead that are not finished.
                        * Then the Pendling I/O (WSASend/WSARead etc..) is cancelled and we return with
                        * ERROR_NETNAME_DELETED..
                        */
                        // if (dwIOError == ERROR_NETNAME_DELETED)

                        pThis->ExitIOLoop(lpClientContext, 9); // the operation code unknown exactly
                        pThis->DisconnectClient(lpClientContext);                            
                        pThis->ReleaseClientContext(lpClientContext); // Should we do this ?

                        // Clear the buffer if returned.
                        pOverlapBuff = NULL;

                        if (lpOverlapped)
                            pOverlapBuff = CONTAINING_RECORD(lpOverlapped, CIOCPBuffer, m_ol);

                        if (pOverlapBuff)
                            pThis->ReleaseBuffer(pOverlapBuff);

                        continue; // 非网络错误，只是某一次I/O错误
                    } // if (lpClientContext)

                    // We shall never come here
                    // anyway this was an error and we should exit the worker thread
                    bError = true;
                    pThis->AppendLog("I/O dispatcher killed because of fatal error in GetQueuedCompletionStatus().");
                    pOverlapBuff = NULL;
                    
                    if (lpOverlapped)
                        pOverlapBuff = CONTAINING_RECORD(lpOverlapped, CIOCPBuffer, m_ol);

                    if (pOverlapBuff)
                        pThis->ReleaseBuffer(pOverlapBuff);

                    continue;
                }
            } // if (!bIORet)

            if (bIORet && lpClientContext && lpOverlapped) // normal i/o completion packet
            {
                pOverlapBuff = CONTAINING_RECORD(lpOverlapped, CIOCPBuffer, m_ol);
                if (pOverlapBuff)
                    pThis->ProcessIOMessage(pOverlapBuff, lpClientContext, dwIoSize);
            }

            if (!lpClientContext && !pOverlapBuff && pThis->m_bShutDown) // see IOCPS::ShutDownIOWorkers()
            {
                pThis->TRACERT("ClientContext returned by GetQueuedCompletionStatus() is NULL.");
                bError = true;
            }

            // pThis->ReleaseBuffer(pOverlapBuff);// from previous call
        }
    }

    pThis->TRACERT("I/O dispatcher died!");
    pThis->AppendLog("I/O dispatcher died!");

    return 0xdead;
}

// TODO: 注意联系AddJob的逻辑！
UINT IOCPS::WorkerThreadProc(LPVOID pParam)
{
    IOCPS* pPoolServer = reinterpret_cast<IOCPS*>(pParam);
    // CWinThread* pThis = NULL;
    HANDLE pThis = NULL;

    if (pPoolServer)
        pThis = pPoolServer->GetWorker();

    if (pThis)
    {
        pPoolServer->TRACERT("Thread %d is alive.\r\n", ::GetCurrentThreadId());
        JobItem* pJob = NULL;
        while (pThis != INVALID_HANDLE_VALUE)
        {
            pJob = NULL;
            pJob = pPoolServer->GetJob(); // pick a job

            if (pJob) // have job
            {
                pPoolServer->ProcessJob(pJob, pPoolServer); // do the job (the outside thread of IOCPS)
                pPoolServer->FreeJob(pJob); // done free
            }
            else // no job
                ::SuspendThread(::GetCurrentThread());
        }
    }

    pPoolServer->TRACERT("I/O handler(logic worker) died!");
    pPoolServer->AppendLog("I/O handler(logic worker) died!");

    return 0xdead;
}


/*
* Setups the listner..
*/
BOOL IOCPS::SetupListener()
{
    m_sockListen = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_IP, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (m_sockListen == INVALID_SOCKET)
    {
        ReportWSAError("WSASocket()");

        return FALSE;
    }

    // Event for handling Network IO
    m_hAcceptEvent = WSACreateEvent();
    if (m_hAcceptEvent == WSA_INVALID_EVENT)
    {
        ReportWSAError("WSACreateEvent()");
        closesocket(m_sockListen);
        m_sockListen = INVALID_SOCKET;

        return FALSE;
    }

    // The listener is ONLY interested in FD_ACCEPT
    // That is when a client connects to or IP/Port
    // Request async notification
    int nRet = WSAEventSelect(m_sockListen, m_hAcceptEvent, FD_ACCEPT);
    if (nRet == SOCKET_ERROR)
    {
        ReportWSAError("WSAEventSelect()");        
        closesocket(m_sockListen);
        m_sockListen = INVALID_SOCKET;

        return FALSE;
    }

    SOCKADDR_IN	saServer;

    // Fill in the rest of the address structure
    // internet address family
    saServer.sin_family = AF_INET;
    // any local address
    saServer.sin_addr.s_addr = INADDR_ANY;
    // listen on our designated port
    saServer.sin_port = htons(m_nListenPort);

    // bind our name to the socket
    nRet = bind(m_sockListen, (LPSOCKADDR)&saServer, sizeof(struct sockaddr));
    if (nRet == SOCKET_ERROR)
    {
        ReportWSAError("bind()");
        closesocket(m_sockListen);
        m_sockListen = INVALID_SOCKET;

        return FALSE;
    }

    // Set the socket to listen
    // nRet = listen(m_socListen, nConnections);
    nRet = listen(m_sockListen, 5);
    if (nRet == SOCKET_ERROR)
    {
        ReportWSAError("listen()");
        closesocket(m_sockListen);
        m_sockListen = INVALID_SOCKET;

        return FALSE;
    }

    m_bAcceptConnections = TRUE;
    // m_pListenThread = AfxBeginThread(IOCPS::ListenerThreadProc, (void*)this, THREAD_PRIORITY_NORMAL);
    DWORD dwID;
    m_pListenThread = CreateThread(NULL, NULL, (LPTHREAD_START_ROUTINE)(&IOCPS::ListenerThreadProc), 
        (LPVOID)this, CREATE_SUSPENDED, &dwID);
    if (!m_pListenThread)
    {
        ReportError("CreateThread()");

        WSACloseEvent(m_hAcceptEvent); // WSAEINPROGRESS?
        closesocket(m_sockListen);
        m_sockListen = INVALID_SOCKET;

        return FALSE;
    }

    ResumeThread(m_pListenThread);

    return TRUE;
}

/*
* Starts the IOWorkers.
*/
BOOL IOCPS::SetupIOWorkers()
{
    // CWinThread* pWorkerThread = NULL;
    HANDLE pWorkerThread = NULL;
    DWORD dwID;

    for (int i=0; i<m_iMaxIOWorkers; i++)
    {
        // pWorkerThread = AfxBeginThread(IOCPS::IOWorkerThreadProc, (void*)this, THREAD_PRIORITY_NORMAL);
        pWorkerThread = CreateThread(NULL, NULL, (LPTHREAD_START_ROUTINE)(&IOCPS::IOWorkerThreadProc), 
            (LPVOID)this, CREATE_SUSPENDED, &dwID);
        if (pWorkerThread)
        {
            // m_IOWorkerList.AddHead((void*)pWorkerThread);
            m_IOWorkerVector.push_back(pWorkerThread);
            ResumeThread(pWorkerThread);
        }
        else
        {
            ReportError("CreateThread()");

            return FALSE;
        }
    }

    // m_nIOWorkers = m_IOWorkerList.GetCount();
    m_nIOWorkers = m_IOWorkerVector.size();

    return TRUE;
}

/*
* Shuttingdown the IOWOrkers..
*
* We put a NULL in CompletionPort and set the m_bShutDown FLAG.
* Then we wait for the IOWorkes to finish the works in the CompletionPort and exit.
*
*/
void IOCPS::ShutDownIOWorkers()
{
    DWORD dwExitCode;
    m_bShutDown = TRUE;

    // Should wait for All IOWorkers to Shutdown..
    BOOL bIOWorkersRunning = TRUE;
    // CWinThread* pThread = NULL;
    HANDLE pThread = NULL;

    while (bIOWorkersRunning) // 同步等待所有线程退出
    {
        // Send Empty Message into CompletionPort so that the threads die.
        if (bIOWorkersRunning)
            PostQueuedCompletionStatus(m_hCompletionPort, 0, (DWORD)NULL, NULL); // pay attention to the two NULL indicator

        //	Sleep(60);

        // Check if the IOWorkers are terminated..
        // POSITION pos = m_IOWorkerList.GetHeadPosition();
        ThreadVector::iterator iter;
        for (iter=m_IOWorkerVector.begin(); iter!=m_IOWorkerVector.end(); ++iter)
        {
             // pThread = (CWinThread*)m_IOWorkerList.GetNext(pos);
            pThread = (HANDLE)(*iter);

            if (pThread)
            {
                if (::GetExitCodeThread(pThread, &dwExitCode) && dwExitCode==STILL_ACTIVE)
                    bIOWorkersRunning = TRUE;
                else
                    bIOWorkersRunning = FALSE;
            }
        }
    }

    // m_IOWorkerList.RemoveAll();
    m_IOWorkerVector.clear();
}

/*
* Returns a Worker in the Worker Pool..
*/
// TODO: 
HANDLE IOCPS::GetWorker(/*WORD WorkerID*/)
{
    /*
    CWinThread* pWorker = NULL;

    m_WorkerThreadMapLock.On();
    pWorker = (CWinThread*)m_WorkerThreadMap[WorkerID];
    m_WorkerThreadMapLock.Off();

    return pWorker;
    */

    // TODO: should be more perfect
    HANDLE pWorker = NULL;
    pWorker = GetCurrentThread();
    return pWorker;
}

/*
* Closes all the logic workers and empty the job queue.
*/
void IOCPS::ShutDownWorkers()
{
    // Close The Workers.
    m_bAcceptJobs = FALSE;
    SetWorkers(0);
    m_WorkerVectorLock.On();
    // m_WorkerThreadMap.RemoveAll();
    m_WorkerVector.clear();
    m_WorkerVectorLock.Off();

    // Empty the JobQueue.
    m_JobQueueLock.On();
    while (!m_JobQueue.empty())
    {
        FreeJob(m_JobQueue.front());
        m_JobQueue.pop();
    }
    m_JobQueueLock.Off();
}

/*
* Creates the  Completion Port m_hCompletionPort used by
* IO worker Threads.
*/
BOOL IOCPS::CreateCompletionPort()
{
    SOCKET s;

    //
    // First open a temporary socket that we will use to create the
    // completion port.  In NT 3.51 it will not be necessary to specify
    // the FileHandle parameter of CreateIoCompletionPort()--it will
    // be legal to specify FileHandle as NULL.  However, for NT 3.5
    // we need an overlapped file handle.
    //

    s = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (s == INVALID_SOCKET)
    {
        ReportWSAError("socket()");

        return FALSE;
    }

    // Create the completion port that will be used by all the workers threads.
    m_hCompletionPort = CreateIoCompletionPort((HANDLE)s, NULL, 0, 0);
    if (!m_hCompletionPort)
    {
        ReportError("CreateIoCompletionPort()");

        closesocket(s);
        s = INVALID_SOCKET;

        return FALSE;
    }

    closesocket(s);
    s = INVALID_SOCKET;

    return TRUE;
}

BOOL IOCPS::AssociateSocketWithCompletionPort(SOCKET socket, HANDLE hCompletionPort, DWORD dwCompletionKey)
{
    HANDLE h = CreateIoCompletionPort((HANDLE)socket, hCompletionPort, dwCompletionKey, 0);
    return h==hCompletionPort;
}

/*
* AssociateIncomingClientWithContext
*
* This function do the Following:
* 1) Does some simpleSecutity Stuff (e.g one connection per client, etc..)
* 2) Allocates an Context for the Socket.
* 3) Configures the Socket.
* 4) Associate the Socket and the context with the completion port.
* 5) Fires an IOInitialize So the IOWORKERS Start to work on the connection.
*/
BOOL IOCPS::AssociateIncomingClientWithContext(SOCKET clientSocket)
{
    if (clientSocket == INVALID_SOCKET)
        return FALSE;

    if (m_bShutDown || !m_bAcceptConnections)
    {
        closesocket(clientSocket);
        clientSocket = INVALID_SOCKET;
        return FALSE;
    }

    //
    // Close connection if we have reached the maximum nr of connections...
    //
    m_ContextMapLock.On(); // Mus lock the m_ContextMapLock Protect (m_NumberOfActiveConnections) ??
    if (m_NumberOfActiveConnections >= m_iMaxNumConnections)
    {
        TRACERT("Connection %s rejected because of overload.", GetHost(clientSocket).c_str());
        AppendLog("Connection %s rejected because of overload.", GetHost(clientSocket).c_str());

        //
        // Disconnect.
        //
        LINGER lingerStruct;
        lingerStruct.l_onoff = 1;
        lingerStruct.l_linger = 0;
        setsockopt(clientSocket, SOL_SOCKET, SO_LINGER, (char *)&lingerStruct, sizeof(lingerStruct));

        //
        // Now close the socket handle. This will do an abortive or graceful close, as requested.
        CancelIo((HANDLE)clientSocket); // will this valid?
        closesocket(clientSocket);
        clientSocket = INVALID_SOCKET;
    }

    m_ContextMapLock.Off();

    if (clientSocket == INVALID_SOCKET) // when will ?
        return FALSE;

    // Create the Client context to be associated with the completion port
    ClientContext* pContext = AllocateContext();
    if (pContext)
    {
        pContext->m_Socket = clientSocket;
        pContext->m_ID = clientSocket; // the same as m_Socket ?

        /*
        * TCP_NODELAY	BOOL=TRUE Disables the "nagle algorithm for send coalescing" which delays
        * short packets in the hope that the application will send more data and allow
        * it to combine them into a single one to improve network efficiency.
        */
        const char chOpt = 1;
        int nErr = setsockopt(pContext->m_Socket, IPPROTO_TCP, TCP_NODELAY, &chOpt, sizeof(char));
        if (nErr == SOCKET_ERROR)
        {
            ReportWSAError("setsockopt(IPPROTO_TCP, TCP_NODELAY)");
            closesocket(clientSocket);
            clientSocket = INVALID_SOCKET;
            ReleaseClientContext(pContext);

            return FALSE;
        }

        if (AddClientContext(pContext)) // add to ContextMap
        {
            // Associate the new socket with a completion port.
            if (!AssociateSocketWithCompletionPort(clientSocket, m_hCompletionPort, (DWORD)pContext))
            {
                ReportWSAError("AssociateSocketWithCompletionPort()");
                DisconnectClient(pContext);
                ReleaseClientContext(pContext);

                return FALSE;
            }

            // Trigger first IO Completion Request
            // Otherwise the Worker thread will remain blocked waiting for GetQueuedCompletionStatus...
            // The first message that gets queued up is ClientIoInitializing - see ThreadPoolFunc and
            // IO_MESSAGE_HANDLER

            // Important!! EnterIOLoop must notify that the socket and the structure
            // pContext have an Pendling IO operation ant should not be deleted.
            // This is nessesary to avoid Access violation.
            // 

            pContext->hAlive = CreateEvent(NULL, FALSE, FALSE, NULL); // auto reset
            if (!pContext->hAlive || !RegisterWaitForSingleObject(&pContext->hTimeOut, pContext->hAlive, IOCPS::TimeOutCallback, (PVOID)pContext->m_Socket, nMaxTimeOut, m_flagQueue))
            {
                ReportError("CreateEvent() or RegisterWaitForSingleObject()");                    
                DisconnectClient(pContext);
                ReleaseClientContext(pContext);

                return FALSE;
            }

            CIOCPBuffer* pOverlapBuff = AllocateBuffer(IOInitialize); // fires up the iocp dispatcher
            if (pOverlapBuff)
            {
                EnterIOLoop(pContext, IOInitialize);
                BOOL bSuccess = PostQueuedCompletionStatus(m_hCompletionPort, 0, (DWORD)pContext, &pOverlapBuff->m_ol);

                if ((!bSuccess && GetLastError()!=ERROR_IO_PENDING))
                {
                    ReportError("PostQueuedCompletionStatus(IOInitialize)");
                    ExitIOLoop(pContext, IOInitialize);
                    ReleaseBuffer(pOverlapBuff);
                    DisconnectClient(pContext);
                    ReleaseClientContext(pContext);

                    return FALSE;
                }
                else // pending or complete at once
                {
                    return TRUE;
                }                
            }
            else
            {
                TRACERT("AllocateBuffer(IOInitialize) failed in AssociateIncomingClientWithContext().");
                DisconnectClient(pContext);
                ReleaseClientContext(pContext);

                return FALSE;
            }
        }
        else
        {
            TRACERT("AddClientContext() failed.");

            return FALSE;
        }
    }
    else
    {        
        TRACERT("AllocateContext() failed.");

        closesocket(clientSocket);
        clientSocket = INVALID_SOCKET;

        return FALSE;
    }

    return TRUE;
}

/*
* AllocateContext. Creates a context in the heap with new or reuse context
* which is in the m_FreeContextList linked list.
*/
ClientContext* IOCPS::AllocateContext()
{
    ClientContext* pContext = NULL;

    if (!m_bShutDown)
    {
        m_FreeContextVectorLock.On();
        BOOL bGetFromBuff = m_FreeContextVector.empty();
        if (bGetFromBuff)
        {
            pContext = new ClientContext();
            pContext->m_File = NULL;
        }
        else
        {
            pContext = m_FreeContextVector.back();
            m_FreeContextVector.pop_back();
        }

        m_FreeContextVectorLock.Off();
    }

    // 初始化相关字段
    pContext->m_ContextLock.On();
    pContext->m_ID = 0;
    pContext->m_Socket = INVALID_SOCKET;
    pContext->m_nNumberOfPendlingIO = 0;
    pContext->m_SendSequenceNumber = pContext->m_ReadSequenceNumber = 0;
    pContext->m_CurrentSendSequenceNumber = pContext->m_CurrentReadSequenceNumber = 0;
    pContext->m_pBuffOverlappedPackage = NULL;
    pContext->hAlive = NULL;
    pContext->hTimeOut = NULL;

    while (!pContext->m_SendReorderQueue.empty())
    {
        pContext->m_SendReorderQueue.pop();
    }

    while (!pContext->m_ReadReorderQueue.empty())
    {
        pContext->m_ReadReorderQueue.pop();
    }

#ifdef TRANSFERFILEFUNCTIONALITY
    pContext->m_sFileName = "";
    pContext->m_bFileSendMode = FALSE;
    pContext->m_bFileReceivedMode = FALSE;
    pContext->m_iMaxFileBytes = -1;
    pContext->m_iFileBytes = -1;

    if (pContext->m_File)
    {
        fclose(pContext->m_File);
        pContext->m_File = NULL;
    }
#endif

    NotifyNewClientContext(pContext);
    pContext->m_ContextLock.Off();

    return pContext;
}

/*
* Releases the Client Context. (put it into freeClientContext or delete it)
*/
inline BOOL IOCPS::ReleaseClientContext(ClientContext* pContext)
{
    BOOL bRet = FALSE;

    if (pContext)
    {
        //
        // We are removing this pContext from the penling IO port.
        //
        // int nNumberOfPendlingIO = ExitIOLoop(pContext);
        int nNumberOfPendlingIO = pContext->m_nNumberOfPendlingIO;

        // We Should not get an EnterIOLoopHere Because the client are disconnected.

#ifdef _DEBUG
        if (nNumberOfPendlingIO < 0)
        {
            TRACERT("I/O pending number is negative in ReleaseClientContext().");
        }

        // ASSERT(nNumberOfPendlingIO >= 0);
#endif

        // If no one else is using this pContext and we are the only owner. Delete it.
        if (nNumberOfPendlingIO == 0)
        {
            //
            // Remove it From m_ContextMap.
            //

            pContext->m_ContextLock.On();
            NotifyContextRelease(pContext);
            ReleaseBuffer(pContext->m_pBuffOverlappedPackage);

#ifdef TRANSFERFILEFUNCTIONALITY
            if (pContext->m_File)
            {
                fclose(pContext->m_File);
                pContext->m_File = NULL;
            }

            pContext->m_sFileName = "";
            pContext->m_bFileSendMode = FALSE;
            pContext->m_bFileReceivedMode = FALSE;
            pContext->m_iMaxFileBytes = -1;
#endif

            ReleaseBufferReorderQueue(&pContext->m_SendReorderQueue);
            ReleaseBufferReorderQueue(&pContext->m_ReadReorderQueue);

            // Added.
            pContext->m_CurrentReadSequenceNumber = 0;
            pContext->m_ReadSequenceNumber = 0;
            pContext->m_SendSequenceNumber = 0;
            pContext->m_CurrentSendSequenceNumber = 0;

            if (pContext->hAlive)
            {
                CloseHandle(pContext->hAlive);
                pContext->hAlive = NULL;
            }

            if (pContext->hTimeOut)
            {
                UnregisterWaitEx(pContext->hTimeOut, NULL);
                pContext->hTimeOut = NULL;
            }

            pContext->m_ContextLock.Off();

            // Move the Context to the free context list (if Possible).
            m_FreeContextVectorLock.On();
            if (m_iMaxNumberOfFreeContext==0 || m_FreeContextVector.size()<m_iMaxNumberOfFreeContext)
            {
                m_FreeContextVector.push_back(pContext);
                TRACERT("Put a client context into free pool.");
            }
            else  // Or just delete it.
            {
                if (pContext)
                {
                    TRACERT("Delete a client context.");
                    pContext->m_Socket = INVALID_SOCKET;
                    delete pContext;
                    pContext = NULL;
                }
            }
            m_FreeContextVectorLock.Off();

            return TRUE;
        }
    }

    return FALSE;
}

/*
* Closes all the Sockets and removes all the buffer and ClientContext.
*/
void IOCPS::FreeClientContext()
{
    m_ContextMapLock.On();

    // First Delete all the objects.
    ClientContext* pContext = NULL;
    ContextMap::iterator iterMap;
    for (iterMap=m_ContextMap.begin(); iterMap!=m_ContextMap.end(); ++iterMap)
    {
        pContext = iterMap->second;

        if (pContext)
        {
            // Socket open we have to kill it..
            if (pContext->m_Socket != INVALID_SOCKET)
            {
                LINGER lingerStruct;
                lingerStruct.l_onoff = 1;
                lingerStruct.l_linger = 0;
                setsockopt(pContext->m_Socket, SOL_SOCKET, SO_LINGER,
                    (char *)&lingerStruct, sizeof(lingerStruct));

                // Now close the socket handle.  This will do an abortive or  graceful close, as requested.
                CancelIo((HANDLE) pContext->m_Socket);
                closesocket(pContext->m_Socket);
                pContext->m_Socket = INVALID_SOCKET;
            }

            ReleaseBuffer(pContext->m_pBuffOverlappedPackage);

#ifdef TRANSFERFILEFUNCTIONALITY
            if (pContext->m_File)
            {
                fclose(pContext->m_File);
                pContext->m_File = NULL;
            }
#endif
            ReleaseBufferReorderQueue(&pContext->m_ReadReorderQueue);
            ReleaseBufferReorderQueue(&pContext->m_SendReorderQueue);

            delete pContext;
            pContext = NULL;

            TRACERT("Remove client context of %s.", GetHost(iterMap->first).c_str());
        }
    }

    // Now remove all the keys..
    m_ContextMap.clear();
    m_ContextMapLock.Off();

    //
    // Remove The stuff in FreeContext list
    //
    m_bAcceptConnections = FALSE;

    m_FreeContextVectorLock.On();
    ContextVector::iterator iterVec = m_FreeContextVector.begin();
    while (iterVec != m_FreeContextVector.end())
    {
        delete *iterVec;
        *iterVec = NULL;
        iterVec = m_FreeContextVector.erase(iterVec);
    }
    m_FreeContextVectorLock.Off();
}

/*
* Adds A client context to the Context Map.
*/
BOOL IOCPS::AddClientContext(ClientContext* mp)
{
    m_ContextMapLock.On();
    unsigned int KeyID = mp->m_Socket;
    //
    // Check if we already have a such key.
    //
    ContextMap::iterator iter = m_ContextMap.find(KeyID);
    if (iter != m_ContextMap.end())
    {
        TRACERT("Duplicate key in AddClientContext()! Disconnecting incoming client.");
        AppendLog("Duplicate key in AddClientContext()! Disconnecting incoming client.");
        AbortiveClose(mp);
        m_ContextMapLock.Off();

        return FALSE;
    }

    //
    // Add it to the Map.
    //

    // What if this fail ?
    mp->m_ID = KeyID;
    m_ContextMap.insert(ContextMap::value_type(KeyID, mp));
    m_NumberOfActiveConnections++;
    m_ContextMapLock.Off();

    return TRUE;
}

void IOCPS::DisconnectClient(unsigned int iID)
{
    m_ContextMapLock.On();

    ClientContext* pContext = FindClient(iID);
    if (!pContext)
    {
        m_ContextMapLock.Off();
        return;
    }

    DisconnectClient(pContext);
    m_ContextMapLock.Off();
}

/*
* Same as Disconnect Client but we does not try to
* remove the context from the Context Map m_ContextMap.
*/
void IOCPS::AbortiveClose(ClientContext* mp)
{
    TRACERT("Abort connection with %s.", GetHost(mp->m_Socket).c_str());
    
    NotifyDisconnectedClient(mp);

    // If we have an active  socket close it.
    if (mp->m_Socket != INVALID_SOCKET)
    {
        LINGER lingerStruct;
        lingerStruct.l_onoff = 1;
        lingerStruct.l_linger = 0;
        setsockopt(mp->m_Socket, SOL_SOCKET, SO_LINGER,
            (char*)&lingerStruct, sizeof(lingerStruct));

        //
        // Now close the socket handle.  This will do an abortive or  graceful close, as requested.
        CancelIo((HANDLE)mp->m_Socket);
        closesocket(mp->m_Socket);
        mp->m_Socket = INVALID_SOCKET;

        if (mp->hAlive)
        {
            CloseHandle(mp->hAlive);
            mp->hAlive = NULL;
        }

        if (mp->hTimeOut)
        {
            UnregisterWaitEx(mp->hTimeOut, NULL);
            mp->hTimeOut = NULL;
        }
    }

    // Move the Context to the free context list or kill it.

    m_FreeContextVectorLock.On();
    if (m_iMaxNumberOfFreeContext==0 || m_FreeContextVector.size()<m_iMaxNumberOfFreeContext)
    {
        m_FreeContextVector.push_back(mp);
    }
    else
    {
        if (mp)
        {
            delete mp;
            mp = NULL;
        }
    }

    m_FreeContextVectorLock.Off();
}

/*
* A workaround the WSAENOBUFS error problem. (For more info please see OnZeroBytesRead
*
* Unlock the memory used by the OVELAPPED structures.
*
*/
BOOL IOCPS::AZeroByteRead(ClientContext* pContext, CIOCPBuffer* pOverlapBuff)
{
    if (m_bServerStarted && !m_bShutDown && pContext)
    {
        if (pContext->m_Socket != INVALID_SOCKET)
        {
            // check the buffer first
            if (!pOverlapBuff)
            {
                pOverlapBuff = AllocateBuffer(IOZeroByteRead);
                if (!pOverlapBuff)
                {
                    TRACERT("AllocateBuffer(IOZeroByteRead) failed in AZeroByteRead().");
                    DisconnectClient(pContext);
                    ReleaseClientContext(pContext);

                    return FALSE;
                }
            }

            pOverlapBuff->SetOperation(IOZeroByteRead);
            EnterIOLoop(pContext, IOZeroByteRead);
            BOOL bSuccess = PostQueuedCompletionStatus(m_hCompletionPort, 0, (DWORD) pContext, &pOverlapBuff->m_ol);
            if ((!bSuccess && GetLastError()!=ERROR_IO_PENDING))
            {
                ReportWSAError("PostQueuedCompletionStatus() invoked by AZeroByteRead().");

                ExitIOLoop(pContext, IOZeroByteRead);
                ReleaseBuffer(pOverlapBuff);
                DisconnectClient(pContext);
                ReleaseClientContext(pContext);

                return FALSE;
            }
            else // pending or complete at once
            {
                return TRUE;
            }
        }
        else // 已经断开
        {
            TRACERT("pContext->m_Socket==INVALID_SOCKET in AZeroByteRead().");
            ReleaseBuffer(pOverlapBuff);
            ReleaseClientContext(pContext); // Take care of it.

            return FALSE;
        }
    }
    else
    {
        if (pOverlapBuff)
        {
            ReleaseBuffer(pOverlapBuff);
        }        

        return FALSE;
    }
}

/*
* Makes a asynchrony Read by posting a IORead message into completion port
* who invokes a Onread.
*
* The read is not made directly to distribute CPU power fairly between the connections.
*/
BOOL IOCPS::ARead(ClientContext* pContext, CIOCPBuffer* pOverlapBuff)
{
    if (m_bServerStarted && !m_bShutDown && pContext)
    {
        if (pContext->m_Socket != INVALID_SOCKET)
        {
            // check the buffer first
            if (!pOverlapBuff)
            {
                pOverlapBuff = AllocateBuffer(IORead);
                if (!pOverlapBuff)
                {
                    TRACERT("AllocateBuffer(IORead) failed in ARead().");
                    DisconnectClient(pContext);
                    ReleaseClientContext(pContext);

                    return FALSE;
                }                
            }

            if (pOverlapBuff)
            {
                pOverlapBuff->SetOperation(IORead);
                EnterIOLoop(pContext, IORead);
                BOOL bSuccess = PostQueuedCompletionStatus(m_hCompletionPort, 0, (DWORD) pContext, &pOverlapBuff->m_ol);
                if ((!bSuccess && GetLastError()!=ERROR_IO_PENDING))
                {
                    ReportError("PostQueuedCompletionStatus(IORead)");
                    ExitIOLoop(pContext, IORead);
                    ReleaseBuffer(pOverlapBuff);
                    DisconnectClient(pContext);
                    ReleaseClientContext(pContext);

                    return FALSE;
                }
                else // pending or complete at once
                {
                    return TRUE;
                }
            }
        }
        else // 已经断开
        {
            TRACERT("pContext->m_Socket==INVALID_SOCKET in ARead().");
            ReleaseBuffer(pOverlapBuff);
            ReleaseClientContext(pContext); // Take care of it.

            return FALSE;
        }
    }
    else
    {
        if (pOverlapBuff)
        {
            ReleaseBuffer(pOverlapBuff);
        }

        return FALSE;
    }
}

/*
* Move to CIOBUFFER ?
*/
// buf = buf2(return)+buf1(remain)
CIOCPBuffer* IOCPS::SplitBuffer(CIOCPBuffer* pBuff, UINT nSize)
{
    CIOCPBuffer* pBuff2 = NULL;
    pBuff2 = AllocateBuffer(0);

    if (!pBuff2)
        return NULL;

    pBuff2->SetSequenceNumber(pBuff->GetSequenceNumber());
    if (!pBuff2->AddData(pBuff->GetBuffer(), nSize))
    {
        delete pBuff2;
        return NULL;
    }

    if (!pBuff->Flush(nSize)) // buf1
    {
        delete pBuff2;
        return NULL;
    }

    return pBuff2;
}

/*
* Adds the nSize bytes from pFromBuff to pToBuff, and
* removes the data from pFromBuff.
*/
BOOL IOCPS::AddAndFlush(CIOCPBuffer* pFromBuff, CIOCPBuffer* pToBuff, UINT nSize)
{
    if (!pFromBuff || !pToBuff || nSize<=0)
        return FALSE;

    if (!pToBuff->AddData(pFromBuff->GetBuffer(), nSize))
    {
        return FALSE;
    }

    if (!pFromBuff->Flush(nSize))
    {
        return FALSE;
    }

    return TRUE;
}

/*
* Deletes all the buffers..
* OBS! this function should not be called if there is any pending operations.
*/
void IOCPS::FreeBuffers()
{
    // Free the buffer in the Free buffer list..
    m_FreeBufferVectorLock.On();
    BufferVector::iterator iterVec = m_FreeBufferVector.begin();
    while (iterVec != m_FreeBufferVector.end())
    {
        delete *iterVec;
        *iterVec = NULL;
        iterVec = m_FreeBufferVector.erase(iterVec);
    }
    m_FreeBufferVectorLock.Off();

    // Free the buffers in the Occupied buffer list (if any).
    m_BufferListLock.On();
    BufferList::iterator iterList = m_BufferList.begin();
    while (iterList != m_BufferList.end())
    {
        delete *iterList;
        *iterList = NULL;
        iterList = m_BufferList.erase(iterList);
    }
    m_BufferListLock.Off();
}

void IOCPS::ReleaseBufferReorderQueue(ReorderQueue* rq)
{
    while (!rq->empty())
    {
        ReleaseBuffer(rq->top());
        rq->pop();
    }
}

// Increase the Send Sequence Number
void IOCPS::IncreaseSendSequenceNumber(ClientContext* pContext)
{
    if (pContext)
    {
        pContext->m_ContextLock.On();
        // increase or reset the sequence number
        pContext->m_CurrentSendSequenceNumber = (pContext->m_CurrentSendSequenceNumber+1) % MAXIMUMSEQUENSENUMBER;
        TRACERT("Expected send sequence number increased to %d.", pContext->m_CurrentSendSequenceNumber);
        pContext->m_ContextLock.Off();
    }
}

// Increase the Read Sequence Number
void IOCPS::IncreaseReadSequenceNumber(ClientContext *pContext)
{
    if (pContext)
    {
        pContext->m_ContextLock.On();
        // increase or reset the sequence number
        pContext->m_CurrentReadSequenceNumber = (pContext->m_CurrentReadSequenceNumber+1) % MAXIMUMSEQUENSENUMBER;
        TRACERT("Expected read sequence number increased to %d.", pContext->m_CurrentReadSequenceNumber);
        pContext->m_ContextLock.Off();
    }
}

// Sets the Send Sequence number to the Buffer.
void IOCPS::SetSendSequenceNumber(ClientContext* pContext, CIOCPBuffer* pBuff)
{
    if (pContext && pBuff)
    {
        pContext->m_ContextLock.On();
        pBuff->SetSequenceNumber(pContext->m_SendSequenceNumber);
        // 序列号作回环自增
        pContext->m_SendSequenceNumber = (pContext->m_SendSequenceNumber+1) % MAXIMUMSEQUENSENUMBER;
        TRACERT("Send sequence number increased to %d.", pContext->m_SendSequenceNumber);
        pContext->m_ContextLock.Off();
    }
}

// Sets the Sequence number to a Buffer and adds the sequence buffer
BOOL IOCPS::MakeOrderdRead(ClientContext* pContext, CIOCPBuffer* pBuff)
{
    // TODO: assume all parameters are right!

    // pContext->m_ContextLock.On();
    pBuff->SetSequenceNumber(pContext->m_ReadSequenceNumber);
    DWORD dwIoSize = 0;
    ULONG ulFlags = MSG_PARTIAL;
    UINT nRetVal = WSARecv(pContext->m_Socket,
        pBuff->GetWSABuffer(),
        1,
        &dwIoSize,
        &ulFlags,
        &pBuff->m_ol,
        NULL);

    if (nRetVal==SOCKET_ERROR && WSAGetLastError()!=WSA_IO_PENDING)
    {
        // pContext->m_ContextLock.Off();
        ReportWSAError("WSARecv()");            
        ExitIOLoop(pContext, IOReadCompleted);
        ReleaseBuffer(pBuff);
        DisconnectClient(pContext); // TODO: what if there are some pendings?
        ReleaseClientContext(pContext); // TODO: what if there are some pendings?

        return FALSE;
    }
    else
    {
        // 投递成功，则序列号回环自增
        pContext->m_ReadSequenceNumber = (pContext->m_ReadSequenceNumber+1)%MAXIMUMSEQUENSENUMBER;
        TRACERT("Read sequence number increased to %d.", pContext->m_ReadSequenceNumber);
        // pContext->m_ContextLock.Off();
        return TRUE;
    }
}

/*
* Notifies that this Client Context Structure is currently in the
* IOCompetetion loop and are used by a another thread.
* This function and ExitIOLoop is used to avoid possible Access Violation
*/
void IOCPS::EnterIOLoop(ClientContext* pContext, int type)
{
    TRACERT("EnterIOLoop(%s, %s).", GetHost(pContext->m_Socket).c_str(), IOTypeString[type]);

    if (pContext)
    {
        pContext->m_ContextLock.On();
        pContext->m_nNumberOfPendlingIO++; // increase pending i/o count
        pContext->m_ContextLock.Off();
    }
}

/*
* Notifies that the ClientContext is no longer in used by thread x, and
* have been removed from the competition port. This function decreases the
* m_nNumberOfPendlingIO and returns it.
*
* if it return zero (0) then it is safe to delete the structure from the heap.
*/
void IOCPS::ExitIOLoop(ClientContext* pContext, int type)
{
    // TODO: maybe invalid socket!
    TRACERT("ExitIOLoop(%s, %s).", GetHost(pContext->m_Socket).c_str(), IOTypeString[type]);

    if (pContext)
    {
        pContext->m_ContextLock.On();
        pContext->m_nNumberOfPendlingIO--;

#ifdef _DEBUG
        if (pContext->m_nNumberOfPendlingIO < 0)
        {
            TRACERT("Pending i/o number is negative in ExitIOLoop().");
        }

#endif

        pContext->m_ContextLock.Off();
    }
}

/*
* Used to avoid inorder packaging.
* Returns The in order Buffer or NULL if not processed.
*/
CIOCPBuffer* IOCPS::GetNextSendBuffer(ClientContext* pContext, CIOCPBuffer* pBuff)
{
    // We must have a ClientContext to begin with.
    if (!pContext)
        return NULL;

    pContext->m_ContextLock.On();    
    // We have a buffer
    if (pBuff)
    {
        // Is the Buffer inorder ?
        unsigned int iBufferSequenceNumber = pBuff->GetSequenceNumber();
        if (iBufferSequenceNumber == pContext->m_CurrentSendSequenceNumber) // it's just the expected send sequence
        {
            TRACERT("Order in GetNextSendBuffer(): PacketSeq=%d, ExpectSeq=%d, NextSeq=%d.", iBufferSequenceNumber, pContext->m_CurrentSendSequenceNumber, pContext->m_SendSequenceNumber);
            // Unlock the Context Lock.
            pContext->m_ContextLock.Off();
            // return the Buffer to be processed.
            return pBuff;
        }
        else // out of order, push it to the send reorder queue
        {
            TRACERT("Disorder in GetNextSendBuffer(): PacketSeq=%d, ExpectSeq=%d, NextSeq=%d.", iBufferSequenceNumber, pContext->m_CurrentSendSequenceNumber, pContext->m_SendSequenceNumber);
            //
            // TODO: Check if we already have a such key.
            //

            //
            // Add it to the queue.
            //
            pContext->m_SendReorderQueue.push(pBuff);
            // TODO: check if storage successfully?
        }
    }

    // return the Ordered Context.
    // try to pick up a send task from the send reorder queue
    CIOCPBuffer* pBufToSend = NULL;
    if (!pContext->m_SendReorderQueue.empty() && 
        pContext->m_SendReorderQueue.top()->GetSequenceNumber()==pContext->m_CurrentSendSequenceNumber)
    {
        TRACERT("Reorder in GetNextSendBuffer(): PacketSeq=%d, ExpectSeq=%d, NextSeq=%d.", pContext->m_SendReorderQueue.top()->GetSequenceNumber(), pContext->m_CurrentSendSequenceNumber, pContext->m_SendSequenceNumber);
        pBufToSend = pContext->m_SendReorderQueue.top();
        pContext->m_SendReorderQueue.pop();
    }
 
    pContext->m_ContextLock.Off();

    return pBufToSend;
}

/*
* Used to avoid inorder packaging.
* Returns The in order Buffer or NULL if not processed.
* Same as GetReadBuffer
*/
CIOCPBuffer* IOCPS::GetNextReadBuffer(ClientContext* pContext, CIOCPBuffer* pBuff)
{
    // We must have a ClientContext to begin with.
    if (!pContext)
        return NULL;

    pContext->m_ContextLock.On();
    // We have a buffer
    if (pBuff)
    {
        // Is the Buffer inorder ?
        unsigned int iBufferSequenceNumber = pBuff->GetSequenceNumber();
        if (iBufferSequenceNumber == pContext->m_CurrentReadSequenceNumber) // it's just the expected send sequence
        {
            TRACERT("Order in GetNextReadBuffer(): PacketSeq=%d, ExpectSeq=%d, NextSeq=%d.", iBufferSequenceNumber, pContext->m_CurrentReadSequenceNumber, pContext->m_ReadSequenceNumber);
            // Unlock the Context Lock.
            pContext->m_ContextLock.Off();
            // return the Buffer to be processed.
            return pBuff;
        }
        else // out of order, push it to the read reorder queue
        {
            TRACERT("Disorder in GetNextReadBuffer(): PacketSeq=%d, ExpectSeq=%d, NextSeq=%d.", iBufferSequenceNumber, pContext->m_CurrentReadSequenceNumber, pContext->m_ReadSequenceNumber);
            //
            // TODO: Check if we already have a such key.
            //

            //
            // Add it to the queue.
            //
            pContext->m_ReadReorderQueue.push(pBuff);
            // TODO: check if storage successfully
        }
    }

    // return the Ordered Buffer.
    // try to pick up a read task from the read reorder queue
    CIOCPBuffer* pBufToRead = NULL;
    if (!pContext->m_ReadReorderQueue.empty() && 
        pContext->m_ReadReorderQueue.top()->GetSequenceNumber()==pContext->m_CurrentReadSequenceNumber)
    {
        TRACERT("Reorder in GetNextReadBuffer(): PacketSeq=%d, ExpectSeq=%d, NextSeq=%d.", pContext->m_ReadReorderQueue.top()->GetSequenceNumber(), pContext->m_CurrentReadSequenceNumber, pContext->m_ReadSequenceNumber);
        pBufToRead = pContext->m_ReadReorderQueue.top();
        pContext->m_ReadReorderQueue.pop();
    } 

    pContext->m_ContextLock.Off();

    return pBufToRead;
}

// IOInitialize→OnInitialize: dispatched to post a zero read and some read operation for the incoming client.
// this will fires the iocp dispatching procedure.
void IOCPS::OnInitialize(ClientContext* pContext, DWORD dwIoSize, CIOCPBuffer* pOverlapBuff)
{
    // Do some init here..
    // Notify new connection.
    pContext->m_ContextLock.On();
    NotifyNewConnection(pContext);
    pContext->m_ContextLock.Off();

    /*
    Operations using the IO completion port will always complete in the order that they were submitted.
    Therefore we start A number of pending read loops (R) and at least a Zero byte read to avoid the WSAENOBUFS problem.
    The number of m_iNumberOfPendlingReads should not be so big that we get the WSAENOBUFS problem.
    */

    // A ZeroByteLoop. EnterIOLoop is not needed here. Already done in previous call.
    // AZeroByteRead(pContext, pOverlapBuff);

    // m_iNumberOfPendlingReads=1 by default.
    for (int i=0; i<m_iNumberOfPendlingReads; i++)
    {
        // EnterIOLoop(pContext); // One for each Read Loop
        // ARead(pContext);

        AZeroByteRead(pContext, pOverlapBuff);
    }
}

/*
OnZeroByteRead(ClientContext *pContext) the workaround
the WSAENOBUFS error problem.
This Bug was a very difficult one.. When I stress tested this server code the
server hung after a while. I first thought that this was a memory leak problem or
deadlock problem. But after a some hours I found that it is because of the system
WSAENOBUFS error.
With every overlapped send or receive operation, it is probable that 
the data buffers submitted will be locked. When memory is locked, it
cannot be paged out of physical memory. The operating system imposes 
a limit on the amount of memory that may be locked. When this limit 
is reached, overlapped operations will fail with the WSAENOBUFS error.
If a server posts many overlapped receives on each connection, this 
limit will be reached as the number of connections grow. If a server 
anticipates handling a very high number of concurrent clients, the server
can post a single zero byte receive on each connection. Because there is 
no buffer associated with the receive operation, no memory needs to be 
locked. With this approach, the per-socket receive buffer should be left 
intact because once the zero-byte receive operation completes, the server 
can simply perform a non-blocking receive to retrieve all the data buffered 
in the socket's receive buffer. There is no more data pending when the 
non-blocking receive fails with WSAEWOULDBLOCK. This design would be for 
servers that require the maximum possible concurrent connections while 
sacrificing the data throughput on each connection.
Of course, the more you are aware of how the clients will be interacting 
with the server, the better. In the previous example, a non-blocking receive 
is performed once the zero-byte receive completes to retrieve the buffered 
data. If the server knows that clients send data in bursts, then once the 
zero-byte receive completes, it may post one or more overlapped receives 
in case the client sends a substantial amount of data
(greater than the per-socket receive buffer that is 8 KB by default).
*/
// IOZeroByteRead→OnZeroByteRead: dispatched to post a zero read operation.
// when the zero read completed, the status code is IOZeroReadCompleted
// it means there are some data buffered, 
// iocp dispatcher will invoke OnZeroByteReadCompleted next.
BOOL IOCPS::OnZeroByteRead(ClientContext* pContext, CIOCPBuffer* pOverlapBuff)
{
    if (m_bServerStarted && !m_bShutDown && pContext)
    {
        if (pContext->m_Socket != INVALID_SOCKET)
        {
            if (!pOverlapBuff)
            {
                pOverlapBuff = AllocateBuffer(IOZeroReadCompleted);
                if (!pOverlapBuff)
                {
                    TRACERT("AllocateBuffer(IOZeroReadCompleted) failed in OnZeroByteRead().");
                    DisconnectClient(pContext);
                    ReleaseClientContext(pContext);

                    return FALSE;
                }
            }

            pOverlapBuff->SetOperation(IOZeroReadCompleted);
            pOverlapBuff->SetupZeroByteRead(); // prepare a zero wsabuf
            EnterIOLoop(pContext, IOZeroReadCompleted);
            // issue a ZeroRead request
            DWORD dwIoSize = 0;
            ULONG ulFlags = MSG_PARTIAL;
            UINT nRetVal = WSARecv(pContext->m_Socket, 
                pOverlapBuff->GetWSABuffer(),
                1,
                &dwIoSize,
                &ulFlags,
                &pOverlapBuff->m_ol,
                NULL);

            if (nRetVal==SOCKET_ERROR && WSAGetLastError()!=WSA_IO_PENDING)
            {
                ReportWSAError("WSARecv(0)");
                ExitIOLoop(pContext, IOZeroReadCompleted);
                ReleaseBuffer(pOverlapBuff);
                DisconnectClient(pContext); // TODO: what if there are some pendings?
                ReleaseClientContext(pContext); // TODO: what if there are some pendings?

                return FALSE;
            }
            else // pending or complete at once
            {
                return TRUE;
            }
        }
        else // 已经断开
        {
            TRACERT("pContext->m_Socket==INVALID_SOCKET in OnZeroByteRead().");
            ReleaseBuffer(pOverlapBuff);
            ReleaseClientContext(pContext);

            return FALSE;
        }
    }
    else
    {
        if (pOverlapBuff)
        {
            ReleaseBuffer(pOverlapBuff);
        }

        return FALSE;
    }
}

// IOZeroReadCompleted→OnZeroByteReadCompleted: dispatched to deal with OnZeroByteRead completed state.
// it will invoke AZeroByteRead to post another zero read operation to make a loop.
// so there will always be a zero read pending to 
// wait for data incoming notification since initialize.
void IOCPS::OnZeroByteReadCompleted(ClientContext* pContext, DWORD dwIoSize, CIOCPBuffer* pOverlapBuff)
{
    if (pContext)
    {
        // Make a Loop.
        // AZeroByteRead(pContext, pOverlapBuff);
        ARead(pContext, pOverlapBuff);
    }
}

// IORead→OnRead: dispatched to post a read operation.
// when the read completed, the status code is IOReadCompleted
// it means incoming data are successfully copied from 
// per-socket receive buffer or tcp receive window to the user data buffer.
// iocp dispatcher will invoke OnReadCompleted next.
BOOL IOCPS::OnRead(ClientContext* pContext, CIOCPBuffer* pOverlapBuff)
{
    if (m_bServerStarted && !m_bShutDown && pContext)
    {
        if (pContext->m_Socket != INVALID_SOCKET)
        {
            if (!pOverlapBuff)
            {
                pOverlapBuff = AllocateBuffer(IOReadCompleted);
                if (!pOverlapBuff)
                {
                    TRACERT("AllocateBuffer(IOReadCompleted) failed in OnRead().");
                    DisconnectClient(pContext);
                    ReleaseClientContext(pContext);

                    return FALSE;
                }
            }

            pOverlapBuff->SetOperation(IOReadCompleted);
            pOverlapBuff->SetupRead(); // prepare a wsabuf
            EnterIOLoop(pContext, IOReadCompleted);
            // post a recv operation
            if (!m_bReadInOrder)
            {
                DWORD dwIoSize = 0;
                ULONG ulFlags = MSG_PARTIAL;
                UINT nRetVal = WSARecv(pContext->m_Socket,
                    pOverlapBuff->GetWSABuffer(),
                    1,
                    &dwIoSize,
                    &ulFlags,
                    &pOverlapBuff->m_ol,
                    NULL);

                if (nRetVal==SOCKET_ERROR && WSAGetLastError()!=WSA_IO_PENDING)
                {
                    ReportWSAError("WSARecv()");
                    ExitIOLoop(pContext, IOReadCompleted);
                    ReleaseBuffer(pOverlapBuff);
                    DisconnectClient(pContext);
                    ReleaseClientContext(pContext);

                    return FALSE;
                }
                else // pending or complete at once
                {
                    return TRUE;
                }
            }
            else
            {
                return MakeOrderdRead(pContext, pOverlapBuff);
            }
        }
        else // 已经断开
        {
            TRACERT("pContext->m_Socket==INVALID_SOCKET in OnRead().");
            ReleaseBuffer(pOverlapBuff);
            ReleaseClientContext(pContext);

            return FALSE;
        }
    }
    else
    {
        if (pOverlapBuff)
        {
            ReleaseBuffer(pOverlapBuff);
        }

        return FALSE;
    }
}

// IOReadCompleted→OnReadCompleted: dispatched to deal with OnRead completed state.
// it invokes ProcessPackage() to process the incoming package according to custom protocol.
// then it invokes ARead() to post another read operation pending on the client.
// the read procedure is managed internally like a pipeline, act as automate machine.
void IOCPS::OnReadCompleted(ClientContext* pContext, DWORD dwIoSize, CIOCPBuffer* pOverlapBuff)
{
    if (dwIoSize==0 || !pOverlapBuff)
    {
        TRACERT("Bad parameters in OnReadCompleted().");

        ReleaseBuffer(pOverlapBuff);
        DisconnectClient(pContext);
        ReleaseClientContext(pContext);

        return;
    }

    // maybe pContext->m_Socket == INVALID_SOCKET, that means the connection already disconnected.    
    if (pContext)
    {
        //
        // Process The package assuming that it have a heap of size (MINIMUMPACKAGESIZE)
        // That contains the size of the message.
        //
        /*
        * Lock the context so that no other thread enters ProcessPackage function,
        * this is necessary to process the package in order. (When IOCP is used with
        * several working IO threads the packages can be processed in wrong order (even
        * if the TCP protocol guarantees ordered stream) because of the operative
        * system process scheduling.
        * Comment and source code Added 9/10/2005
        */

        // pContext->m_ContextLock.On();
        //
        // Insure That the Packages arrive in order.
        //
        if (m_bReadInOrder)
            pOverlapBuff = GetNextReadBuffer(pContext, pOverlapBuff); // 判断pOverlapBuff是否是预期的

        SetEvent(pContext->hAlive);

        while (pOverlapBuff)
        {
            // Mark that we are Using the buffer..
            pOverlapBuff->Use(dwIoSize); // 接收的数据量，可能大于MAXIMUMPACKAGESIZE?

#ifdef TRANSFERFILEFUNCTIONALITY
            if (!pContext->m_bFileReceivedMode)
#endif
                ProcessPackage(pContext, dwIoSize, pOverlapBuff); // process the incoming data

#ifdef TRANSFERFILEFUNCTIONALITY
            else
                AddToFile(pContext, dwIoSize, pOverlapBuff); // add data fragments to file
#endif

            IncreaseReadSequenceNumber(pContext);

            // Check if we need to go out of some more pending IO, if our read is not processed in order.
            pOverlapBuff = NULL;
            if (m_bReadInOrder)
                pOverlapBuff = GetNextReadBuffer(pContext); // check inordered pending read
        } // while
        
        AZeroByteRead(pContext, pOverlapBuff); // will detect the possible disconnection
    }
}

// IOWrite→OnWrite: dispatched to post a write operation.
// when the write completed, the status code is IOWriteCompleted
// it means outgoing data are successfully copied from 
// the user data buffer to the per-socket buffer or tcp send window.
// iocp dispatcher will invoke OnWriteCompleted next.
BOOL IOCPS::OnWrite(ClientContext* pContext, DWORD dwIoSize, CIOCPBuffer* pOverlapBuff)
{
    if (m_bServerStarted && !m_bShutDown && pContext)
    {
        if (pContext->m_Socket != INVALID_SOCKET)
        {
            // pContext->m_ContextLock.On();
            if (m_bSendInOrder)
                pOverlapBuff = GetNextSendBuffer(pContext, pOverlapBuff); // 可能之前有乱序累计的发送任务

            bool bRet;
            while (pOverlapBuff/*&& pContext && pContext->m_Socket!= INVALID_SOCKET*/)
            {
                /*
                * Comments about bug in OnWrite() added 051227..
                *
                * This BUG was difficult to find. The bug was found after  6 hours of
                * extensive debugging  with several debug tools as Rational Test Suite,
                * SoftICe , VC++ DEBUG, GLOWCODE, etc.  I Found that in some rarely bizarre
                * cases  (when a client rapidly disconnect & reconnect, etc..)  we get an
                * access violation , etc. First of all we had one ReleaseClientContext to many
                * in OnWrite() which caused access violation. Second when I remove it, I found
                * that sometimes the client specific data (e.g. ClientContext) does not removed/relesed
                * from memory, even if the client is disconnected. The reason in not
                * intuitive and do not ask me how I figured it out. The problem occurs
                * when an send is not ordered (see http://www.codeproject.com/internet/iocp_server_client.asp,
                * section "3.6.2 The package reordering problem" for more information ) and
                * we call the function GetNextSendBuffer(pContext,pOverlapBuff); (above) in Onwrite(..),
                * to get the correct buffer to process. At exactly this point the remote client disconnects
                * and this leads to the bug. Now I got tired of explaining this hassling stuff, so lets
                * go to the business the fix is below..
                *
                *
                */

                pOverlapBuff->SetOperation(IOWriteCompleted);
                pOverlapBuff->SetupWrite();
                EnterIOLoop(pContext, IOWriteCompleted);
                DWORD dwSendNumBytes = 0;
                ULONG ulFlags = MSG_PARTIAL;                
                int nRetVal = WSASend(pContext->m_Socket,
                    pOverlapBuff->GetWSABuffer(),
                    1,
                    &dwSendNumBytes,
                    ulFlags,
                    &pOverlapBuff->m_ol,
                    NULL);

                if (nRetVal==SOCKET_ERROR && WSAGetLastError()!=WSA_IO_PENDING)
                {
                    // TODO: should be more perfect
                    ReportWSAError("WSASend()");
                    ExitIOLoop(pContext, IOWriteCompleted);
                    ReleaseBuffer(pOverlapBuff);
                    DisconnectClient(pContext); // maybe already disconnected because of several while loop here
                                                // for there will be some reordered pending sends, we just run over the procedure

                    IncreaseSendSequenceNumber(pContext);

                    // Check if we need to go out of some more pending IO, if our send is not processed in order.
                    pOverlapBuff = NULL;
                    if (m_bSendInOrder && !m_bShutDown)
                        pOverlapBuff = GetNextSendBuffer(pContext);

                    ReleaseClientContext(pContext); // pContext may not exist after this call

                    bRet = false;
                }
                else // pending or complete at once
                {
                    // Check if we need to go out of some more pending IO, if our send is not processed in order.
                    IncreaseSendSequenceNumber(pContext);
                    pOverlapBuff = NULL;
                    if (m_bSendInOrder && !m_bShutDown)
                        pOverlapBuff = GetNextSendBuffer(pContext);

                    bRet = true;
                }
            } // while

            return bRet;
        }
        else // 已经断开
        {
            TRACERT("pContext->m_Socket==INVALID_SOCKET in OnWrite().");
            ReleaseBuffer(pOverlapBuff);
            ReleaseClientContext(pContext);

            return FALSE;
        }
    }
    else
    {
        if (pOverlapBuff)
        {
            ReleaseBuffer(pOverlapBuff);
        }

        return FALSE;
    }
}

// IOWriteCompleted→OnWriteCompleted: dispatched to deal with OnWrite completed state.
// it simply release the send buffer, the write procedure is simulated by user manually.
void IOCPS::OnWriteCompleted(ClientContext* pContext, DWORD dwIoSize, CIOCPBuffer* pOverlapBuff)
{
    if (dwIoSize==0 || !pOverlapBuff)
    {
        TRACERT("Bad parameters in OnWriteCompleted().");

        ReleaseBuffer(pOverlapBuff);
        DisconnectClient(pContext);
        ReleaseClientContext(pContext);

        return;
    }

    if (pContext)
    {
        if (pOverlapBuff->GetUsed() != dwIoSize)
        {
            // ReleaseBuffer(pOverlapBuff);
            if (dwIoSize>0 && dwIoSize<pOverlapBuff->GetUsed())
            {
                TRACERT("The message was not sent completely.(%d of %d)", dwIoSize, pOverlapBuff->GetUsed());

                SetEvent(pContext->hAlive);                   

                if (pOverlapBuff->Flush(dwIoSize))
                {
                    ASend(pContext, pOverlapBuff);
                }
            }
        }
        else
        {
            SetEvent(pContext->hAlive);

            pContext->m_ContextLock.On();
            NotifyWriteCompleted(pContext, dwIoSize, pOverlapBuff);
            pContext->m_ContextLock.Off();
            ReleaseBuffer(pOverlapBuff);
        }
    }
}

// construct and post an iocp completion package to 
// cooperate with the dispatching procedure synchronously.
BOOL IOCPS::PostPackage(ClientContext* pContext, CIOCPBuffer* pOverlapBuff)
{
    // TODO: assume pOverlapBuff is valid
    if (m_bServerStarted && !m_bShutDown && pContext)
    {
        if (pContext->m_Socket!=INVALID_SOCKET)
        {
            pOverlapBuff->SetOperation(IOPostedPackage);
            EnterIOLoop(pContext, IOPostedPackage);
            BOOL bSuccess = PostQueuedCompletionStatus(m_hCompletionPort, pOverlapBuff->GetUsed(), (DWORD)pContext, &pOverlapBuff->m_ol);
            if ((!bSuccess && GetLastError() != ERROR_IO_PENDING))
            {
                ReportError("PostQueuedCompletionStatus(IOPostedPackage)");
                ExitIOLoop(pContext, IOPostedPackage);
                ReleaseBuffer(pOverlapBuff);
                DisconnectClient(pContext);
                ReleaseClientContext(pContext);

                return FALSE;
            }
            else // pending or complete at once
            {
                return TRUE;
            }            
        }
        else // 已经断开
        {
            TRACERT("pContext->m_Socket==INVALID_SOCKET in PostPackage().");
            ReleaseBuffer(pOverlapBuff);
            ReleaseClientContext(pContext);

            return FALSE;            
        }
    }
    else
    {
        if (pOverlapBuff)
        {
            ReleaseBuffer(pOverlapBuff);
        }   

        return FALSE;
    }
}

// IOPostedPackage→OnPostedPackage: dispatched to deal with the arrived iocp packet
void IOCPS::OnPostedPackage(ClientContext* pContext, CIOCPBuffer* pOverlapBuff)
{
    if (!pContext)
    {
        TRACERT("pContext==NULL in OnPostedPackage().");
        return;
    }

    if (pOverlapBuff)
    {
        UINT nSize = 0;
        memmove(&nSize,pOverlapBuff->GetBuffer(), MINIMUMPACKAGESIZE);
        pContext->m_ContextLock.On();
        NotifyReceivedPackage(pOverlapBuff,nSize,pContext);
        pContext->m_ContextLock.Off();
        ReleaseBuffer(pOverlapBuff);
    }

    // ReleaseClientContext(pContext);
}

// invoked by IOWorkerThreadProc to deal with i/o completion event.
void IOCPS::ProcessIOMessage(CIOCPBuffer* pOverlapBuff, ClientContext* pContext, DWORD dwSize)
{
    if (!pOverlapBuff)
        return;

    //	Sleep(rand()%50);
    switch (pOverlapBuff->GetOperation())
    {
    case IOInitialize:
        ExitIOLoop(pContext, IOInitialize);
        OnInitialize(pContext, dwSize, pOverlapBuff);
        break;
    case IOZeroByteRead: //  Workaround the the WSAENOBUFS error problem..See OnZeroByteRead.
        ExitIOLoop(pContext, IOZeroByteRead);
        OnZeroByteRead(pContext, pOverlapBuff);
        break;
    case IOZeroReadCompleted : //  Workaround the the WSAENOBUFS error problem..
        ExitIOLoop(pContext, IOZeroReadCompleted);
        OnZeroByteReadCompleted(pContext, dwSize, pOverlapBuff);
        break;
    case IORead:
        ExitIOLoop(pContext, IORead);
        OnRead(pContext, pOverlapBuff);
        break;
    case IOReadCompleted:
        ExitIOLoop(pContext,IOReadCompleted);
        OnReadCompleted(pContext, dwSize, pOverlapBuff);
        break;
    case IOWrite:
        ExitIOLoop(pContext, IOWrite);
        OnWrite(pContext, dwSize, pOverlapBuff);
        break;
    case IOWriteCompleted:
        ExitIOLoop(pContext, IOWriteCompleted);
        OnWriteCompleted(pContext, dwSize, pOverlapBuff);
        break;       

#if defined TRANSFERFILEFUNCTIONALITY
    case IOTransmitFileCompleted : //  Workaround the the WSAENOBUFS error problem..
        ExitIOLoop(pContext, IOTransmitFileCompleted);
        OnTransmitFileCompleted(pContext, pOverlapBuff);
        break;
#endif

    case IOPostedPackage:
        ExitIOLoop(pContext, IOPostedPackage);
        OnPostedPackage(pContext,pOverlapBuff);
        break;

    default: // don`t care
        ReleaseBuffer(pOverlapBuff);
        break;
    }
}

/*
* Assumes that Packages arrive with A MINIMUMPACKAGESIZE header and builds Packages that
* are noticed by the virtual function NotifyReceivedPackage
*/
// invoked by OnReadCompleted
// to deal with the received packages according to the custom protocol.
void IOCPS::ProcessPackage(ClientContext* pContext, DWORD dwIoSize, CIOCPBuffer* pOverlapBuff)
{
    //
    // We may have Several Pending reads. And therefor we have to
    // check and handle partial Messages.
    //
    //    
    // First handle partial packages.
    //
    CIOCPBuffer* pBuffPartialMessage = NULL;
    pBuffPartialMessage = pContext->m_pBuffOverlappedPackage;
    // if we had a partial message in previous message process.
    // 先处理与上次的数据单元粘包的部分
    if (pBuffPartialMessage)
    {
        // Check how big the message is...
        UINT nUsedBuffer = pBuffPartialMessage->GetUsed(); // 已接收的字节数

        // 先读足4字节长度信息
        if (nUsedBuffer < MINIMUMPACKAGESIZE) // 不足4字节
        {
            // Header to small..
            UINT nHowMuchIsNeeded = MINIMUMPACKAGESIZE-nUsedBuffer;
            // too little Data to determine the size.
            if (nHowMuchIsNeeded > pOverlapBuff->GetUsed()) // 仍不足4字节
            {
                AddAndFlush(pOverlapBuff, pBuffPartialMessage, pOverlapBuff->GetUsed());
                // Release the buffer if not used.
                if (pOverlapBuff)
                {
                    ReleaseBuffer(pOverlapBuff); // 已拷贝，释放
                }

                return; // wait for more data..
            }
            else // 读足4字节
                AddAndFlush(pOverlapBuff, pBuffPartialMessage, nHowMuchIsNeeded);
        }

        // Check how big the message is...
        nUsedBuffer = pBuffPartialMessage->GetUsed();
        if (nUsedBuffer >= MINIMUMPACKAGESIZE)
        {
            // Get The size..
            UINT nSize = 0;
            UINT nHowMuchIsNeeded = 0;
            memmove(&nSize, pBuffPartialMessage->GetBuffer(), MINIMUMPACKAGESIZE); // 4字节长度信息
            // The Overlapped Package is good. Never send packages bigger that the MAXIMUMPACKAGESIZE-MINIMUMPACKAGESIZE
            if (nSize <= (MAXIMUMPACKAGESIZE-MINIMUMPACKAGESIZE))
            {
                nHowMuchIsNeeded = nSize-(nUsedBuffer-MINIMUMPACKAGESIZE); // 还有多少字节未读

                // If we need just a little data add it..
                if (nHowMuchIsNeeded <= pOverlapBuff->GetUsed())
                {
                    // Add the remain into pBuffPartialMessage.
                    AddAndFlush(pOverlapBuff,pBuffPartialMessage,nHowMuchIsNeeded);
                    NotifyReceivedPackage(pBuffPartialMessage,nSize,pContext);
                    ReleaseBuffer(pContext->m_pBuffOverlappedPackage); // 已经读完一整包
                    pContext->m_pBuffOverlappedPackage=NULL;
                }
                else // 数据尚未到齐
                {
                    // Put everything in..
                    AddAndFlush(pOverlapBuff, pBuffPartialMessage, pOverlapBuff->GetUsed());
                }
            }
            else // 数据单元长度超过协议规定最大长度，不予处理
            {
                TRACERT("The packet size %d is over limited %d.", nSize, MAXIMUMPACKAGESIZE);
                ReleaseBuffer(pOverlapBuff);
                pOverlapBuff = NULL;
                ReleaseBuffer(pContext->m_pBuffOverlappedPackage);
                pContext->m_pBuffOverlappedPackage = NULL;

#ifdef SIMPLESECURITY
                AddToBanList(pContext->m_Socket);
#endif
                DisconnectClient(pContext);
                return;
            }
        }
    }

    // 处理完粘包或无粘包
    //
    // Process the incoming byte stream in pOverlapBuff
    //
    bool done;
    do {
        UINT nUsedBuffer = pOverlapBuff->GetUsed(); // 如果经上面的处理，还剩多少数据
        done = true;

        if (nUsedBuffer >= MINIMUMPACKAGESIZE)
        {
            UINT nSize = 0;
            memmove(&nSize, pOverlapBuff->GetBuffer(), MINIMUMPACKAGESIZE);

            // We Have a full Package..
            if (nSize == nUsedBuffer-MINIMUMPACKAGESIZE) // pOverlapBuff刚好一包
            {
                NotifyReceivedPackage(pOverlapBuff, nSize, pContext);
                pOverlapBuff->EmptyUsed();
                ReleaseBuffer(pOverlapBuff); // instead of the end?
                done = true;
            }
            else if (nSize < nUsedBuffer-MINIMUMPACKAGESIZE) // pOverlapBuff超过一包
            {
                // We have more data
                CIOCPBuffer *pBuff = SplitBuffer(pOverlapBuff, nSize+MINIMUMPACKAGESIZE);
                NotifyReceivedPackage(pBuff,nSize,pContext);
                ReleaseBuffer(pBuff);
                // loop again, we may have another complete message in there...
                done = false; // 继续循环
            }
            else if (nSize<MAXIMUMPACKAGESIZE && nSize>nUsedBuffer-MINIMUMPACKAGESIZE) // pOverlapBuff不足一包
            {
                //
                // The package is overlapped between this byte chunk stream and the next.
                //
                 TRACERT("New partial buffer in ProcessPackage().");
                pContext->m_pBuffOverlappedPackage = pOverlapBuff; // 后续需重组,解决粘包
                pOverlapBuff = NULL;               
                done = true;
            }
            else if (nSize > MAXIMUMPACKAGESIZE) // 数据单元长度超过协议规定最大长度，不予处理
            {
                TRACERT("The packet size %d is over limited %d.", nSize, MAXIMUMPACKAGESIZE);

#ifdef SIMPLESECURITY
                AddToBanList(pContext->m_Socket);
#endif
                ReleaseBuffer(pOverlapBuff);
                pOverlapBuff = NULL;
                DisconnectClient(pContext);
                break; // 异常
            }
        }
        else if (nUsedBuffer > 0) // 不足四字节
        {
            //  Header  too small.
            // nUsedBuffer < MINIMUMPACKAGESIZE
            // Add it to to the package overlapped buffer.
            // Let the remain be handled later.
            pContext->m_pBuffOverlappedPackage = pOverlapBuff; // 后续需重组,解决粘包
            pOverlapBuff = NULL;
            done = true;
        }
    } while (!done);

    // Release the buffer if not used.
    /*
    if (pOverlapBuff)
    {
    ReleaseBuffer(pOverlapBuff);
    }
    */
}


#if defined TRANSFERFILEFUNCTIONALITY

/*
* AddToFile adds the received bytes to the file.
*/
void IOCPS::AddToFile(ClientContext* pContext, DWORD dwIoSize, CIOCPBuffer* pOverlapBuff)
{   
    if (pContext->m_File)
    {
        TRACERT("Received %d bytes of file: %s.", dwIoSize, pContext->m_sFileName.c_str());

        pContext->m_ContextLock.On();
        int iBytesLeft = (int)pContext->m_iMaxFileBytes-pContext->m_iFileBytes;
        // We have two cases.
        // The buffer contains only data to be written to the buffer.
        if (dwIoSize <= iBytesLeft)
        {            
            // pContext->m_File.Write(pOverlapBuff->GetBuffer(), dwIoSize);
            fwrite(pOverlapBuff->GetBuffer(), 1, dwIoSize, pContext->m_File);
            pContext->m_iFileBytes += dwIoSize;
            // We are finished.
            if (pContext->m_iFileBytes == pContext->m_iMaxFileBytes)
            {
                NotifyFileCompleted(pContext);
                // pContext->m_File.Close();
                fclose(pContext->m_File);
                pContext->m_File = NULL;
                pContext->m_bFileReceivedMode = FALSE;
            }

            ReleaseBuffer(pOverlapBuff);
        }
        else // 文件尾部粘连了其他的数据包
        {
            /*
            We have overlapped filedata and package data
            [..filedata|Pkg..pkg]
            */
            CIOCPBuffer* pBuffFileRemain = AllocateBuffer(0);
            if (pBuffFileRemain)
            {
                // pOverlapBuff->DUMP();
                // Add the rest to the buffer.
                AddAndFlush(pOverlapBuff, pBuffFileRemain, iBytesLeft);
                TRACERT("Buffer after pBuffFileRemain in AddToFile().");
                // pOverlapBuff->DUMP();
                // Write it.
                // pContext->m_File.Write(pBuffFileRemain->GetBuffer(), iBytesLeft);
                fwrite(pBuffFileRemain->GetBuffer(), 1, iBytesLeft, pContext->m_File);
                pContext->m_iFileBytes += iBytesLeft;
                ReleaseBuffer(pBuffFileRemain);
                // We are finished.
                if (pContext->m_iFileBytes == pContext->m_iMaxFileBytes)
                {
                    NotifyFileCompleted(pContext);
                    // pContext->m_File.Close();
                    fclose(pContext->m_File);
                    pContext->m_File = NULL;
                    pContext->m_bFileReceivedMode = FALSE;
                }

                // Let the remain to be processed.
                ProcessPackage(pContext, dwIoSize-iBytesLeft, pOverlapBuff);
            }
            else
            {
                TRACERT("AllocateBuffer(0) failed in AddToFile().");
                AppendLog("AllocateBuffer(0) failed in AddToFile().");
                pContext->m_ContextLock.Off();
                ReleaseBuffer(pOverlapBuff);
                DisconnectClient(pContext);
                ReleaseClientContext(pContext);

                return;
            }
        }

        pContext->m_ContextLock.Off();
    }
}

BOOL IOCPS::StartSendFile(ClientContext* pContext)
{
    if (pContext && pContext->m_Socket!=INVALID_SOCKET)
    {
        pContext->m_ContextLock.On();
        pContext->m_bFileSendMode = TRUE;
        pContext->m_ContextLock.Off();
        CIOCPBuffer* pOverlapBuff = AllocateBuffer(IOWrite);       

        if (pOverlapBuff)
        {
            EnterIOLoop(pContext, IOTransmitFileCompleted);
            pOverlapBuff->SetOperation(IOTransmitFileCompleted);            
            TRACERT("TransmitFile() started in StartSendFile().");
            int nRetVal = TransmitFile(pContext->m_Socket,
                (HANDLE)_get_osfhandle(_fileno(pContext->m_File)), // (HANDLE)pContext->m_File,
                (DWORD)pContext->m_iMaxFileBytes,
                0,
                &pOverlapBuff->m_ol,
                NULL,
                0);
            // TP_USE_KERNEL_APC);

            if (nRetVal==SOCKET_ERROR && WSAGetLastError()!=WSA_IO_PENDING)
            {
                ReportWSAError("TransmitFile() invoked by StartSendFile()");
                ExitIOLoop(pContext, IOTransmitFileCompleted); // add 
                ReleaseBuffer(pOverlapBuff);
                DisconnectClient(pContext);
                ReleaseClientContext(pContext); // Later Implementation

                return FALSE;
            }
            else // pending or complete at once // 文件很大，可能多次发送，接受方多次AddToFile重组文件
            {
                TRACERT("Start to transmit file: %s.", pContext->m_sFileName.c_str());

                return TRUE;
            }
        }
        else
        {
            TRACERT("AllocateBuffer(IOWrite) failed in StartSendFile().");
            DisconnectClient(pContext); // TODO:
            ReleaseClientContext(pContext);

            return FALSE;
        }
    }

    return FALSE;
}

/*
* Perpares the for Filetransfer.
*
* No other type of sends must be made when transfering files.
*
* 1) The Function opens the specefied file.
* 2) Sends a package (se below) that contains information about the file to the Client.
* Se below. [sizeheader 4b|type of package 1b|Size of file 4b|.. filename..].
* 3) The actual transfer begins when the client sends a "start transfer package".
*
*/
BOOL IOCPS::PrepareSendFile(ClientContext* pContext, LPCTSTR lpszFilename)
{
    if (pContext)
    {
        // Already in send mode
        if (pContext->m_bFileSendMode)
            return FALSE;

        //
        // Open the file for write..
        //
        pContext->m_ContextLock.On();
        pContext->m_bFileSendMode = TRUE;

        // close file if it's already open
        if (pContext->m_File)
        {
            fclose(pContext->m_File);
            pContext->m_File = NULL;
        }

        // open source file
        pContext->m_File = fopen((const char*)lpszFilename, "rb");
        //if (!pContext->m_File.Open(lpszFilename, CFile::modeRead | CFile::typeBinary | CFile::osSequentialScan))
        // if (!pContext->m_File.Open(lpszFilename, CFile::modeRead|CFile::typeBinary))
        if (!pContext->m_File)
        {
            pContext->m_ContextLock.Off();
            return FALSE;
        }        

        fseek(pContext->m_File, 0, SEEK_END);
        int nFileLen = ftell(pContext->m_File);
        fseek(pContext->m_File, 0, SEEK_SET);

        pContext->m_sFileName = string(lpszFilename);
        pContext->m_iMaxFileBytes=(unsigned int)nFileLen;
        pContext->m_iFileBytes = 0;
        pContext->m_ContextLock.Off();

        TRACERT("Prepare to send file: %s.", lpszFilename);

        //
        // Send Filepakage info.
        //
        UINT iFileSize = 0;

        // sFileName = pContext->m_File.GetFileName();
        iFileSize = pContext->m_iMaxFileBytes;
        CIOCPBuffer* pOverlapBuff = AllocateBuffer(IOWrite);

        if (pOverlapBuff)
        {
            if (pOverlapBuff->CreatePackage(Job_SendFileInfo, iFileSize, pContext->m_sFileName))
            {
                //
                // If we are sending in order
                //
                if (m_bSendInOrder)
                    SetSendSequenceNumber(pContext, pOverlapBuff);

                //
                // Important!! Notifies Pending the socket and the structure
                // pContext have an Pending IO operation ant should not be deleted.
                // This is necessary to avoid Access violation.
                //
                EnterIOLoop(pContext, IOWrite);
                BOOL bSuccess = PostQueuedCompletionStatus(m_hCompletionPort, pOverlapBuff->GetUsed(), (DWORD)pContext, &pOverlapBuff->m_ol);

                if ((!bSuccess && GetLastError() != ERROR_IO_PENDING))
                {
                    ReportError("PostQueuedCompletionStatus(IOWrite) invoked by PrepareSendFile()");

                    ExitIOLoop(pContext, IOWrite);
                    ReleaseBuffer(pOverlapBuff);
                    DisconnectClient(pContext);
                    ReleaseClientContext(pContext);

                    return FALSE;
                }

                return TRUE;
            }
            else
            {
                TRACERT("CreatePackage(Job_SendFileInfo, iFileSize, sFileName) failed in PrepareSendFile().");
                ReleaseBuffer(pOverlapBuff);

                return FALSE;
            }
        }
        else
        {
            TRACERT("AllocateBuffer(IOWrite) failed in PrepareSendFile().");
            DisconnectClient(pContext);
            ReleaseClientContext(pContext); // TODO:

            return FALSE;
        }

        return TRUE;
    }

    return FALSE;
}

BOOL IOCPS::DisableSendFile(ClientContext* pContext)
{
    pContext->m_ContextLock.On();

    // close file if it's already open
    if (pContext->m_File)
    {
        fclose(pContext->m_File);
        pContext->m_File = NULL;
    }

    pContext->m_sFileName = "";
    pContext->m_iMaxFileBytes = 0;
    pContext->m_iFileBytes = 0;
    pContext->m_bFileSendMode = FALSE;
    pContext->m_ContextLock.Off();

    return TRUE;
}

/*
* Start to save the incoming data to a file instead of processing it in package process.
*/
BOOL IOCPS::PrepareReceiveFile(ClientContext* pContext, LPCTSTR lpszFilename, DWORD dwFileSize)
{
    if (pContext)
    {
        pContext->m_ContextLock.On();

        // close file if it's already open
        if (pContext->m_File)
        {
            fclose(pContext->m_File);
            pContext->m_File = NULL;
        }

        // open source file
        pContext->m_File = fopen((const char*)lpszFilename, "ab+");
        if (!pContext->m_File)
        {
            pContext->m_iFileBytes = 0;
            pContext->m_iMaxFileBytes = 0;
            pContext->m_bFileReceivedMode = FALSE;

            pContext->m_ContextLock.Off();

            return FALSE;
        }

        pContext->m_sFileName = string(lpszFilename);
        pContext->m_iMaxFileBytes = dwFileSize;
        pContext->m_iFileBytes = 0;
        pContext->m_bFileReceivedMode = TRUE;
        pContext->m_ContextLock.Off();

        TRACERT("Prepare to receive file: %s.", lpszFilename);

        return TRUE;
    }

    return FALSE;
}

BOOL IOCPS::DisableReceiveFile(ClientContext* pContext)
{
    pContext->m_ContextLock.On();

    // close file if it's already open
    if (pContext->m_File)
    {
        fclose(pContext->m_File);
        pContext->m_File = NULL;
    }

    pContext->m_sFileName = "";
    pContext->m_iMaxFileBytes = 0;
    pContext->m_iFileBytes = 0;
    pContext->m_bFileReceivedMode = FALSE;
    pContext->m_ContextLock.Off();

    return TRUE;
}

/*
* Transmitted file Completed.
*/
void IOCPS::OnTransmitFileCompleted(ClientContext* pContext, CIOCPBuffer* pOverlapBuff)
{
    pContext->m_ContextLock.On();
    NotifyFileCompleted(pContext);
    DisableSendFile(pContext);
    pContext->m_ContextLock.Off();
}

#endif

#ifdef SIMPLESECURITY

void IOCPS::OneIPPerConnection(BOOL bVal)
{
    m_bOneIPPerConnection = bVal;
}

/*
* Used to determine which connection to accept and deny.
* By using this callback function we can refuse connections
* in lower kernel mode, we don't respond with ACK when the remote
* connection connects with SYN. This means that the remote attacker
* would not know if he/she is blocked or the server is down.
*/
int CALLBACK IOCPS::ConnectAcceptCondition(IN LPWSABUF lpCallerId,
                                           IN LPWSABUF lpCallerData,
                                           IN OUT LPQOS lpSQOS,
                                           IN OUT LPQOS lpGQOS,
                                           IN LPWSABUF lpCalleeId,
                                           OUT LPWSABUF lpCalleeData,
                                           OUT GROUP FAR *g,
                                           IN DWORD dwCallbackData)
{
    sockaddr_in* pCaller=(sockaddr_in*)lpCallerId->buf;
    sockaddr_in* pCallee=(sockaddr_in*)lpCalleeId->buf;

    IOCPS* pThis = reinterpret_cast<IOCPS*>(dwCallbackData);
    
    // Do not take connections from ourself.
    /*	if ( pCaller->sin_addr.S_un.S_addr == inet_addr("127.0.0.1") )
    {
        return CF_REJECT;
    }
    */

    if (pThis->IsInBannedList(pCaller) || pThis->IsAlreadyConnected(pCaller))
    {
        //
        // Do not send ACK, the attacker do not know if the server
        // exist or not.
        //

        return CF_REJECT;
    }

    return CF_ACCEPT;
}

/*
* Disconnect the accepted socket immediately, if an active connection
* already exist with the same IP.
*
*/

/*
* 	IsAlreadyConnected(sockaddr_in* pCaller)
*
*  Returns TRUE if we already have a connection with the specified
*	address. Add the CLient to the list othervise.
*
*	This function is used with ConnectAcceptCondition and
*  #DEFINE SIMPLESECURITY to block multiple connections from the
*  same IP.
*/
inline BOOL IOCPS::IsAlreadyConnected(sockaddr_in* pCaller)
{
    // We don't use ordinary covert IP to string and string compare
    // FIXME: Whats happens when we hav 64bit processor

    // Load the value of IP (32bit) inside the
    // a void pointer of size 32.

    if (!m_bOneIPPerConnection)
        return FALSE;

    m_OneIPPerConnectionLock.On();
    IPList::iterator iter = find(m_OneIPPerConnectionList.begin(), m_OneIPPerConnectionList.end(), pCaller->sin_addr.S_un.S_addr);
 
    // If it is in the list return TRUE else add it to the list..
    if (iter != m_OneIPPerConnectionList.end())
    {
        m_OneIPPerConnectionLock.Off();
        return TRUE;
    }
    else
    {
        m_OneIPPerConnectionList.push_front(pCaller->sin_addr.S_un.S_addr);
    }

    m_OneIPPerConnectionLock.Off();

    return FALSE;
}


/*
* 	IsInBannedList(sockaddr_in* pCaller)
*
*  Returns TRUE if the user is inside the bannedIP list adress.
*
*	This function is used with ConnectAcceptCondition and
*  #DEFINE SIMPLESECURITY to block connections that behave badly.
*
*  The remote end will not receive any notification and will think
*  that the system is down.
*/
BOOL IOCPS::IsInBannedList(sockaddr_in* pCaller)
{
    // We don't use ordinary covert IP to string and string compare
    // FIXME: Whats happens when we hav 64bit processor
    // Load the value of IP (32bit) inside the
    // a void pointer of size 32.

    m_BanIPLock.On();

    if (!m_BanIPList.empty())
    {
        IPList::iterator iter = find(m_BanIPList.begin(), m_BanIPList.end(), pCaller->sin_addr.S_un.S_addr);
        // The Client is in the banned list refuse
        if (iter != m_BanIPList.end())
        {
            m_BanIPLock.Off();

            return TRUE;
        }
    }

    m_BanIPLock.Off();

    return FALSE;
}

void IOCPS::AddToBanList(SOCKET &Socket)
{
    //
    //  Get the incoming connection IPadress
    //
    sockaddr_in sockAddr;
    memset(&sockAddr, 0, sizeof(sockAddr));
    int nSockAddrLen = sizeof(sockAddr);
    UINT nResult = getpeername(Socket,(SOCKADDR*)&sockAddr, &nSockAddrLen);

    if (nResult != INVALID_SOCKET)
    {
        m_BanIPLock.On();
        // We save our unsigned Long inside a Void pointer of size 32.
        m_BanIPList.push_front(sockAddr.sin_addr.S_un.S_addr);
        m_BanIPLock.Off();

        TRACERT("%s is banned because of bad message size.", inet_ntoa(sockAddr.sin_addr));
    }
}

void IOCPS::ClearBanList()
{
    m_BanIPLock.On();

    if (!m_BanIPList.empty())
    {
        m_BanIPList.clear();
        TRACERT("Banned ip list is cleared.");
        AppendLog("Banned ip list is cleared.");
    }

    m_BanIPLock.Off();
}

#endif

string IOCPS::ErrorCode2Text(DWORD dwError, const char* szFunName/*=""*/)
{
    char errorFmt[32] = "%s in %s: %s"; // ErrorCode, FunctionName, ErrorMsg;
    char error[512] = {0};

    // WSAGetLastError()
    switch (dwError)
    {
    case WSASYSNOTREADY: // WSAStartup()
        sprintf(error, errorFmt, "WSASYSNOTREADY", szFunName, 
            "Indicates that the underlying network subsystem is not ready for network communication.");
        break;
    case WSAVERNOTSUPPORTED: // WSAStartup()
        sprintf(error, errorFmt, "WSAVERNOTSUPPORTED", szFunName, 
            "The version of Windows Sockets support requested is not provided by this particular Windows Sockets implementation."); 
        break;
    case WSAEPROCLIM: // WSAStartup()
        sprintf(error, errorFmt, "WSAEPROCLIM", szFunName, 
            "Limit on the number of tasks supported by the Windows Sockets implementation has been reached."); 
        break;
    case WSANOTINITIALISED: // 没有调用WSAStartup()初始化WinSock库
        sprintf(error, errorFmt, "WSANOTINITIALISED", szFunName, 
            "A successful WSAStartup call must occur before using this function.");
        break;
    case WSAENETDOWN:
        sprintf(error, errorFmt, "WSAENETDOWN", szFunName, 
            "The network subsystem has failed.");
        break;
    case WSAEFAULT:
        sprintf(error, errorFmt, "WSAEFAULT", szFunName, 
            // "The lpWSAData is not a valid pointer." // WSAStartup()
            "The buf parameter is not completely contained in a valid part of the user address space.");
        break;
    case WSAENOTCONN:
        sprintf(error, errorFmt, "WSAENOTCONN", szFunName, 
            "The socket is not connected.");
        break;
    case WSAEINTR:
        sprintf(error, errorFmt, "WSAEINTR", szFunName, 
            "The (blocking) call was canceled through WSACancelBlockingCall.");
        break;
    case WSAENOTSOCK:
        sprintf(error, errorFmt, "WSAENOTSOCK", szFunName, 
            "The descriptor is not a socket.");
        break;
    case WSAEINPROGRESS:
        sprintf(error, errorFmt, "WSAEINPROGRESS", szFunName, 
            "A blocking Windows Sockets 1.1 call is in progress, or the service provider is still processing a callback function.");
        break;
    case WSAENETRESET:
        sprintf(error, errorFmt, "WSAENETRESET", szFunName, 
            "The connection has been broken due to the keep-alive activity detecting a failure while the operation was in progress.");
        break;
    case WSAEOPNOTSUPP:
        sprintf(error, errorFmt, "WSAEOPNOTSUPP", szFunName, 
            "MSG_OOB was specified, but the socket is not stream-style such as type SOCK_STREAM, OOB data is not supported in the communication domain associated with this socket, or the socket is unidirectional and supports only send operations.");
        break;
    case WSAESHUTDOWN:
        sprintf(error, errorFmt, "WSAESHUTDOWN", szFunName, 
            "The socket has been shut down; it is not possible to receive on a socket after shutdown has been invoked with how set to SD_RECEIVE or SD_BOTH.");
        break;
    case WSAEWOULDBLOCK:
        sprintf(error, errorFmt, "WSAEWOULDBLOCK", szFunName, 
            "The socket is marked as nonblocking and the receive operation would block.");
        break;
    case WSAEMSGSIZE:
        sprintf(error, errorFmt, "WSAEMSGSIZE", szFunName, 
            "The message was too large to fit into the specified buffer and was truncated.");
        break;
    case WSAEINVAL:
        sprintf(error, errorFmt, "WSAEINVAL", szFunName, 
            "The socket has not been bound with bind, or an unknown flag was specified, or MSG_OOB was specified for a socket with SO_OOBINLINE enabled or (for byte stream sockets only) len was zero or negative.");
        break;
    case WSAECONNABORTED:
        sprintf(error, errorFmt, "WSAECONNABORTED", szFunName, 
            "The virtual circuit was terminated due to a time-out or other failure. The application should close the socket as it is no longer usable.");
        break;
    case WSAETIMEDOUT:
        sprintf(error, errorFmt, "WSAETIMEDOUT", szFunName, 
            "The connection has been dropped because of a network failure or because the peer system failed to respond.");
        break;
    case WSAECONNRESET:
        sprintf(error, errorFmt, "WSAECONNRESET", szFunName, 
            // "The virtual circuit was reset by the remote side executing a hard or abortive close.";
            "The virtual circuit was reset by the remote side.");
        break;
    case WSA_IO_PENDING:
        sprintf(error, errorFmt, "WSA_IO_PENDING", szFunName, 
            "An overlapped operation was successfully initiated and completion will be indicated at a later time.");
        break;
    case WSA_OPERATION_ABORTED:
        sprintf(error, errorFmt, "WSA_OPERATION_ABORTED", szFunName, 
            "The overlapped operation has been canceled due to the closure of the socket, or the execution of the SIO_FLUSH command in WSAIoctl.");
        break;
    case WSAENOBUFS:
        sprintf(error, errorFmt, "WSAENOBUFS", szFunName, 
            "The Windows Sockets provider reports a buffer deadlock.");
        break;
    default:
        break;
    }

    // GetLastError()
    if (strlen(error) == 0)
    {
        LPVOID lpMsgBuf;
        FormatMessage(
            FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS|FORMAT_MESSAGE_ALLOCATE_BUFFER,
            NULL, dwError, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            (LPTSTR)&lpMsgBuf, 0, NULL);
        sprintf(error, "%d in %s: %s", dwError, szFunName, lpMsgBuf);
        LocalFree(lpMsgBuf);
    }

    return string(error);
}

void IOCPS::TimeOutCallback(PVOID lpParameter, BOOLEAN TimerOrWaitFired)
{
    // 超时
    if (TimerOrWaitFired)
    {
        SOCKET sock = (SOCKET)lpParameter;
        iocpserver->TRACERT("Disconnect %s for long time no see i/o from/to it.", iocpserver->GetHost(sock).c_str());
        iocpserver->DisconnectClient(sock);
        // iocpserver->ReleaseClientContext();
    }
}