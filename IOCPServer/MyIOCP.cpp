// MyIOCP.cpp: implementation of the MyIOCP class.
//
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "IOCPServer.h"
#include "IOCPServer.h" // for server
// #include "IOCPClient.h" // for client
#include "MyIOCP.h"

#include <time.h>

#ifdef _DEBUG
#undef THIS_FILE
static char THIS_FILE[]=__FILE__;
#define new DEBUG_NEW
#endif

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

MyIOCP::MyIOCP()
{
    m_bFlood = FALSE;
    m_sSendText = "";
    m_iNumberOfMsg = 0;

    m_bAutoReconnect = FALSE;
    m_sAutoReconnect = "";
    m_iAutoReconnect = 0;
}

MyIOCP::~MyIOCP()
{
}

/*
* This functions defines what should be done when A job from the work queue is arrived.
* (not used).
*/
void MyIOCP::ProcessJob(JobItem* pJob, IOCPS* pServer)
{
    switch (pJob->m_command)
    {
    case Job_SendText2Client :

        break;
    }
}

void MyIOCP::AppendLog(const char* fmt, ...)
{
    char* buf = new char[512];

    va_list args;
    va_start(args, fmt);
    _vsnprintf(buf, 512, fmt, args); // >512截断，尾部没有'\0'
    va_end(args);

    strcat(buf, "\n");

    BOOL bRet = ::PostMessage(m_hWnd, WM_LOGG_APPEND, 0, (LPARAM)buf);
}

void MyIOCP::NotifyNewConnection(ClientContext* pcontext)
{
    unsigned int* pBuffer = new unsigned int;

    if (pBuffer)
        *pBuffer = pcontext->m_Socket;

    ::PostMessage(m_hWnd, WM_NEW_CONNECTION, 0, (LPARAM)pBuffer);
}

void MyIOCP::NotifyNewClientContext(ClientContext* pContext)
{
    pContext->m_iNumberOfReceivedMsg = 0;
    pContext->m_bUpdate = FALSE;
}

void MyIOCP::NotifyDisconnectedClient(ClientContext *pContext)
{
    /*
    unsigned int* pBuffer = new unsigned int;

    if (pBuffer)
        *pBuffer = pContext->m_Socket;

    ::PostMessage(m_hWnd, WM_CLIENTDISCONNECTED, 0, (LPARAM)pBuffer);
    */

    m_StatusLock.On();
    if (m_bAutoReconnect)
    {
        srand((unsigned)time(NULL));
        Sleep(rand()%79);
        Connect(m_sAutoReconnect, m_iAutoReconnect);
    }
    m_StatusLock.Off();
}

void MyIOCP::NotifyReceivedPackage(CIOCPBuffer* pOverlapBuff, int nSize, ClientContext* pContext)
{
    TRACERT("Received %d bytes from %s.", nSize, GetHost(pContext->m_Socket).c_str());

    BYTE PackageType = pOverlapBuff->GetPackageType();

    switch (PackageType)
    {
    case Job_SendText2Client:
        Packagetext(pOverlapBuff, nSize, pContext);
        break;
    case Job_SendFileInfo: // 文件接收方接收到此消息
        PackageFileTransfer(pOverlapBuff, nSize, pContext);
        break;
    case Job_StartFileTransfer: // 文件发送方接收到接收方对Job_SendFileInfo的反馈，同意传输文件。
        PackageStartFileTransfer(pOverlapBuff, nSize, pContext);
        break;
    case Job_AbortFileTransfer:

#ifdef TRANSFERFILEFUNCTIONALITY
        DisableSendFile(pContext->m_Socket);
#endif

        break;
    }
}

