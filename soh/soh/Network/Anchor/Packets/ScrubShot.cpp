#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/ScrubShot.h"
#include "soh/ResourceManagerHelpers.h"
#include <algorithm>
#include <cmath>

extern "C" {
#include "functions.h"
#include "macros.h"
#include "overlays/actors/ovl_En_Nutsball/z_en_nutsball.h"
extern PlayState* gPlayState;
}

namespace {
enum ShotEvent { SHOT_SPAWN, SHOT_REFLECT, SHOT_END };

auto TargetPriority(const Vec3f& target, const Actor* scrub, uint32_t id, uint32_t roomOwner) {
    const float dx = target.x - scrub->world.pos.x;
    const float dz = target.z - scrub->world.pos.z;
    const int distanceBand = static_cast<int>(std::sqrt(dx * dx + dz * dz) / 50.0f);
    return std::make_tuple(distanceBand, id != roomOwner, id);
}
} // namespace

extern "C" s32 Anchor_ShouldSpawnScrubShot(Actor* scrub) {
    return Anchor::Instance == nullptr || Anchor::Instance->ShouldSpawnScrubShot(scrub);
}

extern "C" void Anchor_SendScrubShotSpawn(Actor* actor) {
    if (Anchor::Instance != nullptr) {
        Anchor::Instance->SendScrubShot(actor, SHOT_SPAWN);
    }
}

extern "C" void Anchor_SendScrubShotReflect(Actor* actor) {
    if (Anchor::Instance != nullptr) {
        Anchor::Instance->SendScrubShot(actor, SHOT_REFLECT);
    }
}

extern "C" void Anchor_SendScrubShotEnd(Actor* actor) {
    if (Anchor::Instance != nullptr) {
        Anchor::Instance->SendScrubShot(actor, SHOT_END);
    }
}

bool Anchor::IsScrubShotPeer(uint32_t id, const WorldPeer& peer) {
    return peer.syncCombat && peer.scene == gPlayState->sceneNum && peer.room == gPlayState->roomCtx.curRoom.num &&
           peer.age == gSaveContext.linkAge &&
           std::chrono::steady_clock::now() - peer.updated < std::chrono::seconds(2) && IsSharedTeamMember(id) &&
           peer.masterQuest == (ResourceMgr_IsSceneMasterQuest(peer.scene) != 0);
}

bool Anchor::ShouldSpawnScrubShot(Actor* scrub) {
    return FindNearestCombatController(scrub) == ownClientId;
}

uint32_t Anchor::FindNearestCombatController(Actor* scrub) {
    if (!isConnected || !CanShareCombat() || scrub->room != gPlayState->roomCtx.curRoom.num) {
        return ownClientId;
    }
    auto best = TargetPriority(GET_PLAYER(gPlayState)->actor.world.pos, scrub, ownClientId, roomState.ownerClientId);
    uint32_t controller = ownClientId;
    for (const auto& [id, peer] : worldPeers) {
        if (!IsScrubShotPeer(id, peer)) {
            continue;
        }
        const auto& client = clients.at(id);
        const auto priority = TargetPriority(client.posRot.pos, scrub, id, roomState.ownerClientId);
        if (client.sceneNum == peer.scene && client.curRoomNum == peer.room &&
            std::chrono::steady_clock::now() - client.lastPlayerUpdate < std::chrono::seconds(2) && priority < best) {
            best = priority;
            controller = id;
        }
    }
    return controller;
}

