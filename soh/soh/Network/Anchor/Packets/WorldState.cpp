#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/JsonConversions.hpp"
#include "soh/ObjectExtension/ActorListIndex.h"
#include "soh/ResourceManagerHelpers.h"
#include <algorithm>
#include <cmath>
#include <set>

extern "C" {
#include "functions.h"
#include "overlays/actors/ovl_En_Ani/z_en_ani.h"
#include "overlays/actors/ovl_En_Heishi1/z_en_heishi1.h"
#include "overlays/actors/ovl_En_Hs/z_en_hs.h"
#include "overlays/actors/ovl_En_Hs2/z_en_hs2.h"
#include "overlays/actors/ovl_En_Hy/z_en_hy.h"
extern PlayState* gPlayState;
s32 Interface_ReloadForSunSong(PlayState* play, u16 nextDayTime);
}

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr auto worldUpdateInterval = 250ms;
constexpr auto peerTimeout = 2s;
constexpr auto arrivalTieWindow = 500ms;
constexpr int maxAreaAgeMs = 24 * 60 * 60 * 1000;
constexpr int maxNpcPositions = 48;
constexpr uint32_t maxClockSequence = 1'000'000'000;

bool SyncNpcPositionsEnabled() {
    return CVarGetInteger(CVAR_REMOTE_ANCHOR("SyncNpcPositions"), 0) != 0;
}

bool SyncFieldTimeEnabled() {
    return CVarGetInteger(CVAR_REMOTE_ANCHOR("SyncHyruleFieldTime"), 0) != 0;
}

bool IsGuardCourtyard(int scene) {
    return scene == SCENE_CASTLE_COURTYARD_GUARDS_DAY || scene == SCENE_CASTLE_COURTYARD_GUARDS_NIGHT;
}

bool IsClockScene(int scene) {
    return scene == SCENE_HYRULE_FIELD || scene == SCENE_GRAVEYARD || scene == SCENE_KAKARIKO_VILLAGE;
}

bool IsNightTime(u16 time) {
    return time < 0x4555 || time > 0xC000;
}

bool HasSunSongDestination() {
    return gSaveContext.nextDayTime == 0 || gSaveContext.nextDayTime == 0x8001;
}

bool IsVillageGazeNpc(s16 id) {
    return id == ACTOR_EN_ANI || id == ACTOR_EN_HS || id == ACTOR_EN_HS2 || id == ACTOR_EN_HY;
}

bool IsRoomNpc(const Actor* actor) {
    return (actor->room == gPlayState->roomCtx.curRoom.num ||
            (actor->room < 0 && (actor->id == ACTOR_EN_TK || IsVillageGazeNpc(actor->id)))) &&
           actor->init == nullptr && actor->update != nullptr;
}

int NpcIndex(const Actor* actor) {
    const int index = GetActorListIndex(actor);
    if (index >= 0) {
        return index;
    }
    switch (actor->id) {
        case ACTOR_EN_TK: return INT16_MAX;
        case ACTOR_EN_ANI: return INT16_MAX - 1;
        case ACTOR_EN_HS2: return INT16_MAX - 2;
        case ACTOR_EN_HS: return INT16_MAX - 3;
        default: return -1;
    }
}

std::pair<Vec3s*, Vec3s*> NpcGazeRotations(Actor* actor) {
    if (actor->id == ACTOR_EN_ANI) {
        auto* npc = reinterpret_cast<EnAni*>(actor);
        return { &npc->unk_29C, &npc->unk_2A2 };
    }
    if (actor->id == ACTOR_EN_HS2) {
        auto* npc = reinterpret_cast<EnHs2*>(actor);
        return { &npc->unk_29C, &npc->unk_2A2 };
    }
    if (actor->id == ACTOR_EN_HS) {
        auto* npc = reinterpret_cast<EnHs*>(actor);
        return { &npc->unk_29C, &npc->unk_2A2 };
    }
    auto* npc = reinterpret_cast<EnHy*>(actor);
    return { &npc->interactInfo.headRot, &npc->interactInfo.torsoRot };
}