// Build the a text message message and send it..
BOOL MyIOCP::BuildPackageAndSend(ClientContext* pContext, string& sText)
{
    CIOCPBuffer* pOverlapBuff = AllocateBuffer(IOWrite);

    if (pOverlapBuff)
    {
        if (pOverlapBuff->CreatePackage(Job_SendText2Client, sText))
            return ASend(pContext, pOverlapBuff);
        else
        {
            TRACERT("CreatePackage(Job_SendText2Client, sText) failed in BuildPackageAndSend()");
            ReleaseBuffer(pOverlapBuff);

            return FALSE;
        }
    }
    else
    {
        TRACERT("AllocateBuffer() failed in BuildPackageAndSend().");
        AppendLog("AllocateBuffer() failed in BuildPackageAndSend().");        
        DisconnectClient(pContext->m_Socket);

        return FALSE;
    }

    return FALSE;
}

BOOL MyIOCP::BuildPackageAndSend(int ClientID, string& sText)
{
    BOOL bRet = FALSE;
    m_ContextMapLock.On();
    ClientContext* pContext = FindClient(ClientID);

    if (!pContext)
        return FALSE;

    bRet = BuildPackageAndSend(pContext, sText);
    m_ContextMapLock.Off();

    return bRet;
}

BOOL MyIOCP::BuildPackageAndSendToAll(string& sText)
{
    CIOCPBuffer* pOverlapBuff = AllocateBuffer(IOWrite);

    if (pOverlapBuff)
    {
        if (pOverlapBuff->CreatePackage(Job_SendText2Client, sText))
            return ASendToAll(pOverlapBuff);
        else
        {
            TRACERT("CreatePackage(Job_SendText2Client, sText) failed in BuildPackageAndSendToAll().");
            ReleaseBuffer(pOverlapBuff);

            return FALSE;
        }
    }
    else
    {
        TRACERT("Could not allocate memory in BuildPackageAndSendToAll().");

        return FALSE;
    }

    return FALSE;
}

/*
* Perpares for file transfer and sends a package containing information about the file.
*/
BOOL MyIOCP::BuildFilePackageAndSend(ClientContext* pContext, string& sFile)
{
#ifdef TRANSFERFILEFUNCTIONALITY
    return PrepareSendFile(pContext->m_Socket, sFile);
#else
    return FALSE;
#endif
}

BOOL MyIOCP::BuildFilePackageAndSend(int ClientID, string& sFile)
{
    BOOL bRet = FALSE;
    m_ContextMapLock.On();
    ClientContext* pContext = FindClient(ClientID);

    if (!pContext)
    {
        m_ContextMapLock.Off(); // add by fan
        return FALSE;        
    }

    bRet = BuildFilePackageAndSend(pContext, sFile);
    m_ContextMapLock.Off();

    return bRet;
}

/*
* Send a "Start the file transfer package" to the remote connection.
*/
BOOL MyIOCP::BuildStartFileTransferPackageAndSend(ClientContext* pContext)
{
    CIOCPBuffer* pOverlapBuff = AllocateBuffer(IOWrite);

    if (pOverlapBuff)
    {
        if (pOverlapBuff->CreatePackage(Job_StartFileTransfer))
            return ASend(pContext, pOverlapBuff);
        else
        {
            TRACERT("CreatePackage(Job_StartFileTransfer) failed in BuildStartFileTransferPackageAndSend().");
            ReleaseBuffer(pOverlapBuff);
            
            return FALSE;
        }
    }
    else
    {
        TRACERT("AllocateBuffer() failed in BuildStartFileTransferPackageAndSend().");
        AppendLog("AllocateBuffer() failed in BuildStartFileTransferPackageAndSend().");
        DisconnectClient(pContext->m_Socket);

        return FALSE;
    }

    return TRUE;
}

BOOL MyIOCP::BuildStartFileTransferPackageAndSend(int ClientID)
{
    BOOL bRet=FALSE;
    m_ContextMapLock.On();
    ClientContext* pContext = FindClient(ClientID);

    if (!pContext)
        return FALSE;

    bRet = BuildStartFileTransferPackageAndSend(pContext);
    m_ContextMapLock.Off();

    return bRet;
}

