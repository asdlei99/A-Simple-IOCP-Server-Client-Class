// IOCPBuffer.h: interface for the CIOCPBuffer class.
//
//////////////////////////////////////////////////////////////////////

#if !defined(AFX_IOCPBUFFER_H__87DEF694_32D6_4F21_8FF5_E16184A9CDC3__INCLUDED_)
#define AFX_IOCPBUFFER_H__87DEF694_32D6_4F21_8FF5_E16184A9CDC3__INCLUDED_

#if _MSC_VER > 1000
#pragma once
#endif // _MSC_VER > 1000

/*
 * This Class is used to pass around the buffer and the operation that
 * is done asynchronously.
 *
 */
// #include "IOCPS.h"

#include <windows.h>
#include <winsock2.h>
#pragma comment(lib,"ws2_32.lib")
#include "mswsock.h"
#pragma comment(lib,"mswsock.lib")

#include <string>
using namespace std;

// Determines the size of the first bytes who tells you how big the message are. (pakage heap)
#define MINIMUMPACKAGESIZE    sizeof(UINT)
#define MAXIMUMPACKAGESIZE    512 // 可以根据实际需求修改，例如32*1024,64*1024
#define MAXIMUMSEQUENSENUMBER 5001
#define MAXIMUMPAYLOADSIZE MAXIMUMPACKAGESIZE-MINIMUMPACKAGESIZE

// per i/o data: wrapper of OVERLAPPED & WSABUF
class CIOCPBuffer
{
public:
    CIOCPBuffer();
    virtual ~CIOCPBuffer();

    void Init();

    // SequenceNumber property
    void SetSequenceNumber(int nr);
    int GetSequenceNumber();

    // IOType property
    void SetOperation(int op);
    int GetOperation();

    // POSITION property
    // POSITION GetPosition();
    // void SetPosition(POSITION pos);

    // Setup Different Types in buffer.
    void SetupZeroByteRead();
    void SetupRead();
    void SetupWrite();

    // return pointer to the Buffer
    PBYTE GetBuffer();
    // skip MINIMUMPACKAGESIZE=4 byte is the payload data portion
    PBYTE GetPayLoadBuffer();
    // return pointer to the WSABUF structure
    WSABUF* GetWSABuffer();

    // UINT GetSize();
    // return m_nUsed
    UINT GetUsed();
    // set m_nUsed zero
    void EmptyUsed();
    // add use
    UINT Use(UINT nSize);
    // skip nBytesToRemove bytes, minus use
    BOOL Flush(UINT nBytesToRemove);

    //
    // Write different types of variabels to the buffer
    //
    BOOL AddData(const BYTE* pData, UINT nSize);
    BOOL AddData(const char* pData, UINT nSize);
    BOOL AddData(BYTE data);
    BOOL AddData(UINT data);
    BOOL AddData(unsigned short data);

    //
    //  Create different type of Packages in the Buffer..
    //
    BOOL CreatePackage(BYTE Type); // 一般用作控制信令

    BOOL CreatePackage(string& stxt);
    BOOL GetPackageInfo(string& stxt);

    BOOL CreatePackage(BYTE Type, string& stxt);
    BOOL GetPackageInfo(BYTE& Type, string& stxt);

    BOOL CreatePackage(BYTE Type, BYTE key, string& stxt);
    BOOL GetPackageInfo(BYTE& Type, BYTE& key, string& stxt);

    BOOL CreatePackage(BYTE Type, UINT nData, string& stxt);
    BOOL GetPackageInfo(BYTE& Type, UINT& nData, string& stxt);

    BOOL CreatePackage(BYTE Type, BYTE key1, BYTE key2, BYTE key3, string& stxt);
    BOOL GetPackageInfo(BYTE& Type, BYTE& key1, BYTE& key2, BYTE& key3, string& stxt);

    BOOL CreatePackage(BYTE Type, UINT iFilesize, UINT iBufferSize, const BYTE *const pData);

    //
    // Get information from the Package..
    //
    // Reurns the Pakage size if possible -1(0xffffffff) if error.
    UINT GetPackageSize(); // 第1~4个字节(UINT)为包长
    // Gets The package Type returns 255(0xff) if error.
    BYTE GetPackageType(); // 第5个字节为类型

    // not used yet
    BOOL IsValid();

public:
    // Sequence Priority
    struct SequencePriority
    {
        bool operator()(const CIOCPBuffer *pIOBuf1, const CIOCPBuffer *pIOBuf2) const
        {
            return (pIOBuf1->m_iSequenceNumber > pIOBuf2->m_iSequenceNumber);
        }
    };
    // Used with overlapped..
    OVERLAPPED m_ol;

private:
    // Holds the Buffer Used for send/receive
    WSABUF m_wsabuf;

    BYTE m_Buffer[MAXIMUMPACKAGESIZE]; // real buffer block
    // Size of the Buffer
    // UINT	m_nSize;
    // number of bytes that are Used.
    UINT m_nUsed;

    // The Type of Operation.
    int	m_Operation;
    // The buffer.
    // The I/O sequence number
    int m_iSequenceNumber;

    // Used to remove the buffer from the queue.
    // POSITION m_pPos; // position in the IOCPS::m_BufferList
};

#endif // !defined(AFX_IOCPBUFFER_H__87DEF694_32D6_4F21_8FF5_E16184A9CDC3__INCLUDED_)