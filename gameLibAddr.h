#ifndef GAMELIBADDR_H
#define GAMELIBADDR_H

#include "stdint.h"
#include "cheat/cheat.h"

extern uintptr_t gameLibAddr;
extern Cheat* cheatClient;
// arm64 camera captured from outgoing AIM_SYNC (id=34) packets:
// camFront @ +9 (3f), camPos @ +25 (3f) — filled by the send hook in main.cpp
extern float g_camFront[3];
extern float g_camPos[3];
extern int g_camValid;

#endif