// Text in a Package is arrived.
void MyIOCP::Packagetext(CIOCPBuffer* pOverlapBuff, int nSize, ClientContext* pContext)
{
    string txt = "";
    BYTE type;

    if (pOverlapBuff->GetPackageInfo(type, txt))
    {
        // to be sure that pcontext Suddenly does not dissapear by disconnection...
        m_ContextMapLock.On();
        pContext->m_ContextLock.On();
        pContext->m_sReceived = txt;
        // Update that we have data
        pContext->m_iNumberOfReceivedMsg++;
        pContext->m_bUpdate = TRUE;
        pContext->m_ContextLock.Off();
        m_ContextMapLock.Off();

        // Update Statistics.
        m_StatusLock.On();
        m_iNumberOfMsg++;
        m_StatusLock.Off();
        // Send back the message if we are echoing.
        // Send Flood if needed.
        BOOL bRet = FALSE;

        if (m_bFlood) // just echo back to the client
            bRet = BuildPackageAndSend(pContext, m_sSendText);
    }
}

/*
* This function is called when the remote connection, want to send a file.
*/
void MyIOCP::PackageFileTransfer(CIOCPBuffer* pOverlapBuff, int nSize, ClientContext* pContext)
{
#ifdef TRANSFERFILEFUNCTIONALITY

    string sFileName = "";
    UINT iMaxFileSize = 0;
    BYTE dummy = 0;

    if (pOverlapBuff->GetPackageInfo(dummy, iMaxFileSize, sFileName))
    {
        // Get The Current Path name and application name.
        string sFilePath="";
        char drive[_MAX_DRIVE];
        char dir[_MAX_DIR];
        char fname[_MAX_FNAME];
        char ext[_MAX_EXT];
        char strTemp[MAX_PATH] = {0};// string strTemp;
        GetModuleFileName(NULL, (LPTSTR)strTemp/*.c_str()*/, MAX_PATH); // 中文路径问题
        // strTemp.ReleaseBuffer();
        _splitpath(strTemp/*.c_str()*/, drive, dir, fname, ext);
        sFilePath = drive; // Drive
        sFilePath += dir; // dir
        sFilePath += sFileName; // name..        
        TRACERT("Incoming File > %s.", sFilePath);
        // Perpare for Receive

        if (PrepareReceiveFile(pContext->m_Socket, (LPCTSTR)sFilePath.c_str(), iMaxFileSize))
        {
            // Send start file transfer..
            CIOCPBuffer *pOverlapBuff = AllocateBuffer(IOWrite);

            if (pOverlapBuff)
            {
                if (pOverlapBuff->CreatePackage(Job_StartFileTransfer))
                    ASend(pContext, pOverlapBuff);
            }
        }
        else
        {
            // Abort Transfer.
            CIOCPBuffer* pOverlapBuff = AllocateBuffer(IOWrite);

            if (pOverlapBuff)
            {
                if (pOverlapBuff->CreatePackage(Job_AbortFileTransfer))
                    ASend(pContext,pOverlapBuff);
            }
        }

        // to be sure that pcontext Suddenly does not dissapear..
        m_ContextMapLock.On();
        pContext->m_ContextLock.On();
        pContext->m_sReceived = sFilePath;
        // Update that we have data
        pContext->m_iNumberOfReceivedMsg++;
        pContext->m_ContextLock.Off();
        m_ContextMapLock.Off();

        // Update Statistics.
        m_StatusLock.On();
        m_iNumberOfMsg++;
        m_StatusLock.Off();
    }

#endif
}

// The remote Connections whant to start the transfer.
void MyIOCP::PackageStartFileTransfer(CIOCPBuffer* pOverlapBuff, int nSize, ClientContext* pContext)
{
#ifdef TRANSFERFILEFUNCTIONALITY
    StartSendFile(pContext->m_Socket);
#endif
}