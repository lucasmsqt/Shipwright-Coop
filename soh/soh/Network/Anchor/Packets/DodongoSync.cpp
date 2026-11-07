#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/JsonConversions.hpp"
#include "soh/ObjectExtension/ActorListIndex.h"
#include <cmath>

extern "C" {
#include "functions.h"
#include "macros.h"
#include "overlays/actors/ovl_En_Bom/z_en_bom.h"
#include "overlays/actors/ovl_En_Bombf/z_en_bombf.h"
#include "overlays/actors/ovl_En_Vm/z_en_vm.h"
extern PlayState* gPlayState;
}

namespace {
bool IsDodongoScene(int scene) {
    return scene == SCENE_DODONGOS_CAVERN || scene == SCENE_DODONGOS_CAVERN_BOSS;
}

bool IsBomb(int actorId) {
    return actorId == ACTOR_EN_BOM || actorId == ACTOR_EN_BOMBF;
}

AnchorDodongoBomb& BombSync(Actor* actor) {
    return actor->id == ACTOR_EN_BOM ? reinterpret_cast<EnBom*>(actor)->anchorBomb
                                    : reinterpret_cast<EnBombf*>(actor)->anchorBomb;
}

bool IsBounded(float value, float min, float max) {
    return std::isfinite(value) && value >= min && value <= max;
}

bool IsPosition(const Vec3f& value, float limit = 100000.0f) {
    return IsBounded(value.x, -limit, limit) && IsBounded(value.y, -limit, limit) && IsBounded(value.z, -limit, limit);
}

bool IsInRoom(const Actor* actor) {
    return actor->room < 0 || actor->room == gPlayState->roomCtx.curRoom.num;
}

bool IsFresh(std::chrono::steady_clock::time_point updated) {
    return std::chrono::steady_clock::now() - updated < std::chrono::seconds(1);
}
} // namespace

void Anchor::MarkDodongoBomb(Actor* actor) {
    if (CanShareRoomObjects() && IsDodongoScene(gPlayState->sceneNum)) {
        BombSync(actor).serial = ++dodongoBombSerial;
        nextWorldUpdate = {};
    }
}

void Anchor::RefreshBeamosBeam() {
    if (isConnected && gPlayState != nullptr && gPlayState->sceneNum == SCENE_DODONGOS_CAVERN && CanShareCombat()) {
        nextWorldUpdate = {};
    }
}

void Anchor::AddDodongoSnapshot(nlohmann::json& payload) {
    if (!IsDodongoScene(gPlayState->sceneNum)) {
        return;
    }

    auto platforms = nlohmann::json::array();
    for (Actor* actor = gPlayState->actorCtx.actorLists[ACTORCAT_BG].head; actor != nullptr && platforms.size() < 16;
         actor = actor->next) {
        if (actor->id != ACTOR_BG_DDAN_JD || !IsInRoom(actor) || actor->init != nullptr || actor->update == nullptr) {
            continue;
        }
        const int index = GetActorListIndex(actor);
        if (index >= 0) {
            AnchorDodongoPlatform motion{};
            BgDdanJd_GetAnchorMotion(actor, &motion);
            platforms.push_back({ index, motion.height, motion.targetHeight, motion.state, motion.idleTimer,
                                  motion.speed, motion.moving != 0 });
        }
    }
    if (!platforms.empty()) {
        payload["dodongoPlatforms"] = std::move(platforms);
    }

    if (CanShareCombat()) {
        if (gPlayState->sceneNum == SCENE_DODONGOS_CAVERN_BOSS) {
            Actor* actor = Actor_Find(&gPlayState->actorCtx, ACTOR_BOSS_DODONGO, ACTORCAT_BOSS);
            AnchorDodongoBoss boss{};
            if (actor != nullptr && actor->init == nullptr && actor->update != nullptr &&
                BossDodongo_GetAnchorState(actor, &boss)) {
                payload["dodongoBoss"] = { boss.position, boss.rotation, boss.speed, boss.frame, boss.animationSpeed,
                                           boss.animationStart, boss.animationEnd, boss.yOffset, boss.bodyStretch,
                                           boss.phase, boss.animation, boss.animationMode, boss.health, boss.introState,
                                           boss.timer, boss.inhaleTime, boss.fireTime, boss.corner, boss.direction,
                                           boss.wallHits, boss.rollAngle };
            }
        }
        auto beams = nlohmann::json::array();
        for (Actor* actor = gPlayState->actorCtx.actorLists[ACTORCAT_ENEMY].head; actor != nullptr && beams.size() < 16;
             actor = actor->next) {
            if (actor->id != ACTOR_EN_VM || !IsInRoom(actor) || actor->init != nullptr || actor->update == nullptr) {
                continue;
            }
            const int index = GetActorListIndex(actor);
            if (index >= 0) {
                const auto& beamos = *reinterpret_cast<const EnVm*>(actor);
                beams.push_back({ index, beamos.beamRot, beamos.beamScale, beamos.beamPos3, beamos.skelAnime.curFrame,
                                  beamos.headRotY, beamos.unk_260 });
            }
        }
        if (!beams.empty()) {
            payload["beamosBeams"] = std::move(beams);
        }
    }

    auto bombs = nlohmann::json::array();
    const auto* player = GET_PLAYER(gPlayState);
    for (Actor* actor = gPlayState->actorCtx.actorLists[ACTORCAT_EXPLOSIVE].head; actor != nullptr && bombs.size() < 16;
         actor = actor->next) {
        if (!IsBomb(actor->id) || !IsInRoom(actor) || actor->init != nullptr || actor->update == nullptr) {
            continue;
        }
        const auto& sync = BombSync(actor);
        if (sync.owner != 0 || sync.serial == 0) {
            continue;
        }
        const bool exploded = actor->params == BOMB_EXPLOSION;
        const bool held = !exploded && player->heldActor == actor && actor->parent == &player->actor;
        Vec3f position = actor->world.pos;
        if (held) {
            const auto& origin = player->actor.world.pos;
            position = { position.x - origin.x, position.y - origin.y, position.z - origin.z };
        }
        const float flash = actor->id == ACTOR_EN_BOM ? reinterpret_cast<const EnBom*>(actor)->flashIntensity
                                                       : reinterpret_cast<const EnBombf*>(actor)->flashIntensity;
        bombs.push_back({ sync.serial, actor->id, position, actor->scale.x, flash, held, exploded });
    }
    if (!bombs.empty()) {
        payload["dodongoBombs"] = std::move(bombs);
    }
}