s16 GazeDistanceBand(Actor* actor) {
    if (actor->xzDistToPlayer >= 400.0f ||
        std::abs(static_cast<s16>(actor->yawTowardsPlayer - actor->shape.rot.y)) >= 0x4300) {
        return INT16_MAX;
    }
    const float distance = std::sqrt(std::max(0.0f, actor->xyzDistToPlayerSq));
    return static_cast<s16>(std::min(distance / 20.0f, static_cast<float>(INT16_MAX - 1)));
}
} // namespace

uint32_t Anchor::GetNpcCoordinator() const {
    uint32_t coordinator = ownClientId;
    for (const auto& [id, peer] : worldPeers) {
        if (peer.syncNpcs && peer.visit != 0 && IsSharedTeamMember(id) &&
            (id == roomState.ownerClientId || (coordinator != roomState.ownerClientId && id < coordinator))) {
            coordinator = id;
        }
    }
    return coordinator;
}

uint32_t Anchor::ChooseNpcAuthority(const NpcArea& area) const {
    const auto isSameArea = [&](uint32_t id, const WorldPeer& peer) {
        return peer.syncNpcs && peer.visit != 0 && peer.GetArea() == area && IsSharedTeamMember(id);
    };

    const bool localCandidate = localWorldPeer.GetArea() == area;
    auto firstArrival = localCandidate ? localWorldPeer.entered : Clock::time_point::max();
    for (const auto& [id, peer] : worldPeers) {
        if (isSameArea(id, peer)) {
            firstArrival = std::min(firstArrival, peer.entered);
        }
    }

    if (firstArrival == Clock::time_point::max()) {
        return 0;
    }
    const auto tieLimit = firstArrival + arrivalTieWindow;
    uint32_t authority = localCandidate && localWorldPeer.entered <= tieLimit ? ownClientId : 0;
    for (const auto& [id, peer] : worldPeers) {
        if (isSameArea(id, peer) && peer.entered <= tieLimit &&
            (authority == 0 || id == roomState.ownerClientId ||
             (authority != roomState.ownerClientId && id < authority))) {
            authority = id;
        }
    }
    return authority;
}

Anchor::WorldClockVersion Anchor::GetWorldClockVersion(int scene) const {
    const auto version = worldClockVersions.find(scene);
    return version == worldClockVersions.end() ? WorldClockVersion{} : version->second;
}

const Anchor::WorldPeer* Anchor::FindWorldClockPeer(int scene, bool entering) const {
    const auto now = Clock::now();
    const auto localVersion = GetWorldClockVersion(scene);
    const bool field = scene == SCENE_HYRULE_FIELD;
    const auto eligible = [&](uint32_t id, const WorldPeer& peer) {
        return peer.scene == scene && (field || peer.age == gSaveContext.linkAge) && peer.hasClock &&
               peer.clockVersion >= localVersion && now - peer.updated < peerTimeout && IsSharedTeamMember(id);
    };
    const bool local = !entering && localWorldPeer.scene == scene;
    auto version = localVersion;
    auto first = local ? localWorldPeer.entered : Clock::time_point::max();
    for (const auto& [id, peer] : worldPeers) {
        if (!eligible(id, peer)) {
            continue;
        }
        if (peer.clockVersion > version) {
            version = peer.clockVersion;
            first = peer.entered;
        } else if (peer.clockVersion == version) {
            first = std::min(first, peer.entered);
        }
    }
    if (first == Clock::time_point::max()) {
        return nullptr;
    }
    const auto tie = first + (field ? arrivalTieWindow : 0ms);
    uint32_t authority = local && localVersion == version && localWorldPeer.entered <= tie ? ownClientId : 0;
    const WorldPeer* clock = nullptr;
    for (const auto& [id, peer] : worldPeers) {
        if (eligible(id, peer) && peer.clockVersion == version && peer.entered <= tie &&
            (authority == 0 || id == roomState.ownerClientId || (authority != roomState.ownerClientId && id < authority))) {
            authority = id;
            clock = &peer;
        }
    }
    return clock;
}

