#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/CombatSync.h"
#include "soh/Network/Anchor/JsonConversions.hpp"
#include "soh/ObjectExtension/ActorListIndex.h"
#include "soh/ObjectExtension/ObjectExtension.h"
#include "soh/ResourceManagerHelpers.h"
#include <algorithm>
#include <bit>
#include <cmath>

extern "C" {
#include "functions.h"
#include "macros.h"
#include "overlays/actors/ovl_En_Arrow/z_en_arrow.h"
#include "overlays/actors/ovl_En_Dekunuts/z_en_dekunuts.h"
#include "overlays/actors/ovl_En_Ex_Ruppy/z_en_ex_ruppy.h"
#include "overlays/actors/ovl_En_G_Switch/z_en_g_switch.h"
#include "overlays/actors/ovl_En_Rd/z_en_rd.h"
#include "overlays/actors/ovl_En_Skb/z_en_skb.h"
#include "overlays/actors/ovl_En_Syateki_Itm/z_en_syateki_itm.h"
#include "overlays/actors/ovl_En_St/z_en_st.h"
#include "overlays/actors/ovl_En_Sw/z_en_sw.h"
extern PlayState* gPlayState;
}

namespace {
constexpr size_t maxEnemyPoses = 64;
constexpr size_t maxEnemyHits = 64;
constexpr int firstStalchildIndex = 0x4000;
constexpr size_t maxSharedStalchildren = 8;

bool IsValidWorldPosition(const Vec3f& position) {
    const auto valid = [](float value) { return std::isfinite(value) && std::abs(value) < 100000.0f; };
    return valid(position.x) && valid(position.y) && valid(position.z);
}

bool IsSharedStalchild(const Actor* actor) {
    return actor->id == ACTOR_EN_SKB && GetActorListIndex(actor) >= firstStalchildIndex;
}

// These actors already have their own synchronization, or use a multipart/scripted encounter.
bool IsGenericEnemy(const Actor* actor) {
    if (actor->category != ACTORCAT_ENEMY || !(actor->flags & ACTOR_FLAG_HOSTILE)) {
        return false;
    }
    switch (actor->id) {
        case ACTOR_EN_DEKUBABA:
        case ACTOR_EN_KAREBABA:
        case ACTOR_EN_SW:
        case ACTOR_EN_ST:
        case ACTOR_EN_RD:
        case ACTOR_EN_DEKUNUTS:
        case ACTOR_EN_HINTNUTS:
        case ACTOR_EN_SHOPNUTS:
            return false;
        default:
            return true;
    }
}

std::array<int, 4> PoseKey(const Actor* actor, bool gold) {
    if (actor->id == ACTOR_EN_RD && GetActorListIndex(actor) < 0) {
        return { actor->id, static_cast<int>(actor->home.pos.x), static_cast<int>(actor->home.pos.y),
                 static_cast<int>(actor->home.pos.z) };
    }
    return gold ? std::array<int, 4>{ actor->id, actor->params & 0x1FFF, 0, 0 }
                : std::array<int, 4>{ actor->id, GetActorListIndex(actor), 0, 2 };
}

struct EnemySyncData {
    std::chrono::steady_clock::time_point appliedPose{};
    // Native reactions can retain these pointers after the collision pass.
    std::map<std::pair<int, int>, ColliderQuad> meleeAttacks;
    std::map<std::pair<int, int>, EnArrow> arrowAttacks;
};
ObjectExtension::Register<EnemySyncData> enemySyncDataRegister;

EnemySyncData* GetEnemySyncData(Actor* actor) {
    auto& extensions = ObjectExtension::GetInstance();
    if (!extensions.Has<EnemySyncData>(actor)) {
        extensions.Set(actor, EnemySyncData{});
    }
    return extensions.Get<EnemySyncData>(actor);
}

ColliderInfo* ColliderElement(Collider* collider, int index) {
    switch (collider->shape) {
        case COLSHAPE_JNTSPH: {
            auto* spheres = reinterpret_cast<ColliderJntSph*>(collider);
            return spheres->elements != nullptr && index < spheres->count ? &spheres->elements[index].info : nullptr;
        }
        case COLSHAPE_TRIS: {
            auto* tris = reinterpret_cast<ColliderTris*>(collider);
            return tris->elements != nullptr && index < tris->count ? &tris->elements[index].info : nullptr;
        }
        case COLSHAPE_CYLINDER:
            return index == 0 ? &reinterpret_cast<ColliderCylinder*>(collider)->info : nullptr;
        case COLSHAPE_QUAD:
            return index == 0 ? &reinterpret_cast<ColliderQuad*>(collider)->info : nullptr;
        default:
            return nullptr;
    }
}
} // namespace

extern "C" bool Anchor_ApplyGoldSkulltulaPose(Actor* actor) {
    auto* anchor = Anchor::Instance;
    return anchor != nullptr && anchor->isConnected && anchor->ApplyEnemyPose(actor, true);
}

extern "C" s32 Anchor_ApplySkulltulaPose(Actor* actor) {
    auto* anchor = Anchor::Instance;
    int phase = ENST_TEAM_PHASE_NONE;
    if (anchor != nullptr && anchor->isConnected) {
        anchor->ApplyEnemyPose(actor, false, &phase);
    }
    return phase;
}

extern "C" bool Anchor_FindSkulltulaTarget(Actor* actor, s16* yaw) {
    auto* anchor = Anchor::Instance;
    return anchor != nullptr && anchor->isConnected && anchor->FindSkulltulaTarget(actor, yaw);
}