void Anchor::ReadDodongoSnapshot(const nlohmann::json& payload, WorldPeer& peer) {
    if (!IsDodongoScene(peer.scene)) {
        return;
    }
    if (const auto data = payload.find("dodongoBoss"); peer.scene == SCENE_DODONGOS_CAVERN_BOSS && peer.syncCombat &&
        data != payload.end() && data->is_array() && data->size() == 21) {
        const auto& d = *data;
        const AnchorDodongoBoss boss{ d[0].get<Vec3f>(), d[1].get<Vec3s>(), d[2].get<float>(), d[3].get<float>(),
                                       d[4].get<float>(), d[5].get<float>(), d[6].get<float>(), d[7].get<float>(),
                                       d[8].get<float>(), d[9].get<s16>(), d[10].get<s16>(), d[11].get<s16>(),
                                       d[12].get<s16>(), d[13].get<s16>(), d[14].get<s16>(), d[15].get<s16>(),
                                       d[16].get<s16>(), d[17].get<s16>(), d[18].get<s16>(), d[19].get<s16>(),
                                       d[20].get<s16>() };
        if (IsPosition(boss.position, 8000) && IsBounded(boss.speed, -40, 40) && IsBounded(boss.frame, 0, 200) &&
            IsBounded(boss.animationSpeed, -10, 10) && IsBounded(boss.animationStart, 0, 200) &&
            IsBounded(boss.animationEnd, 0, 200) && IsBounded(boss.yOffset, -10000, 12000) &&
            IsBounded(boss.bodyStretch, 0, 2) && boss.phase >= ANCHOR_DODONGO_INTRO && boss.phase < ANCHOR_DODONGO_PHASE_COUNT &&
            boss.animation >= 0 && boss.animation < 13 && boss.animationMode >= 0 && boss.animationMode <= 5 &&
            boss.health >= -16 && boss.health <= 12 && boss.introState >= 0 && boss.introState <= 100 &&
            boss.timer >= 0 && boss.timer <= 2000 && boss.inhaleTime >= 0 && boss.inhaleTime <= 100 &&
            boss.fireTime >= 0 && boss.fireTime <= 100 && boss.corner >= 0 && boss.corner <= 3 &&
            boss.direction >= 0 && boss.direction <= 1 && boss.wallHits >= 0 && boss.wallHits <= 100) {
            peer.dodongoBoss = boss;
        }
    }
    if (const auto list = payload.find("dodongoPlatforms"); list != payload.end() && list->is_array() && list->size() <= 16) {
        for (const auto& entry : *list) {
            const int index = entry.at(0).get<int>();
            const AnchorDodongoPlatform motion{ entry.at(1).get<float>(), entry.at(2).get<float>(),
                                                entry.at(3).get<int>(), entry.at(4).get<int>(), entry.at(5).get<int>(),
                                                entry.at(6).get<bool>() };
            if (index >= 0 && index <= INT16_MAX && IsBounded(motion.height, 0, 700) &&
                IsBounded(motion.targetHeight, 0, 700) && motion.state >= 0 && motion.state <= 3 &&
                motion.idleTimer >= 0 && motion.idleTimer <= 100 && (motion.speed == 1 || motion.speed == 5)) {
                peer.dodongoPlatforms.emplace(index, motion);
            }
        }
    }
    if (const auto list = payload.find("beamosBeams");
        peer.syncCombat && list != payload.end() && list->is_array() && list->size() <= 16) {
        for (const auto& entry : *list) {
            const int index = entry.at(0).get<int>();
            const AnchorBeamosBeam beam{ entry.at(1).get<Vec3s>(), entry.at(2).get<Vec3f>(), entry.at(3).get<Vec3f>(),
                                         entry.at(4).get<float>(), entry.at(5).get<s16>(), entry.at(6).get<s16>() };
            if (index >= 0 && index <= INT16_MAX && IsPosition(beam.endpoint) && IsBounded(beam.scale.x, 0, 1) &&
                IsBounded(beam.scale.y, 0, 1) && IsBounded(beam.scale.z, -5, 2000) && IsBounded(beam.frame, 0, 100) &&
                beam.stage >= 0 && beam.stage <= 4) {
                peer.beamosBeams.emplace(index, beam);
            }
        }
    }
    if (const auto list = payload.find("dodongoBombs"); list != payload.end() && list->is_array() && list->size() <= 16) {
        for (const auto& entry : *list) {
            const auto serial = entry.at(0).get<uint32_t>();
            const DodongoBombPose pose{ entry.at(2).get<Vec3f>(), entry.at(3).get<float>(), entry.at(4).get<float>(),
                                        entry.at(1).get<int>(), entry.at(5).get<bool>(), entry.at(6).get<bool>() };
            if (serial != 0 && IsBomb(pose.actorId) && IsPosition(pose.position, pose.held ? 180.0f : 100000.0f) &&
                IsBounded(pose.scale, 0, 0.1f) && IsBounded(pose.flash, 0, 255) && !(pose.held && pose.exploded)) {
                peer.dodongoBombs.emplace(serial, pose);
            }
        }
    }
}

