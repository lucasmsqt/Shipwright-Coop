#pragma once

#include <libultraship/libultra.h>
#include "z64math.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Actor;
struct PlayState;

typedef struct AnchorDodongoPlatform {
    f32 height;
    f32 targetHeight;
    s32 state;
    s32 idleTimer;
    s32 speed;
    s32 moving;
} AnchorDodongoPlatform;

typedef struct AnchorBeamosBeam {
    Vec3s rotation;
    Vec3f scale;
    Vec3f endpoint;
    f32 frame;
    s16 headYaw;
    s16 stage;
} AnchorBeamosBeam;

typedef struct AnchorDodongoBomb {
    u32 serial;
    u32 owner;
    u8 exploded;
} AnchorDodongoBomb;

enum AnchorDodongoBossPhase {
    ANCHOR_DODONGO_INTRO, ANCHOR_DODONGO_WALK, ANCHOR_DODONGO_ROLL, ANCHOR_DODONGO_INHALE, ANCHOR_DODONGO_FIRE, ANCHOR_DODONGO_SWALLOWED,
    ANCHOR_DODONGO_FALL, ANCHOR_DODONGO_VULNERABLE, ANCHOR_DODONGO_GET_UP, ANCHOR_DODONGO_DAMAGED, ANCHOR_DODONGO_DEAD, ANCHOR_DODONGO_PHASE_COUNT,
};

typedef struct AnchorDodongoBoss {
    Vec3f position;
    Vec3s rotation;
    f32 speed;
    f32 frame;
    f32 animationSpeed;
    f32 animationStart;
    f32 animationEnd;
    f32 yOffset;
    f32 bodyStretch;
    s16 phase;
    s16 animation;
    s16 animationMode;
    s16 health;
    s16 introState;
    s16 timer;
    s16 inhaleTime;
    s16 fireTime;
    s16 corner;
    s16 direction;
    s16 wallHits;
    s16 rollAngle;
} AnchorDodongoBoss;

void Anchor_MarkDodongoBomb(struct Actor* actor);
void Anchor_RefreshBeamosBeam(void);
s32 Anchor_UpdateDodongoBombReplica(struct Actor* actor, struct PlayState* play, f32* flash);
void BgDdanJd_GetAnchorMotion(struct Actor* actor, AnchorDodongoPlatform* motion);
s32 Anchor_ReadDodongoPlatform(struct Actor* actor, AnchorDodongoPlatform* motion, u64* seen);
s32 Anchor_ReadBeamosBeam(struct Actor* actor, AnchorBeamosBeam* beam);
s32 BossDodongo_GetAnchorState(struct Actor* actor, AnchorDodongoBoss* state);
s32 Anchor_ReadDodongoBoss(struct Actor* actor, AnchorDodongoBoss* state);

#ifdef __cplusplus
}
#endif