extern "C" void Anchor_SendEnemyHit(PlayState* play, Collider* defender, ColliderInfo* element) {
    auto* anchor = Anchor::Instance;
    if (anchor != nullptr && anchor->isConnected) {
        anchor->SendEnemyHit(play, defender, element);
    }
}

extern "C" void Anchor_ApplyEnemyHits(PlayState* play, CollisionCheckContext* collisions) {
    auto* anchor = Anchor::Instance;
    if (anchor != nullptr && anchor->isConnected) {
        anchor->ApplyEnemyHits(play, collisions);
    }
}

extern "C" void Anchor_SendPlayerArrow(Actor* arrow, bool end) {
    auto* anchor = Anchor::Instance;
    if (anchor != nullptr && anchor->isConnected) {
        anchor->SendPlayerArrow(arrow, end);
    }
}

void Anchor::AddEnemyPoseSnapshot(nlohmann::json& payload) {
    const auto area = localWorldPeer.GetArea();
    const auto now = std::chrono::steady_clock::now();
    enemyPoseAuthority = ownClientId;
    bool hasPeers = false;
    for (const auto& [id, peer] : worldPeers) {
        if (peer.syncCombat && peer.GetArea() == area && now - peer.updated < std::chrono::seconds(2) &&
            IsSharedTeamMember(id)) {
            hasPeers = true;
            if (id == roomState.ownerClientId || (enemyPoseAuthority != roomState.ownerClientId && id < enemyPoseAuthority)) {
                enemyPoseAuthority = id;
            }
        }
    }
    const bool regularPoses = enemyPoseAuthority == ownClientId && !Play_InCsMode(gPlayState);
    const bool locallyGrabbed = (GET_PLAYER(gPlayState)->stateFlags2 & PLAYER_STATE2_GRABBED_BY_ENEMY) != 0;
    const bool meadow = gPlayState->sceneNum == SCENE_SACRED_FOREST_MEADOW && !Play_InCsMode(gPlayState);
    if (!hasPeers || (!regularPoses && !locallyGrabbed && !meadow)) {
        return;
    }
    auto poses = nlohmann::json::array();
    for (int category : { ACTORCAT_NPC, ACTORCAT_ENEMY }) {
        for (Actor* actor = gPlayState->actorCtx.actorLists[category].head; actor != nullptr && poses.size() < maxEnemyPoses;
             actor = actor->next) {
            if ((!regularPoses && actor->id != ACTOR_EN_RD && !(meadow && actor->id == ACTOR_EN_DEKUNUTS)) ||
                actor->room != localWorldPeer.room ||
                actor->init != nullptr || actor->update == nullptr ||
                actor->colChkInfo.health == 0 || IsSharedStalchild(actor)) {
                continue;
            }
            const bool gold = EnSw_IsOnGoldWall(actor);
            const int phase = actor->id == ACTOR_EN_ST ? EnSt_GetTeamPhase(reinterpret_cast<const EnSt*>(actor))
                                                       : ENST_TEAM_PHASE_NONE;
            if (meadow && actor->id == ACTOR_EN_DEKUNUTS && GetActorListIndex(actor) >= 0) {
                ScrubMotion motion{};
                if (EnDekunuts_GetTeamMotion(actor, &motion)) {
                    poses.push_back({ PoseKey(actor, false), actor->world.pos, actor->world.rot, actor->shape.rot,
                                      actor->scale.x, motion.phase, { motion.timer, motion.frame, motion.speed,
                                                                     motion.height, motion.attackable } });
                }
            } else if (actor->id == ACTOR_EN_RD) {
                RedeadMotion motion{};
                EnRd_GetTeamMotion(reinterpret_cast<const EnRd*>(actor), &motion);
                if (!regularPoses && !motion.localGrab) {
                    continue;
                }
                poses.push_back({ PoseKey(actor, false), actor->world.pos, actor->world.rot, actor->shape.rot,
                                  actor->scale.x, -1, { motion.frame, motion.yOffset, motion.headYaw, motion.torsoYaw,
                                                       motion.sunFrames, motion.action, motion.animation,
                                                       motion.localGrab != 0 } });
            } else if (gold || ((IsGenericEnemy(actor) || phase != ENST_TEAM_PHASE_NONE) && GetActorListIndex(actor) >= 0)) {
                poses.push_back({ PoseKey(actor, gold), actor->world.pos, actor->world.rot, actor->shape.rot,
                                  actor->scale.x, phase });
            }
        }
    }
    if (!poses.empty()) {
        payload["enemyPoses"] = std::move(poses);
    }
}