void Anchor::ApplyTeamWorldTime() {
    if (!SyncFieldTimeEnabled() || IS_CUTSCENE_LAYER || Play_InCsMode(gPlayState) ||
        gSaveContext.sunsSongState != SUNSSONG_INACTIVE || gPlayState->transitionTrigger != TRANS_TRIGGER_OFF ||
        gPlayState->transitionMode != TRANS_MODE_OFF || gSaveContext.health <= 0) {
        return;
    }

    const int scene = gPlayState->sceneNum;
    const auto* clock = FindWorldClockPeer(scene, false);
    if (clock == nullptr) {
        return;
    }
    const bool songChangedTime = clock->clockVersion > GetWorldClockVersion(scene);
    if (!songChangedTime && scene != SCENE_HYRULE_FIELD) {
        return;
    }
    if (songChangedTime && gPlayState->envCtx.timeIncrement == 0 &&
        IsNightTime(clock->dayTime) != IsNightTime(gSaveContext.dayTime)) {
        if (gPlayState->pauseCtx.state != 0 || gPlayState->pauseCtx.debugState != 0 ||
            gPlayState->msgCtx.msgLength != 0 ||
            !Interface_ReloadForSunSong(gPlayState, IsNightTime(clock->dayTime) ? 0 : 0x8001)) {
            return;
        }
    } else {
        gSaveContext.dayTime = clock->dayTime;
        gSaveContext.skyboxTime = clock->skyboxTime;
        gSaveContext.nightFlag = IsNightTime(clock->dayTime);
    }
    if (songChangedTime) {
        worldClockVersions[scene] = clock->clockVersion;
        nextWorldUpdate = {};
    }
}

void Anchor::RecordSunSongTime(PlayState* play) {
    if (play != gPlayState || !CanShareRoomObjects() || !SyncFieldTimeEnabled() ||
        IS_CUTSCENE_LAYER || play->csCtx.state != CS_STATE_IDLE) {
        return;
    }
    const int scene = play->sceneNum;
    uint32_t sequence = GetWorldClockVersion(scene).first;
    for (const auto& [id, peer] : worldPeers) {
        if (peer.scene == scene && IsSharedTeamMember(id)) {
            sequence = std::max(sequence, peer.clockVersion.first);
        }
    }
    if (sequence < maxClockSequence) {
        worldClockVersions[scene] = { sequence + 1, ownClientId };
        nextWorldUpdate = {};
        if (HasSunSongDestination()) {
            // Fixed-time areas can load a different scene after the fade.
            UpdateWorldState();
        }
    }
}

extern "C" void Anchor_RecordSunSongTime(PlayState* play) {
    if (Anchor::Instance != nullptr) {
        Anchor::Instance->RecordSunSongTime(play);
    }
}

void Anchor::ApplyWorldTimeOnEntry(PlayState* play) {
    if (!isConnected || !roomState.syncItemsAndFlags || !IsSaveLoaded(false) || gPlayState != play ||
        gSaveContext.entranceIndex < 0 || gSaveContext.entranceIndex >= ENTR_MAX || gSaveContext.cutsceneIndex >= 0xFFF0 ||
        HasSunSongDestination()) {
        return;
    }
    const auto scene = gEntranceTable[gSaveContext.entranceIndex].scene;
    if (SyncFieldTimeEnabled()) {
        if (const auto* clock = FindWorldClockPeer(scene, true);
            clock != nullptr && (IsClockScene(scene) || clock->clockVersion.first != 0)) {
            gSaveContext.dayTime = clock->dayTime;
            gSaveContext.skyboxTime = clock->skyboxTime;
            if (clock->clockVersion.first != 0) {
                worldClockVersions[scene] = clock->clockVersion;
            }
            return;
        }
    }
    if (IsGuardCourtyard(scene)) {
        const WorldPeer* first = nullptr;
        const auto now = Clock::now();
        for (const auto& [id, peer] : worldPeers) {
            if (IsGuardCourtyard(peer.scene) &&
                peer.age == gSaveContext.linkAge && now - peer.updated < peerTimeout && IsSharedTeamMember(id) &&
                (first == nullptr || peer.entered < first->entered)) {
                first = &peer;
            }
        }
        if (first != nullptr) {
            gSaveContext.dayTime = gSaveContext.skyboxTime = first->scene == SCENE_CASTLE_COURTYARD_GUARDS_DAY ? 0x8000 : 0;
        }
    }
}

