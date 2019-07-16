#include "stdafx.h"
#include "Lock.h"

Lock::Lock(void)
{
    InitializeCriticalSection(&m_cs);
}

Lock::~Lock(void)
{
    DeleteCriticalSection(&m_cs);
}

void Lock::On()
{
    EnterCriticalSection(&m_cs);
}

void Lock::Off()
{
    LeaveCriticalSection(&m_cs);
}