void Anchor::ReadEnemyPoseSnapshot(const nlohmann::json& payload, WorldPeer& peer) {
    const auto poses = payload.find("enemyPoses");
    if (poses == payload.end() || !poses->is_array() || poses->size() > maxEnemyPoses) {
        return;
    }
    for (const auto& entry : *poses) {
        EnemyPose pose{ entry.at(1).get<Vec3f>(), entry.at(2).get<Vec3s>(), entry.at(3).get<Vec3s>(),
                        entry.at(4).get<float>() };
        if (entry.size() > 5) {
            pose.phase = entry.at(5).get<int>();
        }
        const auto key = entry.at(0).get<EnemyKey>();
        if (key[0] == ACTOR_EN_RD && entry.size() == 7) {
            const auto& data = entry.at(6);
            RedeadMotion motion{ data.at(0).get<float>(), data.at(1).get<float>(), data.at(2).get<s16>(),
                                 data.at(3).get<s16>(), data.at(4).get<s16>(), data.at(5).get<s16>(),
                                 data.at(6).get<s16>(), static_cast<s16>(data.size() > 7 && data.at(7).get<bool>()) };
            if (std::isfinite(motion.frame) && motion.frame >= 0.0f && motion.frame < 200.0f &&
                std::isfinite(motion.yOffset) && std::abs(motion.yOffset) <= 5000.0f &&
                motion.action >= 0 && motion.action <= 11 && motion.sunFrames >= 0 && motion.sunFrames <= 600 &&
                motion.animation >= 0 && motion.animation <= 10) {
                pose.redead = motion;
            }
        }
        if (key[0] == ACTOR_EN_DEKUNUTS && entry.size() == 7 && pose.phase >= 0 && pose.phase < 10) {
            const auto& data = entry.at(6);
            ScrubMotion motion{ pose.phase, data.at(0).get<int>(), data.at(1).get<float>(), data.at(2).get<float>(),
                               data.at(3).get<s16>(), data.at(4).get<s16>() };
            if (motion.timer >= 0 && motion.timer <= INT16_MAX && std::isfinite(motion.frame) && motion.frame >= 0 &&
                motion.frame < 100 && std::isfinite(motion.speed) && std::abs(motion.speed) <= 4.0f &&
                motion.height >= 0 && motion.height <= 50 && (motion.attackable == 0 || motion.attackable == 1)) {
                pose.scrub = motion;
            }
        }
        if (IsValidWorldPosition(pose.position) &&
            std::isfinite(pose.scale) && pose.scale >= 0.0f && pose.scale <= 1.0f) {
            peer.enemyPoses.emplace(key, pose);
        }
    }
}

bool Anchor::ApplyEnemyPose(Actor* actor, bool gold, int* skulltulaPhase, RedeadMotion* redead) {
    if (enemyPoseAuthority == 0 || (enemyPoseAuthority == ownClientId && redead == nullptr) ||
        actor->colChkInfo.health == 0 ||
        actor->room != localWorldPeer.room || gPlayState->sceneNum != localWorldPeer.scene ||
        gPlayState->roomCtx.curRoom.num != localWorldPeer.room ||
        (!gold && skulltulaPhase == nullptr && redead == nullptr && (!IsGenericEnemy(actor) || IsSharedStalchild(actor)))) {
        return false;
    }
    const auto key = PoseKey(actor, gold);
    const auto now = std::chrono::steady_clock::now();
    auto peer = worldPeers.find(enemyPoseAuthority);
    if (redead != nullptr) {
        for (auto other = worldPeers.begin(); other != worldPeers.end(); ++other) {
            if (other->second.GetArea() != localWorldPeer.GetArea() || now - other->second.updated >= std::chrono::seconds(1) ||
                !IsSharedTeamMember(other->first)) {
                continue;
            }
            const auto pose = other->second.enemyPoses.find(key);
            if (pose != other->second.enemyPoses.end() && pose->second.redead && pose->second.redead->localGrab) {
                peer = other;
                break;
            }
        }
    }
    if (peer == worldPeers.end() || peer->second.GetArea() != localWorldPeer.GetArea() ||
        peer->second.enemyPoses.empty() || !IsSharedTeamMember(peer->first)) {
        return false;
    }
    const auto sample = peer->second.enemyPoses.find(key);
    if (sample == peer->second.enemyPoses.end() || now - peer->second.updated >= std::chrono::seconds(1) ||
        Play_InCsMode(gPlayState)) {
        return false;
    }
    if (redead != nullptr) {
        if (!sample->second.redead) {
            return false;
        }
        *redead = *sample->second.redead;
    } else if (skulltulaPhase != nullptr) {
        if (sample->second.phase < ENST_TEAM_PHASE_CEILING || sample->second.phase >= ENST_TEAM_PHASE_MAX) {
            return false;
        }
        *skulltulaPhase = sample->second.phase;
    } else if (!gold) {
        auto* state = GetEnemySyncData(actor);
        if (state->appliedPose == peer->second.updated) {
            return false;
        }
        state->appliedPose = peer->second.updated;
    }
    actor->world.pos = sample->second.position;
    actor->world.rot = sample->second.worldRot;
    actor->shape.rot = sample->second.shapeRot;
    if (gold) {
        Actor_SetScale(actor, sample->second.scale);
    }
    return true;
}

extern "C" s32 Anchor_ApplyRedeadPose(Actor* actor, RedeadMotion* motion) {
    auto* anchor = Anchor::Instance;
    return anchor != nullptr && anchor->isConnected && anchor->ApplyEnemyPose(actor, false, nullptr, motion);
}

Actor* Anchor::FindRedeadTarget(Actor* actor) {
    if (!isConnected || !CanShareCombat() || actor->room != gPlayState->roomCtx.curRoom.num) {
        return nullptr;
    }
    const auto now = std::chrono::steady_clock::now();
    float closest = std::min(actor->xyzDistToPlayerSq, 150.0f * 150.0f);
    Actor* target = nullptr;
    for (const auto& [id, client] : clients) {
        if (client.player == nullptr || client.player->actor.update == nullptr || client.sceneNum != gPlayState->sceneNum ||
            client.curRoomNum != actor->room || client.linkAge != gSaveContext.linkAge ||
            now - client.lastPlayerUpdate >= std::chrono::seconds(1) || !IsSharedTeamMember(id)) {
            continue;
        }
        const float dx = client.posRot.pos.x - actor->world.pos.x;
        const float dy = client.posRot.pos.y - actor->world.pos.y;
        const float dz = client.posRot.pos.z - actor->world.pos.z;
        const float distance = dx * dx + dy * dy + dz * dz;
        if (distance < closest) {
            closest = distance;
            target = &client.player->actor;
        }
    }
    return target;
}