extern "C" void Anchor_ApplyWorldTimeOnEntry(PlayState* play) {
    if (Anchor::Instance != nullptr) {
        Anchor::Instance->ApplyWorldTimeOnEntry(play);
    }
}

void Anchor::ResetWorldState() {
    ClearDynamicActors();
    ResetCombatRoom();
    localWorldPeer = {};
    worldPeers.clear();
    worldClockVersions.clear();
    nextWorldUpdate = {};
    npcAuthority = 0;
    enemyPoseAuthority = 0;
    roomObjectArea = { SCENE_ID_MAX, -1, -1, false };
    roomObjects.clear();
    spawnedTreeSkulltulas.clear();
    treeSkulltulasDirty = false;
}

void Anchor::ApplyTeamNpcPositions() {
    if (npcAuthority == 0 || npcAuthority == ownClientId || gPlayState == nullptr ||
        gPlayState->sceneNum != localWorldPeer.scene || gPlayState->roomCtx.curRoom.num != localWorldPeer.room ||
        IS_CUTSCENE_LAYER || Play_InCsMode(gPlayState)) {
        return;
    }

    const auto peer = worldPeers.find(npcAuthority);
    if (peer == worldPeers.end() || peer->second.GetArea() != localWorldPeer.GetArea() || peer->second.npcs.empty()) {
        return;
    }
    const auto& positions = peer->second.npcs;
    for (Actor* actor = gPlayState->actorCtx.actorLists[ACTORCAT_NPC].head; actor != nullptr; actor = actor->next) {
        if (!IsRoomNpc(actor) || actor->id == ACTOR_EN_HEISHI1) {
            continue;
        }
        const auto position = positions.find(NpcIndex(actor));
        if (position != positions.end() && position->second.actorId == actor->id &&
            position->second.params == actor->params) {
            actor->world.pos = position->second.position;
            actor->world.rot.y = actor->shape.rot.y = position->second.yaw;
        }
    }
}

void Anchor::ApplyDampePose(Actor* actor) {
    if (!isConnected || !SyncNpcPositionsEnabled() || npcAuthority == 0 || npcAuthority == ownClientId ||
        gPlayState->sceneNum != SCENE_GRAVEYARD || localWorldPeer.scene != SCENE_GRAVEYARD || !IsRoomNpc(actor) ||
        Play_InCsMode(gPlayState)) {
        return;
    }
    const auto peer = worldPeers.find(npcAuthority);
    if (peer == worldPeers.end() || peer->second.GetArea() != localWorldPeer.GetArea() ||
        Clock::now() - peer->second.updated >= peerTimeout) {
        return;
    }
    const auto position = peer->second.npcs.find(NpcIndex(actor));
    if (position != peer->second.npcs.end() && position->second.actorId == ACTOR_EN_TK &&
        position->second.params == actor->params) {
        actor->world.pos = position->second.position;
        actor->shape.rot.y = actor->world.rot.y = position->second.yaw;
    }
}

extern "C" void Anchor_ApplyDampePose(Actor* actor) {
    if (Anchor::Instance != nullptr) {
        Anchor::Instance->ApplyDampePose(actor);
    }
}

