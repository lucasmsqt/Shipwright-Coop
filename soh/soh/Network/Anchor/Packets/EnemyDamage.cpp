#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/JsonConversions.hpp"
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/ObjectExtension/ActorListIndex.h"
#include "soh/ResourceManagerHelpers.h"
#include <algorithm>
#include <cmath>
#include <utility>

extern "C" {
extern PlayState* gPlayState;
}

namespace {
bool IsSharedEnemy(int id) {
    return id == ACTOR_EN_DEKUBABA || id == ACTOR_EN_KAREBABA || id == ACTOR_BOSS_GOMA || id == ACTOR_EN_SW ||
           id == ACTOR_EN_ST || id == ACTOR_EN_RD || id == ACTOR_EN_WF || id == ACTOR_EN_DEKUNUTS ||
           id == ACTOR_BOSS_DODONGO;
}

bool IsBaba(int id) {
    return id == ACTOR_EN_DEKUBABA || id == ACTOR_EN_KAREBABA;
}

bool IsGohmaPhase(int reaction) {
    return reaction >= GI_ENEMY_HIT_GOHMA_SEED_STUN && reaction <= GI_ENEMY_HIT_GOHMA_FALL;
}

std::array<int, 4> GetEnemyKey(const Actor* actor) {
    if (IsBaba(actor->id) || actor->id == ACTOR_EN_ST) {
        return { actor->id, GetActorListIndex(actor), 0, 1 };
    }
    if (actor->id == ACTOR_EN_RD && GetActorListIndex(actor) >= 0) {
        return { actor->id, GetActorListIndex(actor), 0, 3 };
    }
    if ((actor->id == ACTOR_EN_WF || actor->id == ACTOR_EN_DEKUNUTS) && GetActorListIndex(actor) >= 0) {
        return { actor->id, GetActorListIndex(actor), 0, 3 };
    }
    if (actor->id == ACTOR_EN_SW) {
        if ((actor->params & 0xE000) != 0) {
            // The token's scene and bit identify even spiders spawned from soil or trees.
            return { actor->id, actor->params & 0x1FFF, 0, 0 };
        }
        // Wall spiders adjust their home coordinates to fit the local collision geometry.
        return { actor->id, GetActorListIndex(actor), 0, 1 };
    }
    return { actor->id, static_cast<int>(actor->home.pos.x), static_cast<int>(actor->home.pos.y),
             static_cast<int>(actor->home.pos.z) };
}

bool IsEligibleEnemy(Actor* actor) {
    if (actor->id == ACTOR_EN_SW && (actor->params & 0xE000) != 0) {
        return true;
    }
    return IsSharedEnemy(actor->id) && (actor->id == ACTOR_EN_RD || actor->id == ACTOR_EN_WF ||
                                      actor->id == ACTOR_BOSS_DODONGO ||
                                      GetActorListIndex(actor) >= 0);
}

bool IsCurrentCombatRoom(const nlohmann::json& payload) {
    return payload.at("scene").get<int>() == gPlayState->sceneNum &&
           payload.at("room").get<int>() == gPlayState->roomCtx.curRoom.num &&
           payload.at("masterQuest").get<bool>() == (ResourceMgr_IsSceneMasterQuest(gPlayState->sceneNum) != 0);
}
} // namespace

bool Anchor::CanShareCombat() {
    return roomState.syncItemsAndFlags && CVarGetInteger(CVAR_REMOTE_ANCHOR("ShareEnemyDamage"), 0) && IsSaveLoaded() &&
           gPlayState->roomCtx.curRoom.num >= 0;
}

void Anchor::ResetCombatRoom() {
    combatScene = -1;
    combatRoom = -1;
    sharedEnemies.clear();
    enemyHits.clear();
}

void Anchor::UpdateCombatRoom() {
    if (combatScene != gPlayState->sceneNum || combatRoom != gPlayState->roomCtx.curRoom.num) {
        sharedEnemies.clear();
        combatScene = gPlayState->sceneNum;
        combatRoom = gPlayState->roomCtx.curRoom.num;
        combatVisit = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        const auto now = std::chrono::steady_clock::now();
        const NpcArea area{ combatScene, combatRoom, gSaveContext.linkAge,
                            ResourceMgr_IsSceneMasterQuest(combatScene) != 0 };
        for (const auto& [id, peer] : worldPeers) {
            if (peer.GetArea() == area && now - peer.updated < std::chrono::seconds(2) && IsSharedTeamMember(id)) {
                for (const auto& key : peer.emergedBabas) {
                    SetBabaEmerged(key, BabaEmergence::Emerged);
                }
            }
        }
    }
}