extern "C" Actor* Anchor_FindRedeadTarget(Actor* actor) {
    return Anchor::Instance == nullptr ? nullptr : Anchor::Instance->FindRedeadTarget(actor);
}

bool Anchor::ApplyScrubPose(Actor* actor, ScrubMotion* motion) {
    if (!isConnected || gPlayState == nullptr || gPlayState->sceneNum != SCENE_SACRED_FOREST_MEADOW ||
        actor->colChkInfo.health == 0 || worldPeers.empty()) {
        return false;
    }
    const auto controller = FindNearestCombatController(actor);
    if (controller == ownClientId) {
        return false;
    }
    const auto peer = worldPeers.find(controller);
    if (peer == worldPeers.end() || std::chrono::steady_clock::now() - peer->second.updated >= std::chrono::seconds(1)) {
        return false;
    }
    const auto pose = peer->second.enemyPoses.find(PoseKey(actor, false));
    if (pose == peer->second.enemyPoses.end() || !pose->second.scrub) {
        return false;
    }
    *motion = *pose->second.scrub;
    actor->world.pos = pose->second.position;
    actor->world.rot = pose->second.worldRot;
    actor->shape.rot = pose->second.shapeRot;
    return true;
}

extern "C" s32 Anchor_ApplyScrubPose(Actor* actor, ScrubMotion* motion) {
    auto* anchor = Anchor::Instance;
    return anchor != nullptr && anchor->ApplyScrubPose(actor, motion);
}

