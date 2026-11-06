#pragma once

#ifdef __cplusplus
extern "C" {
#endif

bool Anchor_ApplyGoldSkulltulaPose(struct Actor* actor);
s32 Anchor_ApplySkulltulaPose(struct Actor* actor);
bool Anchor_FindSkulltulaTarget(struct Actor* actor, s16* yaw);
void Anchor_SendEnemyHit(struct PlayState* play, Collider* defender, ColliderInfo* element);
void Anchor_ApplyEnemyHits(struct PlayState* play, CollisionCheckContext* collisions);
void Anchor_SendPlayerArrow(struct Actor* arrow, bool end);

#ifdef __cplusplus
}
#endif
