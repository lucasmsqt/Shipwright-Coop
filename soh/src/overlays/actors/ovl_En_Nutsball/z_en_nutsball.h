#ifndef Z_EN_NUTSBALL_H
#define Z_EN_NUTSBALL_H

#include <libultraship/libultra.h>
#include "global.h"

struct EnNutsball;

typedef void (*EnNutsballActionFunc)(struct EnNutsball*, PlayState*);

typedef struct EnNutsball {
    /* 0x0000 */ Actor actor;
    /* 0x014C */ EnNutsballActionFunc actionFunc;
    /* 0x0150 */ s8 objBankIndex;
    /* 0x0152 */ s16 timer;
    /* 0x0154 */ ColliderCylinder collider;
    u32 sharedOwner;
    u32 sharedSerial;
    u32 sharedHitFrame;
    bool sharedReplica;
    bool sharedReflected;
    bool sharedReflectedLocally;
} EnNutsball;

Actor* EnNutsball_SpawnFromScrub(PlayState* play, Actor* scrub, Vec3f* pos, s16 type);
void EnNutsball_MarkShared(EnNutsball* shot, u32 owner, u32 serial, bool replica);
void EnNutsball_Reflect(EnNutsball* shot, Vec3f* pos, s16 yaw);
s32 EnNutsball_AllowPlayerContact(EnNutsball* shot, PlayState* play);

#endif
