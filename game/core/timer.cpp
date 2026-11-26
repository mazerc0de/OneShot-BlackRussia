#include "timer.h"
#include "gameLibAddr.h"
#include <chrono>

CTimer::CTimer()
{
    m_snTimeInMilliseconds = 1;
    m_fTimeScale = 1.f;
}

void CTimer::Update()
{
#ifdef __aarch64__
    SetTimeInMilliseconds((int)(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() & 0x7fffffff));
#else
    float* timeScale = (float *)(gameLibAddr + 0x464508C);
    if(timeScale)
    {
        SetTimeScale(*timeScale);
    }

    int* timeInMilliseconds = (int *)(gameLibAddr + 0x464507C);
    if(timeInMilliseconds)
    {
        SetTimeInMilliseconds(*timeInMilliseconds);
    }
#endif
}
