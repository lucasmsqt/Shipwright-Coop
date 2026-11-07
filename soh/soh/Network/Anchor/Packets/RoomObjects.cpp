#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/JsonConversions.hpp"
#include "soh/Network/Anchor/RoomObjects.h"
#include "soh/ObjectExtension/ActorListIndex.h"
#include "soh/ResourceManagerHelpers.h"
#include <algorithm>
#include <cmath>

extern "C" {
#include "functions.h"
#include "macros.h"
extern PlayState* gPlayState;
}

namespace {
constexpr size_t maxRoomObjects = 128;
constexpr auto bombFlowerFrame = std::chrono::milliseconds(50);
constexpr int bombFlowerRegrowthFrames = 200;

bool IsBreakable(int id) {
    return id == ACTOR_OBJ_TSUBO || id == ACTOR_OBJ_KIBAKO || id == ACTOR_OBJ_KIBAKO2;
}

bool IsTransientRoomObject(int phase) {
    return phase == ANCHOR_DOOR_OPENING || phase == ANCHOR_SUN_STUN;
}

bool IsRoomTransitionDoor(const Actor* actor) {
    return actor->category == ACTORCAT_DOOR && (actor->id == ACTOR_EN_DOOR || actor->id == ACTOR_DOOR_SHUTTER);
}
}

bool Anchor::CanShareRoomObjects() {
    return isConnected && ownClientId != 0 && roomState.syncItemsAndFlags && IsSaveLoaded() &&
           gPlayState->roomCtx.curRoom.num >= 0;
}

Anchor::NpcArea Anchor::GetRoomObjectArea() {
    return { gPlayState->sceneNum, gPlayState->roomCtx.curRoom.num, gSaveContext.linkAge,
             ResourceMgr_IsSceneMasterQuest(gPlayState->sceneNum) != 0 };
}

Anchor::RoomObjectKey Anchor::GetRoomObjectKey(const Actor* actor) {
    const int index = GetActorListIndex(actor);
    if (index < 0 && actor->id == ACTOR_OBJ_OSHIHIKI && actor->parent != nullptr &&
        (actor->parent->id == ACTOR_OBJ_MAKEOSHIHIKI || actor->parent->id == ACTOR_OBJ_LIGHTSWITCH)) {
        return { actor->id, GetActorListIndex(actor->parent), actor->parent->id };
    }
    return { actor->id, index, 0 };
}

Anchor::RoomObjectState Anchor::DecodeRoomObject(const nlohmann::json& entry) {
    const auto version = entry.at(1).get<std::array<uint32_t, 2>>();
    const int phase = entry.at(3).get<int>();
    const int age = phase == ANCHOR_BOMB_FLOWER_PLUCKED && entry.size() > 5
                        ? std::clamp(entry.at(5).get<int>(), 0, bombFlowerRegrowthFrames)
                        : 0;
    return { static_cast<uint64_t>(version[0]) << 32 | version[1], entry.at(2).get<Vec3f>(), phase,
             entry.at(4).get<int>(), std::chrono::steady_clock::now() - bombFlowerFrame * age };
}