void Anchor::SpawnDodongoBombReplicas(uint32_t sender, const WorldPeer& peer) {
    if (peer.dodongoBombs.empty() || peer.GetArea() != GetRoomObjectArea()) {
        return;
    }
    std::set<uint32_t> existing;
    for (Actor* actor = gPlayState->actorCtx.actorLists[ACTORCAT_PROP].head; actor != nullptr; actor = actor->next) {
        if (IsBomb(actor->id) && actor->update != nullptr && BombSync(actor).owner == sender) {
            existing.insert(BombSync(actor).serial);
        }
    }
    for (const auto& [serial, pose] : peer.dodongoBombs) {
        if (existing.contains(serial)) {
            continue;
        }
        auto pos = pose.position;
        if (pose.held) {
            const auto& source = clients.at(sender).posRot.pos;
            pos.x += source.x;
            pos.y += source.y;
            pos.z += source.z;
        }
        auto* actor = Actor_Spawn(&gPlayState->actorCtx, gPlayState, pose.actorId, pos.x, pos.y, pos.z, 0, 0, 0, 0);
        if (actor != nullptr) {
            BombSync(actor) = { serial, sender, false };
            actor->room = peer.room;
            if (actor->category != ACTORCAT_PROP) {
                Actor_ChangeCategory(gPlayState, &gPlayState->actorCtx, actor, ACTORCAT_PROP);
            }
        }
    }
}

bool Anchor::UpdateDodongoBombReplica(Actor* actor, PlayState* play, float* flash) {
    auto& sync = BombSync(actor);
    if (sync.owner == 0) {
        return false;
    }
    const auto peer = worldPeers.find(sync.owner);
    if (!CanShareRoomObjects() || peer == worldPeers.end() || !IsSharedTeamMember(sync.owner) ||
        peer->second.GetArea() != GetRoomObjectArea() || !IsFresh(peer->second.updated)) {
        Actor_Kill(actor);
        return true;
    }
    const auto entry = peer->second.dodongoBombs.find(sync.serial);
    if (entry == peer->second.dodongoBombs.end()) {
        Actor_Kill(actor);
        return true;
    }
    const auto& pose = entry->second;
    actor->world.pos = pose.position;
    if (pose.held) {
        const auto& source = clients.at(sync.owner).posRot.pos;
        actor->world.pos.x += source.x;
        actor->world.pos.y += source.y;
        actor->world.pos.z += source.z;
    }
    Actor_SetScale(actor, pose.scale);
    *flash = pose.flash;
    if (pose.exploded && !sync.exploded) {
        sync.exploded = true;
        Vec3f position = actor->world.pos;
        Vec3f velocity{};
        Vec3f acceleration{ 0.0f, 0.1f, 0.0f };
        position.y += 10.0f;
        EffectSsBomb2_SpawnLayered(play, &position, &velocity, &acceleration, 100, 19);
        Audio_PlayActorSound2(actor, NA_SE_IT_BOMB_EXPLOSION);
        actor->draw = nullptr;
    }
    return true;
}

