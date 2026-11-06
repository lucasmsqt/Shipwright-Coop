#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct Actor;

typedef struct ScrubMotion {
    s32 phase;
    s32 timer;
    f32 frame;
    f32 speed;
    s16 height;
    s16 attackable;
} ScrubMotion;
s32 Anchor_ShouldSpawnScrubShot(struct Actor* scrub);
void Anchor_SendScrubShotSpawn(struct Actor* actor);
void Anchor_SendScrubShotReflect(struct Actor* actor);
void Anchor_SendScrubShotEnd(struct Actor* actor);
s32 EnDekunuts_GetTeamMotion(struct Actor* scrub, ScrubMotion* motion);
s32 Anchor_ApplyScrubPose(struct Actor* actor, ScrubMotion* motion);

#ifdef __cplusplus
}
#endif
