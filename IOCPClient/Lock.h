#pragma once

class Lock
{
public:
    Lock(void);
    ~Lock(void);

    void On();
    void Off();

private:
    CRITICAL_SECTION m_cs;
};