void Anchor::AddNpcGazeSnapshot(nlohmann::json& payload) {
    auto gazes = nlohmann::json::array();
    for (auto* actor = gPlayState->actorCtx.actorLists[ACTORCAT_NPC].head; actor != nullptr; actor = actor->next) {
        if (!IsVillageGazeNpc(actor->id) || !IsRoomNpc(actor)) {
            continue;
        }
        const int index = NpcIndex(actor);
        const auto distance = GazeDistanceBand(actor);
        const bool talking = gPlayState->msgCtx.talkActor == actor;
        if (index >= 0 && (talking || distance != INT16_MAX)) {
            const auto [head, torso] = NpcGazeRotations(actor);
            gazes.push_back({ index, actor->id, actor->params, distance, talking, *head, *torso });
        }
    }
    if (!gazes.empty()) {
        payload["npcGaze"] = std::move(gazes);
    }
}

void Anchor::ReadNpcGazeSnapshot(const nlohmann::json& payload, WorldPeer& peer) {
    const auto entries = payload.find("npcGaze");
    if (entries == payload.end() || !entries->is_array() || entries->size() > maxNpcPositions) {
        return;
    }
    for (const auto& entry : *entries) {
        const int index = entry.at(0).get<int>();
        NpcGaze gaze{ entry.at(1).get<s16>(), entry.at(2).get<s16>(), entry.at(3).get<s16>(), entry.at(4).get<bool>(),
                      entry.at(5).get<Vec3s>(), entry.at(6).get<Vec3s>() };
        if (index >= 0 && index <= INT16_MAX && IsVillageGazeNpc(gaze.actorId) && gaze.distanceBand >= 0) {
            peer.gazes.emplace(index, gaze);
        }
    }
}

void Anchor::ApplyTeamNpcGaze() {
    if (gPlayState == nullptr || gPlayState->sceneNum != SCENE_KAKARIKO_VILLAGE || !SyncNpcPositionsEnabled() ||
        localWorldPeer.scene != SCENE_KAKARIKO_VILLAGE || Play_InCsMode(gPlayState) || worldPeers.empty()) {
        return;
    }
    const auto now = Clock::now();
    const auto area = localWorldPeer.GetArea();
    const auto priority = [&](uint32_t id, bool talking, s16 distance) {
        return std::make_tuple(!talking, distance, id != roomState.ownerClientId, id);
    };
    for (auto* actor = gPlayState->actorCtx.actorLists[ACTORCAT_NPC].head; actor != nullptr; actor = actor->next) {
        if (!IsVillageGazeNpc(actor->id) || !IsRoomNpc(actor)) {
            continue;
        }
        auto best = priority(ownClientId, gPlayState->msgCtx.talkActor == actor, GazeDistanceBand(actor));
        const NpcGaze* selected = nullptr;
        const int index = NpcIndex(actor);
        for (const auto& [id, peer] : worldPeers) {
            if (peer.GetArea() != area || peer.gazes.empty() || now - peer.updated >= std::chrono::milliseconds(650) ||
                !IsSharedTeamMember(id)) {
                continue;
            }
            const auto sample = peer.gazes.find(index);
            if (sample != peer.gazes.end() && sample->second.actorId == actor->id && sample->second.params == actor->params) {
                const auto score = priority(id, sample->second.talking, sample->second.distanceBand);
                if (score < best) {
                    best = score;
                    selected = &sample->second;
                }
            }
        }
        if (selected != nullptr) {
            const auto [head, torso] = NpcGazeRotations(actor);
            *head = selected->head;
            *torso = selected->torso;
        }
    }
}

int Anchor::ApplyGuardPose(Actor* actor, s16* headAngle) {
    if (!isConnected || npcAuthority == 0 || npcAuthority == ownClientId || !IsGuardCourtyard(gPlayState->sceneNum) ||
        gPlayState->sceneNum != localWorldPeer.scene || gPlayState->roomCtx.curRoom.num != actor->room) {
        return -1;
    }
    const auto peer = worldPeers.find(npcAuthority);
    if (peer == worldPeers.end() || peer->second.GetArea() != localWorldPeer.GetArea() ||
        Clock::now() - peer->second.updated >= peerTimeout) {
        return -1;
    }
    const auto sample = peer->second.npcs.find(GetActorListIndex(actor));
    if (sample == peer->second.npcs.end() || sample->second.actorId != actor->id ||
        sample->second.params != actor->params || sample->second.patrolPhase < 0) {
        return -1;
    }
    actor->world.pos = sample->second.position;
    actor->world.rot.y = actor->shape.rot.y = sample->second.yaw;
    *headAngle = sample->second.headAngle;
    return sample->second.patrolPhase;
}

