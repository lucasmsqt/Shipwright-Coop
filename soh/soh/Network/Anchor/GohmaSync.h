#pragma once

#include "z64math.h"

struct Actor;
struct BossGoma;

enum GohmaTeamPhase {
    GOHMA_TEAM_ENCOUNTER,
    GOHMA_TEAM_FLOOR_MAIN,
    GOHMA_TEAM_FLOOR_IDLE,
    GOHMA_TEAM_FLOOR_LAND,
    GOHMA_TEAM_FLOOR_LAND_STRUCK_DOWN,
    GOHMA_TEAM_FLOOR_STUNNED,
    GOHMA_TEAM_FLOOR_DAMAGED,
    GOHMA_TEAM_ATTACK_POSTURE,
    GOHMA_TEAM_PREPARE_ATTACK,
    GOHMA_TEAM_ATTACK,
    GOHMA_TEAM_WALL_CLIMB,
    GOHMA_TEAM_CEILING_CENTER,
    GOHMA_TEAM_CEILING_IDLE,
    GOHMA_TEAM_PREPARE_EGGS,
    GOHMA_TEAM_LAY_EGGS,
    GOHMA_TEAM_FALL_JUMP,
    GOHMA_TEAM_FALL_STRUCK_DOWN,
    GOHMA_TEAM_DEFEATED,
    GOHMA_TEAM_PHASE_COUNT,
};

typedef struct GohmaTeamState {
    Vec3f position;
    Vec3s rotation;
    f32 speed;
    f32 verticalSpeed;
    f32 gravity;
    f32 animationFrame;
    f32 animationSpeed;
    s16 phase;
    s16 actionState;
    s16 frameCount;
    s16 actionTimer;
    s16 timer;
    s16 patience;
    s16 eyeClosed;
    s16 eyeState;
    s16 visualState;
    s16 children[3];
    u16 childGenerations[3];
} GohmaTeamState;

#ifdef __cplusplus
extern "C" {
#endif

void BossGoma_GetTeamState(const struct BossGoma* boss, GohmaTeamState* state);
s32 Anchor_ReadGohmaState(struct Actor* actor, GohmaTeamState* state, u64* seen);
u32 Anchor_GetGohmaChildDefeats(const GohmaTeamState* state);
s32 Anchor_UseSharedGohmaPortal(void);
s32 Anchor_GetGohmaWarpRequest(void);
void Anchor_StartGohmaWarp(s32 story);
void Anchor_CommitGohmaWarp(void);

#ifdef __cplusplus
}
#endif