bool Anchor::IsValidRoomObject(const RoomObjectKey& key, const RoomObjectState& state) {
    const auto inBounds = [](float v) { return std::isfinite(v) && std::abs(v) < 100000.0f; };
    if (key[1] < 0 || key[1] > 32767 || state.version >> 32 == 0 || state.version >> 32 >= UINT32_MAX ||
        static_cast<uint32_t>(state.version) == 0 || !inBounds(state.position.x) || !inBounds(state.position.y) ||
        !inBounds(state.position.z)) {
        return false;
    }
    if (key[0] == ACTOR_OBJ_OSHIHIKI) {
        return (key[2] == 0 || key[2] == ACTOR_OBJ_MAKEOSHIHIKI || key[2] == ACTOR_OBJ_LIGHTSWITCH) &&
               (state.phase == ANCHOR_BLOCK_MOVING || state.phase == ANCHOR_BLOCK_SETTLED);
    }
    if (IsBreakable(key[0])) {
        return key[2] == 0 && state.phase == ANCHOR_OBJECT_BROKEN && (state.detail == 0 || state.detail == 1);
    }
    if (key[0] == ACTOR_EN_SW) {
        return key[2] == 0 && key[1] >= 0x100 && (key[1] >> 8) <= static_cast<int>(std::size(gSaveContext.gsFlags)) * 4 &&
               (key[1] & 0xFF) != 0 &&
               state.phase == ANCHOR_TREE_SKULLTULA && state.detail >= INT16_MIN && state.detail <= INT16_MAX;
    }
    if (key[0] == ACTOR_EN_TA) {
        return key[2] == 0 && state.phase == ANCHOR_TALON_AWAKENING && state.detail == 0;
    }
    if (key[0] == ACTOR_EN_DOOR || key[0] == ACTOR_DOOR_SHUTTER) {
        return key[1] < 64 && (key[2] == 0 || (key[0] == ACTOR_EN_DOOR && key[2] == 1)) &&
               state.phase == ANCHOR_DOOR_OPENING && state.detail >= 0 &&
               (key[0] == ACTOR_EN_DOOR ? state.detail < 8 : state.detail == 0);
    }
    if (key[0] == ACTOR_OCEFF_SPOT) {
        return key[1] == 0 && key[2] == 0 && state.phase == ANCHOR_SUN_STUN && state.detail == 0;
    }
    if (key[0] == ACTOR_OBJ_SYOKUDAI) {
        return key[2] == 0 && state.phase == ANCHOR_TORCH_STATE && state.detail >= -1 && state.detail <= 1310;
    }
    if (key[0] == ACTOR_EN_BOMBF) {
        return key[2] == 0 && state.phase == ANCHOR_BOMB_FLOWER_PLUCKED && state.detail == 180;
    }
    if (key[0] == ACTOR_BG_DODOAGO) {
        return key[1] < 64 && (key[2] == 0 || key[2] == 1) && state.phase == ANCHOR_DODONGO_EYE_LIT &&
               state.detail == 0;
    }
    return key[0] == ACTOR_EN_HINTNUTS && key[2] == 0 &&
           (state.phase == ANCHOR_SCRUB_HIT || state.phase == ANCHOR_SCRUB_LEFT) &&
           state.detail >= -4 && state.detail <= 3;
}

void Anchor::MergeRoomObject(const RoomObjectKey& key, const RoomObjectState& state) {
    const auto existing = roomObjects.find(key);
    if (existing != roomObjects.end()) {
        const bool expiredPlant = state.phase == ANCHOR_BOMB_FLOWER_PLUCKED &&
            std::chrono::steady_clock::now() - existing->second.received >= bombFlowerFrame * bombFlowerRegrowthFrames;
        if ((expiredPlant || state.version > existing->second.version) && existing->second.phase != ANCHOR_SCRUB_LEFT &&
            existing->second.phase != ANCHOR_OBJECT_BROKEN && existing->second.phase != ANCHOR_TREE_SKULLTULA) {
            existing->second = state;
        }
    } else if (roomObjects.size() < maxRoomObjects) {
        roomObjects.emplace(key, state);
        treeSkulltulasDirty |= state.phase == ANCHOR_TREE_SKULLTULA;
        if ((state.phase == ANCHOR_OBJECT_BROKEN || state.phase == ANCHOR_TALON_AWAKENING ||
             state.phase == ANCHOR_TORCH_STATE) &&
            static_cast<uint32_t>(state.version) != ownClientId) {
            for (int category : { ACTORCAT_PROP, ACTORCAT_BG, ACTORCAT_NPC }) {
                for (auto* actor = gPlayState->actorCtx.actorLists[category].head; actor != nullptr; actor = actor->next) {
                    if (actor->id == key[0] && actor->room == std::get<1>(roomObjectArea) &&
                        GetActorListIndex(actor) == key[1]) {
                        actor->flags |= ACTOR_FLAG_UPDATE_CULLING_DISABLED;
                    }
                }
            }
        }
    }
}

void Anchor::UpdateRoomObjects() {
    if (std::get<0>(roomObjectArea) == gPlayState->sceneNum && std::get<1>(roomObjectArea) == gPlayState->roomCtx.curRoom.num &&
        std::get<2>(roomObjectArea) == gSaveContext.linkAge) {
        return;
    }
    const auto area = GetRoomObjectArea();
    roomObjectArea = area;
    roomObjects.clear();
    spawnedTreeSkulltulas.clear();
    treeSkulltulasDirty = false;
    const auto now = std::chrono::steady_clock::now();
    for (const auto& [id, peer] : worldPeers) {
        if (peer.GetArea() == area && now - peer.updated < std::chrono::seconds(2) && IsSharedTeamMember(id)) {
            for (const auto& [key, state] : peer.objects) {
                MergeRoomObject(key, state);
            }
        }
    }
}

void Anchor::SendRoomObject(Actor* actor, int phase, int detail) {
    if (!CanShareRoomObjects() || (actor->room != gPlayState->roomCtx.curRoom.num &&
        !(actor->room < 0 && phase == ANCHOR_BOMB_FLOWER_PLUCKED))) {
        return;
    }
    const auto key = GetRoomObjectKey(actor);
    if (key[1] < 0) {
        return;
    }
    UpdateRoomObjects();
    PublishRoomObject(key, actor->world.pos, phase, detail);
}

