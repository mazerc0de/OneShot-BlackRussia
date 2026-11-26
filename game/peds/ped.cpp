#include "ped.h"
#include "memory/memory.h"
#include "gameLibAddr.h"

// arm64 (BR 16.116) CPed field offsets — live-verified via PEDDBG float scan:
// health +0x4B0=100, healthLimit +0x4B4=100, armour +0x4B8=0, armourLimit +0x4BC=100.
// moved +0x4D8, rotationCur/Dest +0x4E0/0x4E4, headingRate +0x4E8.
// CPed fields = .h model +0x28. CPhysical unchanged: pos +0x38/0x3C/0x40,
// vecMoveSpeed +0x130, m_fMass +0x18C, physFlags +0x230.
#ifdef __aarch64__
#define PED_OFF_HEALTH       0x4B0
#define PED_OFF_HEALTHLIMIT  0x4B4
#define PED_OFF_ARMOUR       0x4B8
#define PED_OFF_HEADINGRATE  0x4F8  // verified: default 7.5 in PEDDBG scan
#define PED_OFF_JUMPMULT     0x1C8  // m_fMass +0x1C8 (70.0) verified via PEDDBG
#define PED_OFF_PEDSTATE     0x494
#define PED_OFF_PREVSTATE    0x498
#define PED_OFF_MOVESTATE    0x49C
#define PED_OFF_VEHICLE      0x538
#define PED_OFF_INVEHICLE    0x540
#else
#define PED_OFF_HEALTH       0x360
#define PED_OFF_ARMOUR       0x364
#define PED_OFF_HEADINGRATE  0x38C
#define PED_OFF_JUMPMULT     0xC4
#define PED_OFF_PEDSTATE     0x250
#define PED_OFF_PREVSTATE    0x254
#define PED_OFF_MOVESTATE    0x258
#define PED_OFF_VEHICLE      0x3B0
#define PED_OFF_INVEHICLE    0x3B8
#endif

CPed::CPed(uintptr_t ped)
{
    thiz = ped;
    health = 0.f;
    armour = 0.f;
    headingRate = 0.f;
    jumpMult = 0.f;
    pedState = (ePedState)0;
    prevPedState = (ePedState)0;
    pedMoveState = (eMoveState)0;
    animationID = 0;
    vehicle = 0;
    inVehicle = 0;
    if(thiz < 0x100000) { return; } // pool slots can hold small garbage values
    health = *(float *)(thiz + PED_OFF_HEALTH);
    armour = *(float *)(thiz + PED_OFF_ARMOUR);
    headingRate = *(float *)(thiz + PED_OFF_HEADINGRATE);
    jumpMult = *(float *)(thiz + PED_OFF_JUMPMULT);
    pedState = *(ePedState *)(thiz + PED_OFF_PEDSTATE);
    prevPedState = *(ePedState *)(thiz + PED_OFF_PREVSTATE);
    pedMoveState = *(eMoveState *)(thiz + PED_OFF_MOVESTATE);
#ifdef __aarch64__
    animationID = 0; // arm64 anim id offset unverified
#else
    animationID = *(uint16_t *)(ped + 0x64C);
#endif
    if(*(uintptr_t *)(thiz + PED_OFF_VEHICLE))
    {
        vehicle = *(uintptr_t *)(thiz + PED_OFF_VEHICLE);
    }
    else
    {
        vehicle = 0;
    }
	inVehicle = *(int8_t *)(thiz + PED_OFF_INVEHICLE);
}

void CPed::Update()
{
    if(thiz < 0x100000) { return; }
    health = *(float *)(thiz + PED_OFF_HEALTH);
    armour = *(float *)(thiz + PED_OFF_ARMOUR);
    headingRate = *(float *)(thiz + PED_OFF_HEADINGRATE);
    jumpMult = *(float *)(thiz + PED_OFF_JUMPMULT);
    pedState = *(ePedState *)(thiz + PED_OFF_PEDSTATE);
    prevPedState = *(ePedState *)(thiz + PED_OFF_PREVSTATE);
    pedMoveState = *(eMoveState *)(thiz + PED_OFF_MOVESTATE);
#ifdef __aarch64__
    animationID = 0;
#else
    animationID = *(uint16_t *)(thiz + 0x64C);
#endif
    if(*(uintptr_t *)(thiz + PED_OFF_VEHICLE))
    {
        vehicle = *(uintptr_t *)(thiz + PED_OFF_VEHICLE);
    }
    else
    {
        vehicle = 0;
    }
}

void CPed::SetHealth(float hp)
{
    *(float *)(thiz + PED_OFF_HEALTH) = hp;
}

void CPed::SetArmour(float armor)
{
    *(float *)(thiz + PED_OFF_ARMOUR) = armor;
}

void CPed::SetHeadingRate(float rate)
{
    *(float *)(thiz + PED_OFF_HEADINGRATE) = rate;
}

void CPed::SetHighJumpMult(float mult)
{
    *(float *)(thiz + PED_OFF_JUMPMULT) = mult;
}

void CPed::SetLongJumpMult(float mult)
{
#ifndef __aarch64__
    WriteMemoryFloat(gameLibAddr + 0x392FDC, mult);
#endif
    // arm64: long jump handled per-frame via velocity in CheatFrameBody
}

void CPed::SetPedState(ePedState state)
{
    *(ePedState *)(thiz + PED_OFF_PEDSTATE) = state;
}

void CPed::SetPrevPedState(ePedState state)
{
    *(ePedState *)(thiz + PED_OFF_PREVSTATE) = state;
}

void CPed::SetMoveState(eMoveState state)
{
    *(eMoveState *)(thiz + PED_OFF_MOVESTATE) = state;
}

void CPed::SetMoveAnim()
{
#ifndef __aarch64__
    ((void(*)(uintptr_t))(gameLibAddr + 0x37da34 + 1))(thiz);
#endif
}