bool Anchor::ReadDodongoPlatform(Actor* actor, AnchorDodongoPlatform* motion, u64* seen) {
    if (!CanShareRoomObjects() || gPlayState->sceneNum != SCENE_DODONGOS_CAVERN || !IsInRoom(actor) || worldPeers.empty()) {
        return false;
    }
    const auto area = GetRoomObjectArea();
    const auto peer = worldPeers.find(ChooseNpcAuthority(area, false));
    if (peer == worldPeers.end() || peer->second.GetArea() != area || !IsFresh(peer->second.updated)) {
        return false;
    }
    const auto stamp = static_cast<u64>(peer->second.updated.time_since_epoch().count());
    const auto sample = peer->second.dodongoPlatforms.find(GetActorListIndex(actor));
    if (stamp == *seen || sample == peer->second.dodongoPlatforms.end()) {
        return false;
    }
    *seen = stamp;
    *motion = sample->second;
    return true;
}

bool Anchor::ReadBeamosBeam(Actor* actor, AnchorBeamosBeam* beam) {
    if (gPlayState == nullptr || gPlayState->sceneNum != SCENE_DODONGOS_CAVERN || worldPeers.empty()) {
        return false;
    }
    const auto peer = worldPeers.find(FindNearestCombatController(actor));
    if (peer == worldPeers.end() || !IsFresh(peer->second.updated)) {
        return false;
    }
    const auto sample = peer->second.beamosBeams.find(GetActorListIndex(actor));
    if (sample == peer->second.beamosBeams.end()) {
        return false;
    }
    *beam = sample->second;
    return true;
}

bool Anchor::ReadDodongoBoss(Actor* actor, AnchorDodongoBoss* state) {
    if (!isConnected || !CanShareCombat() || gPlayState->sceneNum != SCENE_DODONGOS_CAVERN_BOSS ||
        !IsInRoom(actor) || worldPeers.empty()) {
        return false;
    }
    const auto area = GetRoomObjectArea();
    const auto peer = worldPeers.find(ChooseNpcAuthority(area, false));
    if (peer != worldPeers.end() && peer->second.GetArea() == area && peer->second.dodongoBoss &&
        IsFresh(peer->second.updated)) {
        *state = *peer->second.dodongoBoss;
        return true;
    }
    AnchorDodongoBoss local{};
    if (BossDodongo_GetAnchorState(actor, &local) && local.phase == ANCHOR_DODONGO_INTRO && local.introState == 0) {
        for (const auto& [id, other] : worldPeers) {
            if (other.GetArea() == area && other.dodongoBoss && other.dodongoBoss->phase == ANCHOR_DODONGO_INTRO &&
                other.dodongoBoss->introState > 0 && IsFresh(other.updated) && IsSharedTeamMember(id)) {
                *state = *other.dodongoBoss;
                return true;
            }
        }
    }
    return false;
}

extern "C" void Anchor_MarkDodongoBomb(Actor* actor) {
    if (Anchor::Instance != nullptr) {
        Anchor::Instance->MarkDodongoBomb(actor);
    }
}

extern "C" void Anchor_RefreshBeamosBeam(void) {
    if (Anchor::Instance != nullptr) {
        Anchor::Instance->RefreshBeamosBeam();
    }
}

extern "C" s32 Anchor_UpdateDodongoBombReplica(Actor* actor, PlayState* play, f32* flash) {
    return Anchor::Instance != nullptr && Anchor::Instance->UpdateDodongoBombReplica(actor, play, flash);
}

extern "C" s32 Anchor_ReadDodongoPlatform(Actor* actor, AnchorDodongoPlatform* motion, u64* seen) {
    return Anchor::Instance != nullptr && Anchor::Instance->ReadDodongoPlatform(actor, motion, seen);
}

extern "C" s32 Anchor_ReadBeamosBeam(Actor* actor, AnchorBeamosBeam* beam) {
    return Anchor::Instance != nullptr && Anchor::Instance->ReadBeamosBeam(actor, beam);
}

extern "C" s32 Anchor_ReadDodongoBoss(Actor* actor, AnchorDodongoBoss* state) {
    return Anchor::Instance != nullptr && Anchor::Instance->ReadDodongoBoss(actor, state);
}
