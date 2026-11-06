#pragma once

#include <libultraship/libultra.h>
#include "z64math.h"

struct Actor;
struct PlayState;

enum AnchorRoomObjectPhase {
    ANCHOR_BLOCK_MOVING = 1,
    ANCHOR_BLOCK_SETTLED,
    ANCHOR_SCRUB_HIT,
    ANCHOR_SCRUB_LEFT,
    ANCHOR_OBJECT_BROKEN,
    ANCHOR_TREE_SKULLTULA,
    ANCHOR_TALON_AWAKENING,
    ANCHOR_DOOR_OPENING,
    ANCHOR_SUN_STUN,
    ANCHOR_TORCH_STATE,
};

#ifdef __cplusplus
extern "C" {
#endif

void Anchor_SendRoomObject(struct Actor* actor, s32 phase, s32 detail);
s32 Anchor_ReadRoomObject(struct Actor* actor, Vec3f* position, s32* detail, u64* seen);
s32 Anchor_ShouldUpdateSharedScrub(struct Actor* actor);
s32 Anchor_ReadBreakable(struct Actor* actor, Vec3f* position);
void Anchor_SpawnTreeSkulltula(struct PlayState* play, Vec3f* position, s16 yaw, s16 params);
void Anchor_SendDoorOpening(struct Actor* actor, s32 leaf, s32 style);
s32 Anchor_ReadDoorOpening(struct Actor* actor, s32 leaf, u64* seen);
void Anchor_StartSunStun(struct PlayState* play);
s32 Anchor_ConsumeSunStun(struct Actor* actor, u64* seen);

#ifdef __cplusplus
}
#endif
