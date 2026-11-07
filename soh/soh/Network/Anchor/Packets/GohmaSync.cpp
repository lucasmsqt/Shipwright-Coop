#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/JsonConversions.hpp"
#include <cmath>

extern "C" {
#include "functions.h"
#include "overlays/actors/ovl_Boss_Goma/z_boss_goma.h"
extern PlayState* gPlayState;
}

namespace {
using namespace std::chrono_literals;
constexpr auto arrivalTie = 350ms;
constexpr auto gohmaSampleTimeout = 1200ms;
}

void Anchor::AddGohmaSnapshot(nlohmann::json& payload) {
    if (gPlayState->sceneNum != SCENE_DEKU_TREE_BOSS) {
        return;
    }
    auto* actor = Actor_Find(&gPlayState->actorCtx, ACTOR_BOSS_GOMA, ACTORCAT_BOSS);
    if (actor == nullptr || actor->init != nullptr || actor->update == nullptr) {
        return;
    }
    GohmaTeamState state{};
    BossGoma_GetTeamState(reinterpret_cast<const BossGoma*>(actor), &state);
    payload["gohma"] = { state.position, state.rotation, state.speed, state.verticalSpeed, state.gravity,
                          state.animationFrame, state.animationSpeed, state.phase, state.actionState, state.frameCount,
                          state.actionTimer, state.timer, state.patience, state.eyeClosed, state.eyeState,
                          state.visualState, state.children, state.childGenerations };
}

void Anchor::ReadGohmaSnapshot(const nlohmann::json& payload, WorldPeer& peer) {
    const auto entry = payload.find("gohma");
    if (peer.scene != SCENE_DEKU_TREE_BOSS || !peer.syncCombat || entry == payload.end() || !entry->is_array() ||
        entry->size() != 18) {
        return;
    }
    const auto& data = *entry;
    GohmaTeamState state{ data[0].get<Vec3f>(), data[1].get<Vec3s>(), data[2].get<float>(), data[3].get<float>(),
                          data[4].get<float>(), data[5].get<float>(), data[6].get<float>(), data[7].get<s16>(),
                          data[8].get<s16>(), data[9].get<s16>(), data[10].get<s16>(), data[11].get<s16>(),
                          data[12].get<s16>(), data[13].get<s16>(), data[14].get<s16>(), data[15].get<s16>(),
                          { data[16].at(0).get<s16>(), data[16].at(1).get<s16>(), data[16].at(2).get<s16>() },
                          { data[17].at(0).get<u16>(), data[17].at(1).get<u16>(), data[17].at(2).get<u16>() } };
    const auto bounded = [](float value, float limit) { return std::isfinite(value) && std::abs(value) <= limit; };
    if (state.phase < GOHMA_TEAM_ENCOUNTER || state.phase >= GOHMA_TEAM_PHASE_COUNT || state.eyeState < 0 ||
        state.eyeState > 2 || state.visualState < 0 || state.visualState > 5 ||
        !bounded(state.position.x, 4000) || !bounded(state.position.y, 4000) || !bounded(state.position.z, 4000) ||
        !bounded(state.speed, 100) || !bounded(state.verticalSpeed, 100) || !bounded(state.gravity, 20) ||
        !bounded(state.animationFrame, 1000) || !bounded(state.animationSpeed, 20)) {
        return;
    }
    peer.gohma = state;
}

uint32_t Anchor::GetGohmaChildDefeats(const GohmaTeamState* state) {
    if (!isConnected || !CanShareCombat() || localWorldPeer.scene != SCENE_DEKU_TREE_BOSS ||
        localWorldPeer.room != gPlayState->roomCtx.curRoom.num || gPlayState->sceneNum != SCENE_DEKU_TREE_BOSS) {
        return 0;
    }
    uint32_t defeats = 0;
    const auto now = std::chrono::steady_clock::now();
    for (const auto& [id, peer] : worldPeers) {
        if (!peer.gohma || peer.GetArea() != localWorldPeer.GetArea() || now - peer.updated >= gohmaSampleTimeout ||
            !IsSharedTeamMember(id)) {
            continue;
        }
        for (int i = 0; i < 3; ++i) {
            if (state->children[i] == 1 && peer.gohma->children[i] == -1 &&
                peer.gohma->childGenerations[i] == state->childGenerations[i]) {
                defeats |= 1u << i;
            }
        }
    }
    return defeats;
}

bool Anchor::ReadGohmaState(Actor* actor, GohmaTeamState* state, u64* seen) {
    if (!isConnected || !CanShareCombat() || gPlayState->sceneNum != SCENE_DEKU_TREE_BOSS ||
        localWorldPeer.scene != SCENE_DEKU_TREE_BOSS || actor->room != localWorldPeer.room ||
        actor->room != gPlayState->roomCtx.curRoom.num) {
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto area = localWorldPeer.GetArea();
    const auto eligible = [&](uint32_t id, const WorldPeer& peer) {
        return peer.gohma && peer.GetArea() == area && now - peer.updated < gohmaSampleTimeout && IsSharedTeamMember(id);
    };
    auto first = localWorldPeer.entered;
    for (const auto& [id, peer] : worldPeers) {
        if (eligible(id, peer) && peer.entered < first) {
            first = peer.entered;
        }
    }
    uint32_t authority = localWorldPeer.entered <= first + arrivalTie ? ownClientId : 0;
    const WorldPeer* sample = nullptr;
    for (const auto& [id, peer] : worldPeers) {
        if (eligible(id, peer) && peer.entered <= first + arrivalTie &&
            (authority == 0 || id == roomState.ownerClientId || (authority != roomState.ownerClientId && id < authority))) {
            authority = id;
            sample = &peer;
        }
    }
    if (sample == nullptr) {
        // A teammate can start the reveal by looking up while the first arrival is looking away.
        GohmaTeamState local{};
        BossGoma_GetTeamState(reinterpret_cast<const BossGoma*>(actor), &local);
        if (local.phase == GOHMA_TEAM_ENCOUNTER && local.actionState == 3) {
            for (const auto& [id, peer] : worldPeers) {
                if (eligible(id, peer) && peer.gohma->phase == GOHMA_TEAM_ENCOUNTER && peer.gohma->actionState >= 4) {
                    sample = &peer;
                    break;
                }
            }
        }
    }
    if (sample == nullptr) {
        return false;
    }
    *state = *sample->gohma;
    *seen = static_cast<u64>(sample->updated.time_since_epoch().count());
    return true;
}

extern "C" s32 Anchor_ReadGohmaState(Actor* actor, GohmaTeamState* state, u64* seen) {
    return Anchor::Instance != nullptr && Anchor::Instance->ReadGohmaState(actor, state, seen);
}

extern "C" u32 Anchor_GetGohmaChildDefeats(const GohmaTeamState* state) {
    return Anchor::Instance != nullptr ? Anchor::Instance->GetGohmaChildDefeats(state) : 0;
}