void Anchor::UpdateBabaEmergence(Actor* actor, bool* should, bool* alreadyEmerged) {
    if (!CanShareCombat() || actor->room != gPlayState->roomCtx.curRoom.num || GetActorListIndex(actor) < 0) {
        return;
    }
    UpdateCombatRoom();
    const auto key = GetEnemyKey(actor);
    const auto existing = sharedEnemies.find(key);
    if (existing != sharedEnemies.end() && existing->second.emergence != BabaEmergence::Dormant) {
        *should = true;
        *alreadyEmerged = existing->second.emergence == BabaEmergence::Emerged;
        existing->second.emergence = BabaEmergence::Emerged;
        return;
    }
    if (!*should) {
        return;
    }

    sharedEnemies[key].emergence = BabaEmergence::Emerged;
    nlohmann::json payload;
    payload["type"] = ENEMY_EMERGED;
    payload["quiet"] = true;
    payload["targetTeamId"] = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
    payload["scene"] = combatScene;
    payload["room"] = combatRoom;
    payload["age"] = gSaveContext.linkAge;
    payload["masterQuest"] = ResourceMgr_IsSceneMasterQuest(combatScene) != 0;
    payload["enemy"] = key;
    SendJsonToRemote(payload);
}

bool Anchor::ShouldKeepBabaEmerged(Actor* actor) {
    if (!CanShareCombat() || combatScene != gPlayState->sceneNum || combatRoom != gPlayState->roomCtx.curRoom.num ||
        actor->room != combatRoom || GetActorListIndex(actor) < 0) {
        return false;
    }
    const auto existing = sharedEnemies.find(GetEnemyKey(actor));
    return existing != sharedEnemies.end() && existing->second.emergence != BabaEmergence::Dormant;
}

void Anchor::SetBabaEmerged(const EnemyKey& key, BabaEmergence emergence) {
    if (key[1] < 0 || key[2] != 0 || key[3] != 1) {
        return;
    }
    auto& state = sharedEnemies[key];
    if (state.emergence != BabaEmergence::Dormant) {
        return;
    }
    state.emergence = emergence;
    for (Actor* actor = gPlayState->actorCtx.actorLists[ACTORCAT_ENEMY].head; actor != nullptr; actor = actor->next) {
        if (actor->id == key[0] && actor->room == combatRoom && GetActorListIndex(actor) >= 0 &&
            GetEnemyKey(actor) == key) {
            actor->flags |= ACTOR_FLAG_UPDATE_CULLING_DISABLED;
            break;
        }
    }
}

void Anchor::HandlePacket_EnemyEmerged(const nlohmann::json& payload) {
    const auto id = payload.at("clientId").get<uint32_t>();
    if (!CanShareCombat() || !IsSharedTeamMember(id) || !IsCurrentCombatRoom(payload) ||
        payload.at("age").get<int>() != gSaveContext.linkAge) {
        return;
    }
    const auto key = payload.at("enemy").get<EnemyKey>();
    if (IsBaba(key[0])) {
        UpdateCombatRoom();
        SetBabaEmerged(key, BabaEmergence::Animate);
    }
}