extern "C" s32 Anchor_ApplyGuardPose(Actor* actor, s16* headAngle) {
    return Anchor::Instance == nullptr ? -1 : Anchor::Instance->ApplyGuardPose(actor, headAngle);
}

void Anchor::UpdateWorldState() {
    if (!roomState.syncItemsAndFlags || !IsSaveLoaded() || ownClientId == 0) {
        localWorldPeer.scene = SCENE_ID_MAX;
        npcAuthority = 0;
        enemyPoseAuthority = 0;
        return;
    }

    const auto now = Clock::now();
    const int scene = gPlayState->sceneNum;
    const int room = gPlayState->roomCtx.curRoom.num;
    const int age = gSaveContext.linkAge;
    if (localWorldPeer.scene != scene || localWorldPeer.room != room || localWorldPeer.age != age) {
        localWorldPeer.scene = scene;
        localWorldPeer.room = room;
        localWorldPeer.age = age;
        localWorldPeer.visit = ++worldVisit;
        localWorldPeer.entered = now;
        nextWorldUpdate = now;
        npcAuthority = 0;
        enemyPoseAuthority = 0;
        UpdateRoomObjects();
    }

    if (now < nextWorldUpdate) {
        return;
    }
    const bool liveActors = IsGuardCourtyard(scene) || scene == SCENE_SHOOTING_GALLERY ||
                            scene == SCENE_KAKARIKO_VILLAGE || scene == SCENE_GRAVEYARD ||
                            ((scene == SCENE_DEKU_TREE_BOSS || (scene == SCENE_HYRULE_FIELD && !IS_DAY)) && CanShareCombat());
    nextWorldUpdate = now + (liveActors ? 100ms : worldUpdateInterval);
    std::erase_if(worldPeers, [&](const auto& entry) { return now - entry.second.updated >= peerTimeout; });

    const bool syncNpcs = SyncNpcPositionsEnabled() || IsGuardCourtyard(scene);
    const bool syncClock = SyncFieldTimeEnabled();
    const bool syncCombat = CanShareCombat();
    localWorldPeer.masterQuest = ResourceMgr_IsSceneMasterQuest(scene) != 0;
    if (scene == SCENE_HYRULE_FIELD || scene == SCENE_SHOOTING_GALLERY) {
        UpdateDynamicActors();
    }
    if (std::none_of(clients.begin(), clients.end(), [&](const auto& client) {
            return IsSharedTeamMember(client.first);
        })) {
        npcAuthority = 0;
        enemyPoseAuthority = 0;
        return;
    }

    const bool inCutscene = IS_CUTSCENE_LAYER || Play_InCsMode(gPlayState);
    nlohmann::json payload;
    payload["type"] = WORLD_STATE;
    payload["quiet"] = true;
    payload["targetTeamId"] = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
    payload["scene"] = scene;
    payload["room"] = room;
    payload["age"] = age;
    payload["masterQuest"] = localWorldPeer.masterQuest;
    payload["syncNpcs"] = syncNpcs;
    payload["syncCombat"] = syncCombat;
    payload["upgrades"] = gSaveContext.inventory.upgrades;
    payload["visit"] = localWorldPeer.visit;
    payload["areaAgeMs"] = std::min<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                 now - localWorldPeer.entered).count(),
                                             maxAreaAgeMs);
    AddRoomObjectSnapshot(payload);
    AddDynamicActorSnapshot(payload);
    if (!inCutscene && syncNpcs && scene == SCENE_KAKARIKO_VILLAGE) {
        AddNpcGazeSnapshot(payload);
    }

    npcAuthority = 0;
    if (syncNpcs) {
        const uint32_t coordinator = GetNpcCoordinator();
        const auto localArea = localWorldPeer.GetArea();
        if (coordinator == ownClientId) {
            std::set<NpcArea> areas;
            if (room >= 0) {
                areas.insert(localArea);
            }
            for (const auto& [id, peer] : worldPeers) {
                if (peer.syncNpcs && peer.visit != 0 && peer.room >= 0 && IsSharedTeamMember(id)) {
                    areas.insert(peer.GetArea());
                }
            }
            auto authorities = nlohmann::json::array();
            for (const auto& area : areas) {
                const auto [areaScene, areaRoom, areaAge, masterQuest] = area;
                const uint32_t authority = ChooseNpcAuthority(area);
                authorities.push_back({ areaScene, areaRoom, areaAge, masterQuest, authority });
                if (area == localArea) {
                    npcAuthority = authority;
                }
            }
            payload["npcAuthorities"] = std::move(authorities);
        } else {
            const auto& authorities = worldPeers.at(coordinator).npcAuthorities;
            const auto authority = authorities.find(localArea);
            if (authority != authorities.end()) {
                npcAuthority = authority->second;
            }
        }
    }

    if (!inCutscene && npcAuthority == ownClientId) {
        auto positions = nlohmann::json::array();
        Actor* firstNpc = gPlayState->actorCtx.actorLists[ACTORCAT_NPC].head;
        for (Actor* actor = firstNpc; actor != nullptr && positions.size() < maxNpcPositions; actor = actor->next) {
            const int index = NpcIndex(actor);
            if (index >= 0 && IsRoomNpc(actor)) {
                auto sample = nlohmann::json::array({ index, actor->id, actor->params, actor->world.pos.x,
                                      actor->world.pos.y, actor->world.pos.z, actor->shape.rot.y });
                if (actor->id == ACTOR_EN_HEISHI1) {
                    const auto* guard = reinterpret_cast<const EnHeishi1*>(actor);
                    sample.push_back(static_cast<s16>(guard->headAngle));
                    sample.push_back(EnHeishi1_GetPatrolPhase(guard));
                }
                positions.push_back(std::move(sample));
            }
        }
        payload["npcs"] = std::move(positions);
    }
    const auto clockVersion = GetWorldClockVersion(scene);
    const bool songReload = clockVersion.first != 0 && HasSunSongDestination();
    if (syncClock && (IsClockScene(scene) || clockVersion.first != 0) &&
        (songReload || (!inCutscene && gSaveContext.sunsSongState != SUNSSONG_SPEED_TIME))) {
        const u16 time = songReload ? gSaveContext.nextDayTime : gSaveContext.dayTime;
        const u16 sky = songReload ? time : gSaveContext.skyboxTime;
        payload["clock"] = { time, sky, clockVersion };
    }
    if (syncCombat) {
        AddEnemyDamageSnapshot(payload);
        AddEnemyPoseSnapshot(payload);
        AddGohmaSnapshot(payload);
    } else {
        enemyPoseAuthority = 0;
    }
    SendJsonToRemote(std::move(payload));
}

