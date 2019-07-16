// MyIOCP.h: interface for the MyIOCP class.
//
//////////////////////////////////////////////////////////////////////

#if !defined(AFX_MYIOCP_H__4093193A_D0A0_4E6A_82FA_65B17A1FEB71__INCLUDED_)
#define AFX_MYIOCP_H__4093193A_D0A0_4E6A_82FA_65B17A1FEB71__INCLUDED_

#if _MSC_VER > 1000
#pragma once
#endif // _MSC_VER > 1000
#include "IOCPS.h"
// #include "MyListCtrl.h"

#define WM_NEW_CONNECTION	WM_APP + 0x1001
#define WM_LOGG_APPEND	WM_NEW_CONNECTION+1
#define WM_CLIENTDISCONNECTED WM_NEW_CONNECTION+2


class MyIOCP : public IOCPS  
{
public:
    MyIOCP();
    virtual ~MyIOCP();

    /*
    inherited interfaces
    */
    // Called to do some work. 
    virtual inline void ProcessJob(JobItem* pJob, IOCPS* pServer);
    // logger
    virtual void AppendLog(const char* fmt, ...);

    // A new connection have been established
    virtual void NotifyNewConnection(ClientContext* pcontext);
    // A client have connected
	virtual void NotifyNewClientContext(ClientContext* pContext);
	// A client have disconnected
	virtual void NotifyDisconnectedClient(ClientContext* pContext);
	// A Package have arrived. 
	virtual void NotifyReceivedPackage(CIOCPBuffer* pOverlapBuff, int nSize, ClientContext* pContext);	

    /*
    * customize send
    */
    // Build the a text message message and send it.. 
    BOOL BuildPackageAndSend(ClientContext* pContext, string& sText);
    BOOL BuildPackageAndSend(int ClientID, string& sText);
    BOOL BuildPackageAndSendToAll(string& sText);

    // Builds a Package containing info about a filetransfer and send it to the Remote Computer
    BOOL BuildFilePackageAndSend(ClientContext* pContext, string& sFile);
    BOOL BuildFilePackageAndSend(int ClientID, string& sFile);

     // Build StartFileTransfer
    BOOL BuildStartFileTransferPackageAndSend(ClientContext* pContext);
    BOOL BuildStartFileTransferPackageAndSend(int ClientID);

    /*
    * customize package processing
    */
	// Text in a Package is arrived. 
	void Packagetext(CIOCPBuffer* pOverlapBuff, int nSize, ClientContext* pContext);
    // A start transfering package is arrived.
    void PackageStartFileTransfer(CIOCPBuffer* pOverlapBuff, int nSize, ClientContext* pContext);
    // A File transfer info package is arrived
	void PackageFileTransfer(CIOCPBuffer* pOverlapBuff, int nSize, ClientContext* pContext);

public:
    HWND m_hWnd;
    int m_iNumberOfMsg;
    string m_sSendText;
    volatile BOOL m_bFlood; // Used to Flood..
    Lock m_StatusLock;

    // Auoreconnect stuff.
    BOOL m_bAutoReconnect;
    string m_sAutoReconnect;
    int m_iAutoReconnect;
};

#endif // !defined(AFX_MYIOCP_H__4093193A_D0A0_4E6A_82FA_65B17A1FEB71__INCLUDED_)