void Anchor::SendScrubShot(Actor* actor, int event) {
    auto* shot = reinterpret_cast<EnNutsball*>(actor);
    if (!isConnected || !CanShareCombat() || actor->room != gPlayState->roomCtx.curRoom.num) {
        return;
    }
    if (event == SHOT_SPAWN) {
        if (ownClientId == 0 || std::none_of(worldPeers.begin(), worldPeers.end(), [&](const auto& entry) {
                return IsScrubShotPeer(entry.first, entry.second);
            })) {
            return;
        }
        EnNutsball_MarkShared(shot, ownClientId, ++scrubShotSerial, false);
    } else if (shot->sharedOwner == 0) {
        return;
    }
    nlohmann::json payload;
    payload["type"] = SCRUB_SHOT;
    payload["quiet"] = true;
    payload["targetTeamId"] = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
    payload["scene"] = gPlayState->sceneNum;
    payload["room"] = actor->room;
    payload["age"] = gSaveContext.linkAge;
    payload["masterQuest"] = ResourceMgr_IsSceneMasterQuest(gPlayState->sceneNum) != 0;
    payload["event"] = event;
    payload["owner"] = shot->sharedOwner;
    payload["shot"] = shot->sharedSerial;
    if (event != SHOT_END) {
        const auto& pos = actor->world.pos;
        const auto& rot = actor->world.rot;
        payload["pos"] = { pos.x, pos.y, pos.z };
        payload["rot"] = { rot.x, rot.y, rot.z };
        payload["params"] = actor->params;
    }
    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_ScrubShot(const nlohmann::json& payload) {
    const auto sender = payload.at("clientId").get<uint32_t>();
    if (!CanShareCombat() || !IsSharedTeamMember(sender) || payload.at("scene").get<int>() != gPlayState->sceneNum ||
        payload.at("room").get<int>() != gPlayState->roomCtx.curRoom.num ||
        payload.at("age").get<int>() != gSaveContext.linkAge ||
        payload.at("masterQuest").get<bool>() != (ResourceMgr_IsSceneMasterQuest(gPlayState->sceneNum) != 0)) {
        return;
    }
    const int event = payload.at("event").get<int>();
    const auto owner = payload.at("owner").get<uint32_t>();
    const auto serial = payload.at("shot").get<uint32_t>();
    if (owner == 0 || serial == 0 || (event != SHOT_REFLECT && sender != owner)) {
        return;
    }
    EnNutsball* shot = nullptr;
    for (Actor* actor = gPlayState->actorCtx.actorLists[ACTORCAT_PROP].head; actor != nullptr; actor = actor->next) {
        if (actor->id == ACTOR_EN_NUTSBALL && actor->room == gPlayState->roomCtx.curRoom.num && actor->update != nullptr) {
            auto* candidate = reinterpret_cast<EnNutsball*>(actor);
            if (candidate->sharedOwner == owner && candidate->sharedSerial == serial) {
                shot = candidate;
                break;
            }
        }
    }
    if (event == SHOT_END) {
        if (shot != nullptr && !shot->sharedReflected) {
            Actor_Kill(&shot->actor);
        }
        return;
    }
    if ((event == SHOT_SPAWN && shot != nullptr) ||
        (event == SHOT_REFLECT && (shot == nullptr || shot->sharedReflected)) ||
        (event != SHOT_SPAWN && event != SHOT_REFLECT)) {
        return;
    }
    const auto posValues = payload.at("pos").get<std::array<float, 3>>();
    const auto rot = payload.at("rot").get<std::array<s16, 3>>();
    if (std::any_of(posValues.begin(), posValues.end(),
                    [](float value) { return !std::isfinite(value) || std::abs(value) > 100000.0f; })) {
        return;
    }
    Vec3f pos{ posValues[0], posValues[1], posValues[2] };
    if (event == SHOT_REFLECT) {
        EnNutsball_Reflect(shot, &pos, rot[1]);
        return;
    }
    const int params = payload.at("params").get<int>();
    if (params < 0 || params > 2) {
        return;
    }
    auto* actor = Actor_Spawn(&gPlayState->actorCtx, gPlayState, ACTOR_EN_NUTSBALL, pos.x, pos.y, pos.z, rot[0], rot[1],
                              rot[2], static_cast<s16>(params));
    if (actor != nullptr) {
        EnNutsball_MarkShared(reinterpret_cast<EnNutsball*>(actor), owner, serial, true);
        SoundSource_PlaySfxAtFixedWorldPos(gPlayState, &pos, 20, NA_SE_EN_NUTS_THROW);
    }
}