void Anchor::RecordDodongoEye(Actor* actor, int eye) {
    if (!CanShareRoomObjects() || gPlayState->sceneNum != SCENE_DODONGOS_CAVERN ||
        (actor->room >= 0 && actor->room != gPlayState->roomCtx.curRoom.num)) {
        return;
    }
    UpdateRoomObjects();
    const RoomObjectKey key{ ACTOR_BG_DODOAGO, actor->params & 0x3F, eye };
    if (!roomObjects.contains(key)) {
        PublishRoomObject(key, actor->world.pos, ANCHOR_DODONGO_EYE_LIT, 0);
    }
}

int Anchor::GetDodongoEyes(Actor* actor) {
    if (!CanShareRoomObjects() || gPlayState->sceneNum != SCENE_DODONGOS_CAVERN ||
        (actor->room >= 0 && actor->room != gPlayState->roomCtx.curRoom.num)) {
        return 0;
    }
    UpdateRoomObjects();
    const int flag = actor->params & 0x3F;
    const int leftEye = roomObjects.contains({ ACTOR_BG_DODOAGO, flag, 0 });
    const int rightEye = roomObjects.contains({ ACTOR_BG_DODOAGO, flag, 1 });
    return leftEye | (rightEye << 1);
}

void Anchor::PublishRoomObject(const RoomObjectKey& key, const Vec3f& position, int phase, int detail) {
    const auto existing = roomObjects.find(key);
    const uint64_t previous = existing == roomObjects.end() ? 0 : existing->second.version >> 32;
    const bool useRoomSequence = IsTransientRoomObject(phase) || phase == ANCHOR_BOMB_FLOWER_PLUCKED;
    const uint64_t sequence = useRoomSequence ?
        std::max<uint64_t>(transientRoomSequence, previous) + 1 : previous + 1;
    RoomObjectState state{ sequence << 32 | ownClientId, position, phase, detail, std::chrono::steady_clock::now() };
    if (!IsValidRoomObject(key, state)) {
        return;
    }
    if (useRoomSequence) {
        transientRoomSequence = static_cast<uint32_t>(sequence);
    }
    MergeRoomObject(key, state);
    if (std::none_of(clients.begin(), clients.end(), [&](const auto& client) { return IsSharedTeamMember(client.first); })) {
        return;
    }
    const auto [scene, room, age, mq] = roomObjectArea;
    nlohmann::json payload = { { "type", ROOM_OBJECT }, { "quiet", true },
                               { "targetTeamId", CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default") },
                               { "scene", scene }, { "room", room }, { "age", age }, { "masterQuest", mq },
                               { "object", { key, { sequence, ownClientId }, state.position, state.phase, state.detail } } };
    SendJsonToRemote(payload);
}

void Anchor::SendDoorOpening(Actor* actor, int leaf, int style) {
    if (CanShareRoomObjects() && IsRoomTransitionDoor(actor)) {
        UpdateRoomObjects();
        PublishRoomObject({ actor->id, static_cast<u16>(actor->params) >> 10, leaf }, actor->home.pos,
                          ANCHOR_DOOR_OPENING, style);
    }
}

int Anchor::ReadDoorOpening(Actor* actor, int leaf, u64* seen) {
    if (!CanShareRoomObjects() || !IsRoomTransitionDoor(actor)) {
        return -1;
    }
    UpdateRoomObjects();
    const auto it = roomObjects.find({ actor->id, static_cast<u16>(actor->params) >> 10, leaf });
    if (it == roomObjects.end() || it->second.version <= *seen) {
        return -1;
    }
    const auto& state = it->second;
    *seen = state.version;
    if (state.phase != ANCHOR_DOOR_OPENING || static_cast<uint32_t>(state.version) == ownClientId ||
        std::chrono::steady_clock::now() - state.received >= std::chrono::seconds(1)) {
        return -1;
    }
    return state.detail;
}

void Anchor::StartSunStun(PlayState* play) {
    if (play != gPlayState || !CanShareRoomObjects()) {
        return;
    }
    UpdateRoomObjects();
    const auto* player = GET_PLAYER(play);
    PublishRoomObject({ ACTOR_OCEFF_SPOT, 0, 0 }, player->actor.world.pos, ANCHOR_SUN_STUN, 0);
}

int Anchor::ConsumeSunStun(Actor* actor, u64* seen) {
    if (!CanShareRoomObjects() || (actor->room >= 0 && actor->room != gPlayState->roomCtx.curRoom.num)) {
        return 0;
    }
    UpdateRoomObjects();
    const auto it = roomObjects.find({ ACTOR_OCEFF_SPOT, 0, 0 });
    if (it == roomObjects.end() || it->second.version <= *seen) {
        return 0;
    }
    const auto& state = it->second;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - state.received);
    if (elapsed >= std::chrono::seconds(30)) {
        *seen = state.version;
        return 0;
    }
    *seen = state.version;
    return 600 - static_cast<int>(elapsed.count() / 50);
}

