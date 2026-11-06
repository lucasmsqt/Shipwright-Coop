#pragma once

#include <libultraship/libultra.h>

struct Actor;
struct EnRd;

typedef struct RedeadMotion {
    f32 frame;
    f32 yOffset;
    s16 headYaw;
    s16 torsoYaw;
    s16 sunFrames;
    s16 action;
    s16 animation;
    s16 localGrab;
} RedeadMotion;

#ifdef __cplusplus
extern "C" {
#endif

void EnRd_GetTeamMotion(const struct EnRd* redead, RedeadMotion* motion);
s32 Anchor_ApplyRedeadPose(struct Actor* actor, RedeadMotion* motion);
struct Actor* Anchor_FindRedeadTarget(struct Actor* actor);

#ifdef __cplusplus
}
#endif