void Anchor::HandlePacket_WorldState(const nlohmann::json& payload) {
    const auto id = payload.at("clientId").get<uint32_t>();
    if (!CanShareRoomObjects() || !IsSharedTeamMember(id)) {
        return;
    }

    WorldPeer peer;
    peer.scene = payload.at("scene").get<int>();
    peer.room = payload.at("room").get<int>();
    peer.age = payload.at("age").get<int>();
    peer.masterQuest = payload.at("masterQuest").get<bool>();
    peer.syncNpcs = payload.at("syncNpcs").get<bool>();
    peer.syncCombat = payload.value("syncCombat", false);
    peer.visit = payload.value("visit", 0u);
    peer.updated = Clock::now();
    const int ageMs = payload.at("areaAgeMs").get<int>();
    if (peer.scene < 0 || peer.scene >= SCENE_ID_MAX || peer.room < -1 || peer.room > 127 ||
        (peer.age != LINK_AGE_CHILD && peer.age != LINK_AGE_ADULT) || ageMs < 0 || ageMs > maxAreaAgeMs) {
        return;
    }
    peer.entered = peer.updated - std::chrono::milliseconds(ageMs);
    if (payload.contains("upgrades")) {
        MergeInventoryUpgrades(gSaveContext.inventory, gSaveContext.inventory.upgrades,
                               payload.at("upgrades").get<uint32_t>());
    }
    ReadRoomObjectSnapshot(payload, peer);
    ReadDynamicActorSnapshot(payload, peer);
    if (peer.scene == SCENE_KAKARIKO_VILLAGE && SyncNpcPositionsEnabled()) {
        ReadNpcGazeSnapshot(payload, peer);
    }
    if (CanShareCombat()) {
        ReadEnemyDamageSnapshot(payload);
        ReadBabaEmergenceSnapshot(payload, peer);
        ReadEnemyPoseSnapshot(payload, peer);
        ReadGohmaSnapshot(payload, peer);
    }
    const auto previous = worldPeers.find(id);
    if (previous != worldPeers.end() && peer.visit != 0 && peer.visit == previous->second.visit &&
        peer.GetArea() == previous->second.GetArea() && peer.updated - previous->second.updated < peerTimeout) {
        peer.entered = previous->second.entered;
    }

    if ((SyncNpcPositionsEnabled() || IsGuardCourtyard(gPlayState->sceneNum)) && peer.syncNpcs &&
        payload.contains("npcAuthorities")) {
        const auto& authorities = payload.at("npcAuthorities");
        if (!authorities.is_array() || authorities.size() > clients.size()) {
            return;
        }
        for (const auto& authority : authorities) {
            const NpcArea area{ authority.at(0).get<int>(), authority.at(1).get<int>(), authority.at(2).get<int>(),
                                authority.at(3).get<bool>() };
            peer.npcAuthorities.emplace(area, authority.at(4).get<uint32_t>());
        }
    }

    if ((SyncNpcPositionsEnabled() || IsGuardCourtyard(peer.scene)) && peer.syncNpcs && payload.contains("npcs")) {
        const auto& positions = payload.at("npcs");
        if (!positions.is_array() || positions.size() > maxNpcPositions) {
            return;
        }
        for (const auto& position : positions) {
            const int index = position.at(0).get<int>();
            const Vec3f pos = { position.at(3).get<float>(), position.at(4).get<float>(), position.at(5).get<float>() };
            if (index >= 0 && std::isfinite(pos.x) && std::isfinite(pos.y) && std::isfinite(pos.z)) {
                NpcPosition sample{ position.at(1).get<s16>(), position.at(2).get<s16>(), pos,
                                    position.at(6).get<s16>() };
                if (sample.actorId == ACTOR_EN_HEISHI1 && position.size() == 9) {
                    sample.headAngle = position.at(7).get<s16>();
                    const auto phase = position.at(8).get<s16>();
                    sample.patrolPhase = phase >= 0 && phase <= 1 ? phase : -1;
                }
                peer.npcs.emplace(index, sample);
            }
        }
    }
    if (SyncFieldTimeEnabled() && payload.contains("clock")) {
        const auto& clock = payload.at("clock");
        const auto version = clock.size() > 2 ? clock.at(2).get<WorldClockVersion>() : WorldClockVersion{};
        const bool validSong = version.first > 0 && version.first <= maxClockSequence && version.second != 0;
        peer.hasClock = validSong || (version == WorldClockVersion{} && IsClockScene(peer.scene));
        peer.dayTime = clock.at(0).get<u16>();
        peer.skyboxTime = clock.at(1).get<u16>();
        peer.clockVersion = validSong ? version : WorldClockVersion{};
    }
    const bool sameSceneClock = peer.hasClock && peer.scene == gPlayState->sceneNum;
    worldPeers[id] = std::move(peer);

    if (sameSceneClock) {
        ApplyTeamWorldTime();
    }
}