bool Anchor::FindSkulltulaTarget(Actor* actor, s16* yaw) {
    if (!CanShareCombat() || actor->room != gPlayState->roomCtx.curRoom.num) {
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    float closest = 160.0f * 160.0f;
    const Vec3f* target = nullptr;
    for (const auto& [id, client] : clients) {
        if (client.sceneNum != gPlayState->sceneNum || client.curRoomNum != actor->room ||
            now - client.lastPlayerUpdate >= std::chrono::seconds(2) || !IsSharedTeamMember(id)) {
            continue;
        }
        const auto& pos = client.posRot.pos;
        const float dx = actor->world.pos.x - pos.x;
        const float dz = actor->world.pos.z - pos.z;
        const float dy = actor->world.pos.y - pos.y;
        const float distance = dx * dx + dz * dz;
        if (distance <= closest && dy >= 0.0f && dy <= 400.0f && pos.y >= actor->floorHeight) {
            closest = distance;
            target = &pos;
        }
    }
    if (target != nullptr && yaw != nullptr) {
        Vec3f pos = *target;
        *yaw = Math_Vec3f_Yaw(&actor->world.pos, &pos);
    }
    return target != nullptr;
}

void Anchor::SendEnemyHit(PlayState* play, Collider* defender, ColliderInfo* element) {
    Actor* actor = defender->actor;
    const auto* attack = element->acHit;
    const auto* attackInfo = element->acHitInfo;
    if (!IsGenericEnemy(actor) || actor->id == ACTOR_EN_WF || actor->room != play->roomCtx.curRoom.num ||
        (defender->acFlags & AC_HARD) ||
        attack == nullptr || attack->actor == nullptr || attack->shape != COLSHAPE_QUAD || attackInfo == nullptr ||
        !std::has_single_bit(attackInfo->toucher.dmgFlags) || !CanShareCombat()) {
        return;
    }
    const bool arrow = attack->actor->id == ACTOR_EN_ARROW;
    if (arrow) {
        if (!reinterpret_cast<const EnArrow*>(attack->actor)->fromLocalLink ||
            !EnArrow_IsSharedType(attack->actor->params)) {
            return;
        }
    } else if (attack->actor != (Actor*)GET_PLAYER(play)) {
        return;
    }
    const int index = GetActorListIndex(actor);
    if (index < 0) {
        return;
    }
    int colliderSlot = 0;
    for (int i = 0; i < play->colChkCtx.colACCount && play->colChkCtx.colAC[i] != defender; ++i) {
        const auto* prior = play->colChkCtx.colAC[i];
        if (prior != nullptr && prior->actor == actor && prior->shape == defender->shape) {
            ++colliderSlot;
        }
    }
    int elementSlot = 0;
    for (; elementSlot < 64 && ColliderElement(defender, elementSlot) != element; ++elementSlot) {}
    if (elementSlot == 64) {
        return;
    }
    const auto& touch = attackInfo->toucher;
    nlohmann::json payload = { { "type", arrow ? ENEMY_ARROW_HIT : ENEMY_MELEE_HIT }, { "quiet", true },
                               { "targetTeamId", CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default") },
                               { "scene", play->sceneNum }, { "room", actor->room }, { "age", gSaveContext.linkAge },
                               { "masterQuest", ResourceMgr_IsSceneMasterQuest(play->sceneNum) != 0 },
                               { "target", { actor->id, index, defender->shape, colliderSlot, elementSlot } },
                               { "attack", { touch.dmgFlags, touch.effect, touch.damage } },
                               { "position", element->bumper.hitPos } };
    if (arrow) {
        payload["arrowType"] = attack->actor->params;
        payload["arrowPose"] = attack->actor->world;
    }
    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_EnemyHit(const nlohmann::json& payload, bool arrow) {
    const auto sender = payload.at("clientId").get<uint32_t>();
    if (!CanShareCombat() || !IsSharedTeamMember(sender) || enemyHits.size() >= maxEnemyHits) {
        return;
    }
    const NpcArea area{ payload.at("scene").get<int>(), payload.at("room").get<int>(), payload.at("age").get<int>(),
                        payload.at("masterQuest").get<bool>() };
    if (area != localWorldPeer.GetArea()) {
        return;
    }
    const auto target = payload.at("target").get<std::array<int, 5>>();
    const auto& attack = payload.at("attack");
    const ColliderTouch touch{ attack.at(0).get<u32>(), attack.at(1).get<u8>(), attack.at(2).get<u8>() };
    const int arrowType = arrow ? payload.at("arrowType").get<int>() : -1;
    const auto arrowPose = arrow ? payload.at("arrowPose").get<PosRot>() : PosRot{};
    if (arrow && (arrowType < ARROW_NORMAL_LIT || arrowType > ARROW_SEED ||
                  !EnArrow_IsSharedType(static_cast<s16>(arrowType)) || !IsValidWorldPosition(arrowPose.pos))) {
        return;
    }
    if (target[1] >= 0 && target[2] >= COLSHAPE_JNTSPH && target[2] <= COLSHAPE_QUAD && target[3] >= 0 &&
        target[3] < COLLISION_CHECK_AC_MAX && target[4] >= 0 && target[4] < 64 && std::has_single_bit(touch.dmgFlags)) {
        enemyHits.push_back({ sender, area, target[0], target[1], target[2], target[3], target[4], touch,
                              payload.at("position").get<Vec3s>(), arrowType, arrowPose,
                              std::chrono::steady_clock::now() + std::chrono::milliseconds(750) });
    }
}

void Anchor::ApplyEnemyHits(PlayState* play, CollisionCheckContext* collisions) {
    if (enemyHits.empty()) {
        return;
    }
    const NpcArea area{ play->sceneNum, play->roomCtx.curRoom.num, gSaveContext.linkAge,
                        ResourceMgr_IsSceneMasterQuest(play->sceneNum) != 0 };
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(enemyHits, [&](const EnemyHit& hit) {
        if (hit.area != area || now >= hit.expires || !IsSharedTeamMember(hit.sender)) {
            return true;
        }
        auto* attacker = clients.at(hit.sender).player;
        if (hit.arrowType < 0 && (attacker == nullptr || attacker->actor.update == nullptr)) {
            return false;
        }
        int slot = 0;
        for (int i = 0; i < collisions->colACCount; ++i) {
            Collider* defender = collisions->colAC[i];
            if (defender == nullptr || defender->shape != hit.shape || defender->actor == nullptr) {
                continue;
            }
            Actor* actor = defender->actor;
            if (actor->room != play->roomCtx.curRoom.num || actor->id != hit.actorId || actor->id == ACTOR_EN_WF ||
                !IsGenericEnemy(actor) ||
                GetActorListIndex(actor) != hit.actorIndex || slot++ != hit.colliderSlot) {
                continue;
            }
            auto* element = ColliderElement(defender, hit.elementSlot);
            if (actor->update == nullptr || (defender->acFlags & (AC_HARD | AC_HIT)) || !(defender->acFlags & AC_ON) ||
                !(defender->acFlags & AC_TYPE_PLAYER) || element == nullptr || !(element->bumperFlags & BUMP_ON) ||
                !(element->bumper.dmgFlags & hit.touch.dmgFlags)) {
                break;
            }
            auto* state = GetEnemySyncData(actor);
            const auto key = std::make_pair(hit.colliderSlot * 4 + hit.shape, hit.elementSlot);
            ColliderQuad* source;
            if (hit.arrowType >= 0) {
                // A typed collision source keeps the original impact identity without running a second projectile.
                auto& arrow = state->arrowAttacks[key];
                arrow.actor.id = ACTOR_EN_ARROW;
                arrow.actor.params = static_cast<s16>(hit.arrowType);
                arrow.actor.world = hit.arrowPose;
                source = &arrow.collider;
                source->base.actor = &arrow.actor;
            } else {
                source = &state->meleeAttacks[key];
                source->base.actor = &attacker->actor;
            }
            auto& attack = *source;
            attack.base.atFlags = AT_ON | AT_TYPE_PLAYER;
            attack.base.shape = COLSHAPE_QUAD;
            attack.info.toucher = hit.touch;
            defender->acFlags |= AC_HIT;
            defender->ac = attack.base.actor;
            element->bumperFlags |= BUMP_HIT;
            element->bumper.hitPos = hit.position;
            element->acHit = &attack.base;
            element->acHitInfo = &attack.info;
            actor->colChkInfo.acHitEffect = hit.touch.effect;
            return true;
        }
        return false;
    });
}

void Anchor::SendPlayerArrow(Actor* actor, bool end) {
    if ((!CanShareCombat() && !(gPlayState->sceneNum == SCENE_SHOOTING_GALLERY && CanShareRoomObjects())) ||
        ownClientId == 0) {
        return;
    }
    // Arrows held by Link inherit his global actor room. Route the shot through the current room.
    const int room = gPlayState->roomCtx.curRoom.num;
    if (actor->room >= 0 && actor->room != room) {
        return;
    }
    auto* arrow = reinterpret_cast<EnArrow*>(actor);
    if (!end) {
        arrow->anchorShot = ++playerArrowSerial;
    }
    nlohmann::json payload = { { "type", PLAYER_ARROW }, { "quiet", true },
                               { "targetTeamId", CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default") },
                               { "scene", gPlayState->sceneNum }, { "room", room }, { "age", gSaveContext.linkAge },
                               { "masterQuest", ResourceMgr_IsSceneMasterQuest(gPlayState->sceneNum) != 0 },
                               { "shot", arrow->anchorShot }, { "end", end } };
    if (!end) {
        payload["params"] = actor->params;
        payload["pose"] = actor->world;
    }
    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_PlayerArrow(const nlohmann::json& payload) {
    const auto sender = payload.at("clientId").get<uint32_t>();
    if ((!CanShareCombat() && !(gPlayState->sceneNum == SCENE_SHOOTING_GALLERY && CanShareRoomObjects())) ||
        !IsSharedTeamMember(sender) || payload.at("scene").get<int>() != gPlayState->sceneNum ||
        payload.at("room").get<int>() != gPlayState->roomCtx.curRoom.num || payload.at("age").get<int>() != gSaveContext.linkAge ||
        payload.at("masterQuest").get<bool>() != (ResourceMgr_IsSceneMasterQuest(gPlayState->sceneNum) != 0)) {
        return;
    }
    const auto shot = payload.at("shot").get<uint32_t>();
    const bool end = payload.at("end").get<bool>();
    if (shot == 0) {
        return;
    }
    for (Actor* actor = gPlayState->actorCtx.actorLists[ACTORCAT_ITEMACTION].head; actor != nullptr; actor = actor->next) {
        if (actor->id == ACTOR_EN_ARROW && actor->room == gPlayState->roomCtx.curRoom.num && actor->update != nullptr) {
            auto* arrow = reinterpret_cast<EnArrow*>(actor);
            if (arrow->anchorReplica && arrow->anchorOwner == sender && arrow->anchorShot == shot) {
                if (end && (gPlayState->sceneNum != SCENE_SHOOTING_GALLERY || actor->init == nullptr)) {
                    Actor_Kill(actor);
                }
                return;
            }
        }
    }
    if (end) {
        return;
    }
    const int params = payload.at("params").get<int>();
    const auto pose = payload.at("pose").get<PosRot>();
    if (params < ARROW_NORMAL_LIT || params > ARROW_SEED || !EnArrow_IsSharedType(static_cast<s16>(params)) ||
        !IsValidWorldPosition(pose.pos)) {
        return;
    }
    auto* actor = Actor_Spawn(&gPlayState->actorCtx, gPlayState, ACTOR_EN_ARROW, pose.pos.x, pose.pos.y, pose.pos.z,
                              pose.rot.x, pose.rot.y, pose.rot.z, static_cast<s16>(params));
    if (actor != nullptr) {
        EnArrow_StartReplica(reinterpret_cast<EnArrow*>(actor), sender, shot);
    }
}

uint32_t Anchor::GetStalchildAuthority() const {
    const auto now = std::chrono::steady_clock::now();
    const auto area = localWorldPeer.GetArea();
    const auto eligible = [&](uint32_t id, const WorldPeer& peer) {
        return peer.GetArea() == area && peer.syncCombat && now - peer.updated < std::chrono::seconds(2) &&
               IsSharedTeamMember(id);
    };
    auto first = localWorldPeer.entered;
    for (const auto& [id, peer] : worldPeers) {
        if (eligible(id, peer)) {
            first = std::min(first, peer.entered);
        }
    }
    const auto tie = first + std::chrono::milliseconds(500);
    uint32_t authority = localWorldPeer.entered <= tie ? ownClientId : 0;
    for (const auto& [id, peer] : worldPeers) {
        if (eligible(id, peer) && peer.entered <= tie &&
            (authority == 0 || id == roomState.ownerClientId || (authority != roomState.ownerClientId && id < authority))) {
            authority = id;
        }
    }
    return authority;
}

bool Anchor::ShouldSpawnStalchildren() {
    return !isConnected || !CanShareCombat() || localWorldPeer.scene != SCENE_HYRULE_FIELD ||
           GetStalchildAuthority() == ownClientId;
}

void Anchor::RegisterStalchild(Actor* actor) {
    if (isConnected && CanShareCombat()) {
        const auto index = firstStalchildIndex | ((ownClientId * 1009 + ++stalchildSerial) & 0x3FFF);
        SetActorListIndex(actor, static_cast<s16>(index));
    }
}

extern "C" s32 Anchor_ShouldSpawnStalchildren(PlayState* play) {
    return Anchor::Instance == nullptr || play != gPlayState || Anchor::Instance->ShouldSpawnStalchildren();
}

extern "C" void Anchor_RegisterStalchild(PlayState* play, Actor* actor) {
    if (Anchor::Instance != nullptr && play == gPlayState) {
        Anchor::Instance->RegisterStalchild(actor);
    }
}

void Anchor::AddDynamicActorSnapshot(nlohmann::json& payload) {
    const int scene = gPlayState->sceneNum;
    if (scene == SCENE_HYRULE_FIELD && CanShareCombat() && GetStalchildAuthority() == ownClientId) {
        auto entries = nlohmann::json::array();
        for (auto* actor = gPlayState->actorCtx.actorLists[ACTORCAT_ENEMY].head;
             actor != nullptr && entries.size() < maxSharedStalchildren; actor = actor->next) {
            if (!IsSharedStalchild(actor) || actor->update == nullptr || actor->init != nullptr || actor->room != localWorldPeer.room) {
                continue;
            }
            const auto* stalchild = reinterpret_cast<const EnSkb*>(actor);
            if (!stalchild->anchorReplica) {
                entries.push_back({ GetActorListIndex(actor), actor->world.pos, actor->shape.rot, actor->params,
                                    EnSkb_GetTeamPhase(stalchild), actor->colChkInfo.health, actor->shape.yOffset,
                                    stalchild->setColliderAT != 0 });
            }
        }
        payload["stalchildren"] = std::move(entries);
    } else if (scene == SCENE_SHOOTING_GALLERY && gPlayState->shootingGalleryStatus != 0) {
        auto* actor = Actor_Find(&gPlayState->actorCtx, ACTOR_EN_SYATEKI_ITM, ACTORCAT_PROP);
        if (actor == nullptr || actor->init != nullptr || actor->update == nullptr) {
            return;
        }
        const auto* gallery = reinterpret_cast<const EnSyatekiItm*>(actor);
        uint16_t markers = 0;
        for (size_t i = 0; i < std::size(gallery->markers); ++i) {
            if (gallery->markers[i] != nullptr && gallery->markers[i]->galleryFlag) {
                markers |= 1u << i;
            }
        }
        auto targets = nlohmann::json::array();
        for (auto* target = gPlayState->actorCtx.actorLists[ACTORCAT_PROP].head; target != nullptr; target = target->next) {
            if (target->id != ACTOR_EN_G_SWITCH || target->parent != actor || target->init != nullptr || target->update == nullptr) {
                continue;
            }
            const auto* rupee = reinterpret_cast<const EnGSwitch*>(target);
            if (rupee->type == ENGSWITCH_TARGET_RUPEE && !rupee->broken && rupee->delayTimer == 0) {
                targets.push_back({ rupee->index, target->world.pos, rupee->colorIdx, target->shape.rot.y });
            }
        }
        payload["gallery"] = { markers, std::move(targets) };
    }
}

void Anchor::ReadDynamicActorSnapshot(const nlohmann::json& payload, WorldPeer& peer) {
    if (const auto entries = payload.find("stalchildren"); peer.scene == SCENE_HYRULE_FIELD && peer.syncCombat &&
        entries != payload.end() && entries->is_array() && entries->size() <= maxSharedStalchildren) {
        auto& stalchildren = peer.stalchildren.emplace();
        for (const auto& entry : *entries) {
            const auto index = entry.at(0).get<int>();
            StalchildSample sample{ entry.at(1).get<Vec3f>(), entry.at(2).get<Vec3s>(), entry.at(3).get<s16>(),
                                    entry.at(4).get<s16>(), entry.at(5).get<s16>(), entry.at(6).get<float>(),
                                    entry.at(7).get<bool>() };
            if (index >= firstStalchildIndex && index <= INT16_MAX && IsValidWorldPosition(sample.position) &&
                sample.params >= 0 && sample.params <= 100 && sample.phase >= 0 && sample.phase <= 7 &&
                sample.health >= 0 && sample.health <= 2 && std::isfinite(sample.offset) &&
                sample.offset >= -8000.0f && sample.offset <= 0.0f) {
                stalchildren.emplace(index, sample);
            }
        }
    }
    if (const auto entry = payload.find("gallery"); peer.scene == SCENE_SHOOTING_GALLERY && entry != payload.end()) {
        const auto markers = entry->at(0).get<int>();
        const auto& targets = entry->at(1);
        if (markers < 0 || markers > 0x3FF || !targets.is_array() || targets.size() > 2) {
            return;
        }
        auto& gallery = peer.gallery.emplace();
        gallery.markers = markers;
        for (const auto& target : targets) {
            const auto index = target.at(0).get<int>();
            GalleryTarget sample{ target.at(1).get<Vec3f>(), target.at(2).get<s16>(), target.at(3).get<s16>() };
            if (index >= 0 && index < 2 && sample.color >= 0 && sample.color < 5 && IsValidWorldPosition(sample.position)) {
                gallery.targets[index] = sample;
            }
        }
    }
}

void Anchor::UpdateDynamicActors() {
    const auto scene = gPlayState->sceneNum;
    if (scene == SCENE_HYRULE_FIELD && CanShareCombat()) {
        const auto authority = GetStalchildAuthority();
        const auto peer = worldPeers.find(authority);
        const auto* samples = peer != worldPeers.end() && peer->second.stalchildren ? &*peer->second.stalchildren : nullptr;
        if (authority != ownClientId && samples == nullptr) {
            return;
        }
        std::map<int, EnSkb*> replicas;
        for (auto* actor = gPlayState->actorCtx.actorLists[ACTORCAT_ENEMY].head; actor != nullptr; actor = actor->next) {
            if (!IsSharedStalchild(actor) || actor->update == nullptr) {
                continue;
            }
            auto* stalchild = reinterpret_cast<EnSkb*>(actor);
            if (authority == ownClientId) {
                if (stalchild->anchorReplica) {
                    Actor_Kill(actor);
                }
            } else if (!stalchild->anchorReplica) {
                Actor_Kill(actor);
            } else {
                const int index = GetActorListIndex(actor);
                replicas.emplace(index, stalchild);
                if (actor->init == nullptr && !samples->contains(index)) {
                    EnSkb_EndTeamReplica(stalchild, gPlayState);
                }
            }
        }
        if (samples != nullptr) {
            for (const auto& [index, sample] : *samples) {
                auto* stalchild = replicas.contains(index) ? replicas.at(index) : nullptr;
                if (stalchild == nullptr && sample.health != 0) {
                    auto* actor = Actor_Spawn(&gPlayState->actorCtx, gPlayState, ACTOR_EN_SKB, sample.position.x,
                                              sample.position.y, sample.position.z, 0, sample.rotation.y, 0, sample.params);
                    if (actor == nullptr || actor->id != ACTOR_EN_SKB) {
                        continue;
                    }
                    SetActorListIndex(actor, index);
                    stalchild = reinterpret_cast<EnSkb*>(actor);
                    stalchild->anchorReplica = true;
                    actor->flags |= ACTOR_FLAG_UPDATE_CULLING_DISABLED;
                    dynamicReplicasActive = true;
                }
                if (stalchild != nullptr && stalchild->actor.init == nullptr && !stalchild->anchorEnding) {
                    auto& actor = stalchild->actor;
                    EnSkb_ApplyTeamState(stalchild, gPlayState, sample.phase, sample.health, sample.offset, sample.attacking);
                    actor.world.pos = sample.position;
                    actor.world.rot = actor.shape.rot = sample.rotation;
                }
            }
        }
    } else if (scene == SCENE_SHOOTING_GALLERY) {
        const auto now = std::chrono::steady_clock::now();
        const GallerySample* sample = nullptr;
        if (gPlayState->shootingGalleryStatus == 0) {
            for (const auto& [id, peer] : worldPeers) {
                if (peer.gallery && peer.GetArea() == localWorldPeer.GetArea() && now - peer.updated < std::chrono::seconds(1) &&
                    IsSharedTeamMember(id)) {
                    sample = &*peer.gallery;
                    if (id == roomState.ownerClientId) {
                        break;
                    }
                }
            }
        }
        if (sample == nullptr && !gallerySpectating) {
            return;
        }
        std::array<EnGSwitch*, 2> replicas{};
        for (auto* actor = gPlayState->actorCtx.actorLists[ACTORCAT_PROP].head; actor != nullptr; actor = actor->next) {
            if (actor->id != ACTOR_EN_G_SWITCH || actor->update == nullptr) {
                continue;
            }
            auto* target = reinterpret_cast<EnGSwitch*>(actor);
            if (target->anchorSpectator) {
                if (sample != nullptr && sample->targets[target->index]) {
                    replicas[target->index] = target;
                } else {
                    Actor_Kill(actor);
                }
            }
        }
        auto* controller = Actor_Find(&gPlayState->actorCtx, ACTOR_EN_SYATEKI_ITM, ACTORCAT_PROP);
        if (controller != nullptr && controller->init == nullptr && gPlayState->shootingGalleryStatus == 0) {
            auto* gallery = reinterpret_cast<EnSyatekiItm*>(controller);
            for (size_t i = 0; i < std::size(gallery->markers); ++i) {
                if (gallery->markers[i] != nullptr) {
                    gallery->markers[i]->galleryFlag = sample != nullptr && (sample->markers & (1u << i)) != 0;
                }
            }
        }
        gallerySpectating = sample != nullptr;
        if (sample == nullptr) {
            return;
        }
        dynamicReplicasActive = true;
        for (size_t i = 0; i < sample->targets.size(); ++i) {
            if (const auto& entry = sample->targets[i]) {
                if (replicas[i] == nullptr) {
                    auto* actor = Actor_Spawn(&gPlayState->actorCtx, gPlayState, ACTOR_EN_G_SWITCH, entry->position.x,
                                              entry->position.y, entry->position.z, 0, entry->yaw, 0,
                                              (ENGSWITCH_TARGET_RUPEE << 12) | 0x3F);
                    if (actor == nullptr || actor->id != ACTOR_EN_G_SWITCH) {
                        continue;
                    }
                    replicas[i] = reinterpret_cast<EnGSwitch*>(actor);
                    replicas[i]->anchorSpectator = true;
                    replicas[i]->index = static_cast<s16>(i);
                }
                replicas[i]->actor.world.pos = entry->position;
                replicas[i]->actor.shape.rot.y = entry->yaw;
                replicas[i]->colorIdx = entry->color;
            }
        }
    }
}

void Anchor::ClearDynamicActors() {
    if (dynamicReplicasActive && IsSaveLoaded()) {
        for (int category : { ACTORCAT_ENEMY, ACTORCAT_PROP }) {
            for (auto* actor = gPlayState->actorCtx.actorLists[category].head; actor != nullptr; actor = actor->next) {
                if ((actor->id == ACTOR_EN_SKB && reinterpret_cast<const EnSkb*>(actor)->anchorReplica) ||
                    (actor->id == ACTOR_EN_G_SWITCH && reinterpret_cast<const EnGSwitch*>(actor)->anchorSpectator)) {
                    Actor_Kill(actor);
                }
            }
        }
    }
    dynamicReplicasActive = false;
    gallerySpectating = false;
}