void Anchor::SendPacket_EnemyDamage(Actor* actor, int32_t damage, int32_t reaction) {
    if (!IsSharedEnemy(actor->id) || !CanShareCombat() || !IsEligibleEnemy(actor) ||
        (actor->room >= 0 && actor->room != gPlayState->roomCtx.curRoom.num)) {
        return;
    }

    UpdateCombatRoom();
    const auto key = GetEnemyKey(actor);
    int32_t total = 0;
    if (reaction == GI_ENEMY_HIT_DEFEAT) {
        sharedEnemies[key].defeated = true;
    }
    if (damage > 0) {
        auto& accumulated = sharedEnemies[key].totals[{ ownClientId, combatVisit }];
        accumulated = std::min(accumulated + damage, 255);
        total = accumulated;
    }

    nlohmann::json payload;
    payload["type"] = ENEMY_DAMAGE;
    payload["quiet"] = true;
    payload["targetTeamId"] = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
    payload["scene"] = combatScene;
    payload["room"] = combatRoom;
    payload["age"] = gSaveContext.linkAge;
    payload["masterQuest"] = ResourceMgr_IsSceneMasterQuest(combatScene) != 0;
    payload["enemy"] = key;
    payload["damage"] = damage;
    payload["reaction"] = reaction;
    payload["visit"] = combatVisit;
    payload["total"] = total;
    if (actor->id == ACTOR_BOSS_GOMA && IsGohmaPhase(reaction)) {
        payload["reactionPose"] = actor->world;
    }
    SendJsonToRemote(payload);
}

void Anchor::QueueEnemyDamage(const EnemyKey& key, int32_t damage, int32_t reaction, EnemyDamageOrigin origin,
                              int32_t total, const PosRot* pose) {
    if (!IsSharedEnemy(key[0]) || total < 0 || total > 255 || damage < 0 || damage > 255 ||
        reaction < 0 || reaction >= GI_ENEMY_HIT_MAX || (total == 0 && damage == 0 && reaction == 0) ||
        (IsGohmaPhase(reaction) && (key[0] != ACTOR_BOSS_GOMA || pose == nullptr)) ||
        ((IsBaba(key[0]) || key[0] == ACTOR_EN_ST) && (key[1] < 0 || key[2] != 0 || key[3] != 1)) ||
        (key[0] == ACTOR_EN_KAREBABA && (damage != 0 || total != 0 || reaction != GI_ENEMY_HIT_KAREBABA_KILLED)) ||
        (reaction == GI_ENEMY_HIT_KAREBABA_KILLED && key[0] != ACTOR_EN_KAREBABA) ||
        (reaction == GI_ENEMY_HIT_SCRUB_RUN && key[0] != ACTOR_EN_DEKUNUTS) ||
        (reaction == GI_ENEMY_HIT_DODONGO_SWALLOW && (key[0] != ACTOR_BOSS_DODONGO || total != 0 || damage != 0)) ||
        (reaction == GI_ENEMY_HIT_DEFEAT && key[0] != ACTOR_EN_WF && key[0] != ACTOR_EN_DEKUNUTS) ||
        (reaction == GI_ENEMY_HIT_SKULLTULA_ARROW && key[0] != ACTOR_EN_ST)) {
        return;
    }
    if (IsBaba(key[0])) {
        // A hit that got through means the plant was already up on the sender.
        SetBabaEmerged(key, BabaEmergence::Emerged);
    }
    auto& state = sharedEnemies[key];
    if (reaction == GI_ENEMY_HIT_DEFEAT && !state.defeated) {
        state.defeated = true;
        state.pendingReaction = GI_ENEMY_HIT_DEFEAT;
    }
    if (total > 0) {
        auto& previous = state.totals[origin];
        if (total <= previous) {
            return;
        }
        damage = total - previous;
        previous = total;
    }
    state.pendingDamage = std::min(state.pendingDamage + damage, 255);
    if (reaction != GI_ENEMY_HIT_NORMAL && reaction != GI_ENEMY_HIT_DEFEAT &&
        state.pendingReaction != GI_ENEMY_HIT_DEFEAT) {
        state.pendingReaction = reaction;
        if (pose != nullptr) {
            state.pendingReactionPose = *pose;
        }
    }
}

void Anchor::HandlePacket_EnemyDamage(nlohmann::json payload) {
    const auto id = payload.at("clientId").get<uint32_t>();
    if (!CanShareCombat() || !IsSharedTeamMember(id) || !payload.contains("enemy") || !IsCurrentCombatRoom(payload) ||
        payload.value("age", static_cast<int>(gSaveContext.linkAge)) != gSaveContext.linkAge) {
        return;
    }
    UpdateCombatRoom();
    const int32_t reaction = payload.at("reaction").get<int32_t>();
    PosRot pose{};
    if (IsGohmaPhase(reaction)) {
        pose = payload.at("reactionPose").get<PosRot>();
        const auto valid = [](float value) { return std::isfinite(value) && std::abs(value) < 100000.0f; };
        if (!valid(pose.pos.x) || !valid(pose.pos.y) || !valid(pose.pos.z)) {
            return;
        }
    }
    QueueEnemyDamage(payload.at("enemy").get<EnemyKey>(), payload.at("damage").get<int32_t>(), reaction,
                     { id, payload.at("visit").get<uint64_t>() }, payload.at("total").get<int32_t>(),
                     IsGohmaPhase(reaction) ? &pose : nullptr);
}