void Anchor::SpawnTreeSkulltula(PlayState* play, Vec3f* position, s16 yaw, s16 params) {
    if (!CanShareRoomObjects()) {
        Actor_Spawn(&play->actorCtx, play, ACTOR_EN_SW, position->x, position->y, position->z, 0, yaw, 0, params);
        return;
    }
    UpdateRoomObjects();
    const RoomObjectKey key{ ACTOR_EN_SW, params & 0x1FFF, 0 };
    if (!roomObjects.contains(key) &&
        Actor_Spawn(&play->actorCtx, play, ACTOR_EN_SW, position->x, position->y, position->z, 0, yaw, 0, params) != nullptr) {
        spawnedTreeSkulltulas.insert(key);
        PublishRoomObject(key, *position, ANCHOR_TREE_SKULLTULA, yaw);
    }
}

void Anchor::ApplyTreeSkulltulas() {
    if (!treeSkulltulasDirty || !CanShareRoomObjects()) {
        return;
    }
    UpdateRoomObjects();
    treeSkulltulasDirty = false;
    for (const auto& [key, state] : roomObjects) {
        if (state.phase != ANCHOR_TREE_SKULLTULA || spawnedTreeSkulltulas.contains(key)) {
            continue;
        }
        if ((GET_GS_FLAGS((key[1] >> 8) - 1) & (key[1] & 0xFF)) != 0 ||
            Actor_Spawn(&gPlayState->actorCtx, gPlayState, ACTOR_EN_SW, state.position.x, state.position.y,
                        state.position.z, 0, state.detail, 0, key[1] | 0xE000) != nullptr) {
            spawnedTreeSkulltulas.insert(key);
        } else {
            treeSkulltulasDirty = true;
        }
    }
}

int Anchor::ReadRoomObject(Actor* actor, Vec3f* position, s32* detail, u64* seen) {
    if (!CanShareRoomObjects() || (actor->room != gPlayState->roomCtx.curRoom.num &&
        !(actor->room < 0 && actor->id == ACTOR_EN_BOMBF))) {
        return 0;
    }
    UpdateRoomObjects();
    if (roomObjects.empty()) {
        return 0;
    }
    const auto state = roomObjects.find(GetRoomObjectKey(actor));
    if (state == roomObjects.end() ||
        (actor->id == ACTOR_EN_BOMBF ? state->second.version == *seen : state->second.version <= *seen)) {
        return 0;
    }
    const auto& sample = state->second;
    *seen = sample.version;
    if (static_cast<uint32_t>(sample.version) == ownClientId) {
        return 0;
    }
    *position = sample.position;
    *detail = sample.detail;
    if (sample.phase == ANCHOR_BOMB_FLOWER_PLUCKED) {
        const auto age = (std::chrono::steady_clock::now() - sample.received) / bombFlowerFrame;
        if (age >= bombFlowerRegrowthFrames) {
            return 0;
        }
        *detail -= static_cast<int>(age);
    }
    return sample.phase;
}

void Anchor::HandlePacket_RoomObject(const nlohmann::json& payload) {
    const auto sender = payload.at("clientId").get<uint32_t>();
    if (!CanShareRoomObjects() || !IsSharedTeamMember(sender)) {
        return;
    }
    const NpcArea area{ payload.at("scene").get<int>(), payload.at("room").get<int>(), payload.at("age").get<int>(),
                        payload.at("masterQuest").get<bool>() };
    if (area != GetRoomObjectArea()) {
        return;
    }
    const auto& entry = payload.at("object");
    const auto key = entry.at(0).get<RoomObjectKey>();
    const auto state = DecodeRoomObject(entry);
    if (static_cast<uint32_t>(state.version) == sender && IsValidRoomObject(key, state)) {
        UpdateRoomObjects();
        if (IsTransientRoomObject(state.phase) || state.phase == ANCHOR_BOMB_FLOWER_PLUCKED) {
            transientRoomSequence = std::max(transientRoomSequence, static_cast<uint32_t>(state.version >> 32));
        }
        MergeRoomObject(key, state);
    }
}