void Anchor::ConsumeSharedEnemyDamage(Actor* actor, int32_t* damage, int32_t* reaction, PosRot* pose) {
    if (sharedEnemies.empty() || combatScene != gPlayState->sceneNum || combatRoom != gPlayState->roomCtx.curRoom.num ||
        (actor->room >= 0 && actor->room != combatRoom) || !IsEligibleEnemy(actor) || !CanShareCombat()) {
        return;
    }

    const auto entry = sharedEnemies.find(GetEnemyKey(actor));
    if (entry != sharedEnemies.end()) {
        *damage = std::exchange(entry->second.pendingDamage, 0);
        *reaction = std::exchange(entry->second.pendingReaction, 0);
        if (pose != nullptr && IsGohmaPhase(*reaction)) {
            *pose = entry->second.pendingReactionPose;
        }
    }
}

void Anchor::AddEnemyDamageSnapshot(nlohmann::json& payload) {
    UpdateCombatRoom();
    auto enemies = nlohmann::json::array();
    auto emerged = nlohmann::json::array();
    for (const auto& [key, state] : sharedEnemies) {
        if (state.emergence != BabaEmergence::Dormant) {
            emerged.push_back(key);
        }
        if (!state.totals.empty() || state.defeated) {
            auto hits = nlohmann::json::array();
            for (const auto& [origin, total] : state.totals) {
                hits.push_back({ origin.first, origin.second, total });
            }
            enemies.push_back({ key, std::move(hits), state.defeated });
        }
    }
    if (!enemies.empty()) {
        payload["enemyDamage"] = std::move(enemies);
    }
    if (!emerged.empty()) {
        payload["emergedBabas"] = std::move(emerged);
    }
}

void Anchor::ReadBabaEmergenceSnapshot(const nlohmann::json& payload, WorldPeer& peer) {
    const auto list = payload.find("emergedBabas");
    if (list == payload.end() || !list->is_array() || list->size() > 128) {
        return;
    }
    const bool sameArea = peer.age == gSaveContext.linkAge && IsCurrentCombatRoom(payload);
    if (sameArea) {
        UpdateCombatRoom();
    }
    for (const auto& entry : *list) {
        const auto key = entry.get<EnemyKey>();
        if (IsBaba(key[0])) {
            peer.emergedBabas.push_back(key);
            if (sameArea) {
                SetBabaEmerged(key, BabaEmergence::Emerged);
            }
        }
    }
}

void Anchor::ReadEnemyDamageSnapshot(const nlohmann::json& payload) {
    if (!payload.contains("enemyDamage") || !IsCurrentCombatRoom(payload) ||
        payload.value("age", static_cast<int>(gSaveContext.linkAge)) != gSaveContext.linkAge) {
        return;
    }
    const auto& enemies = payload.at("enemyDamage");
    if (!enemies.is_array() || enemies.size() > 128) {
        return;
    }
    UpdateCombatRoom();
    for (const auto& enemy : enemies) {
        const auto key = enemy.at(0).get<EnemyKey>();
        const auto& hits = enemy.at(1);
        if (!hits.is_array() || hits.size() > 128) {
            continue;
        }
        for (const auto& hit : hits) {
            QueueEnemyDamage(key, 0, GI_ENEMY_HIT_NORMAL, { hit.at(0).get<uint32_t>(), hit.at(1).get<uint64_t>() },
                             hit.at(2).get<int32_t>());
        }
        if (enemy.size() > 2 && enemy.at(2).is_boolean() && enemy.at(2).get<bool>()) {
            QueueEnemyDamage(key, 0, GI_ENEMY_HIT_DEFEAT, {}, 0);
        }
    }
}