void Anchor::AddRoomObjectSnapshot(nlohmann::json& payload) {
    UpdateRoomObjects();
    if (!roomObjects.empty()) {
        auto objects = nlohmann::json::array();
        for (const auto& [key, state] : roomObjects) {
            if (IsTransientRoomObject(state.phase)) {
                continue;
            }
            auto object = nlohmann::json::array({ key, { state.version >> 32, static_cast<uint32_t>(state.version) },
                                                  state.position, state.phase, state.detail });
            if (state.phase == ANCHOR_BOMB_FLOWER_PLUCKED) {
                const auto age = (std::chrono::steady_clock::now() - state.received) / bombFlowerFrame;
                if (age >= bombFlowerRegrowthFrames) {
                    continue;
                }
                object.push_back(age);
            }
            objects.push_back(std::move(object));
        }
        if (!objects.empty()) {
            payload["roomObjects"] = std::move(objects);
        }
    }
}

void Anchor::ReadRoomObjectSnapshot(const nlohmann::json& payload, WorldPeer& peer) {
    const auto entries = payload.find("roomObjects");
    if (entries == payload.end() || !entries->is_array() || entries->size() > maxRoomObjects) {
        return;
    }
    UpdateRoomObjects();
    const bool sameRoom = peer.GetArea() == roomObjectArea;
    for (const auto& entry : *entries) {
        const auto key = entry.at(0).get<RoomObjectKey>();
        const auto state = DecodeRoomObject(entry);
        if (!IsTransientRoomObject(state.phase) && IsValidRoomObject(key, state)) {
            peer.objects.emplace(key, state);
            if (sameRoom) {
                MergeRoomObject(key, state);
            }
        }
    }
}

bool Anchor::ShouldUpdateSharedScrub(Actor* actor) {
    return isConnected && CanShareCombat() && actor->room == gPlayState->roomCtx.curRoom.num;
}

extern "C" void Anchor_SendRoomObject(Actor* actor, s32 phase, s32 detail) {
    if (Anchor::Instance != nullptr) {
        Anchor::Instance->SendRoomObject(actor, phase, detail);
    }
}

extern "C" s32 Anchor_ReadRoomObject(Actor* actor, Vec3f* position, s32* detail, u64* seen) {
    return Anchor::Instance != nullptr ? Anchor::Instance->ReadRoomObject(actor, position, detail, seen) : 0;
}

extern "C" s32 Anchor_ShouldUpdateSharedScrub(Actor* actor) {
    return Anchor::Instance != nullptr && Anchor::Instance->ShouldUpdateSharedScrub(actor);
}

extern "C" s32 Anchor_ReadBreakable(Actor* actor, Vec3f* position) {
    u64 seen = 0;
    s32 water = 0;
    return Anchor_ReadRoomObject(actor, position, &water, &seen) == ANCHOR_OBJECT_BROKEN ? water + 1 : 0;
}

extern "C" void Anchor_SpawnTreeSkulltula(PlayState* play, Vec3f* position, s16 yaw, s16 params) {
    if (Anchor::Instance != nullptr) {
        Anchor::Instance->SpawnTreeSkulltula(play, position, yaw, params);
    } else {
        Actor_Spawn(&play->actorCtx, play, ACTOR_EN_SW, position->x, position->y, position->z, 0, yaw, 0, params);
    }
}

extern "C" void Anchor_SendDoorOpening(Actor* actor, s32 leaf, s32 style) {
    if (Anchor::Instance != nullptr) {
        Anchor::Instance->SendDoorOpening(actor, leaf, style);
    }
}

extern "C" s32 Anchor_ReadDoorOpening(Actor* actor, s32 leaf, u64* seen) {
    return Anchor::Instance == nullptr ? -1 : Anchor::Instance->ReadDoorOpening(actor, leaf, seen);
}

extern "C" void Anchor_StartSunStun(PlayState* play) {
    if (Anchor::Instance != nullptr) {
        Anchor::Instance->StartSunStun(play);
    }
}

extern "C" s32 Anchor_ConsumeSunStun(Actor* actor, u64* seen) {
    return Anchor::Instance == nullptr ? 0 : Anchor::Instance->ConsumeSunStun(actor, seen);
}

extern "C" void Anchor_RecordDodongoEye(Actor* actor, s32 eye) {
    if (Anchor::Instance != nullptr) {
        Anchor::Instance->RecordDodongoEye(actor, eye);
    }
}

extern "C" s32 Anchor_GetDodongoEyes(Actor* actor) {
    return Anchor::Instance == nullptr ? 0 : Anchor::Instance->GetDodongoEyes(actor);
}
