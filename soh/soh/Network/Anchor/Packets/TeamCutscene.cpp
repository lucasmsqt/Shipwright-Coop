#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/BossWarp.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/ResourceManagerHelpers.h"
#include "soh/resource/type/Cutscene.h"
#include "scenes/dungeons/ydan/ydan_scene.h"
#include "scenes/misc/hakaana_ouke/hakaana_ouke_scene.h"
#include "scenes/overworld/spot02/spot02_scene.h"
#include "scenes/overworld/spot04/spot04_scene.h"
#include <algorithm>
#include <cstring>
#include <string_view>

extern "C" {
#include "functions.h"
#include "macros.h"
#include "overlays/actors/ovl_Bg_Treemouth/z_bg_treemouth.h"
#include "overlays/actors/ovl_En_Zl4/z_en_zl4.h"
extern PlayState* gPlayState;
extern CutsceneData D_808BCE20[];
extern CutsceneData D_808BD2A0[];
s32 EnSa_StartTeamGreeting(Actor* actor, PlayState* play);
}

namespace {
enum CutsceneId {
    SARIA_BRIDGE, ZELDA_ESCAPE, DEKU_TREE_MEETING, DEKU_TREE_QUESTION, DEKU_TREE_ENTRANCE, GOHMA_EXIT_STORY,
    GOHMA_EXIT_RETURN, ZELDA_MEETING, ROYAL_TOMB_OPENING, SUN_SONG_LESSON, ROOM_SCRIPTED, SARIA_GREETING,
    DODONGO_EXIT_STORY, DODONGO_EXIT_RETURN,
};

struct TeamCutscene {
    s32 entrance;
    s16 scene;
    u16 index;
    s16 eventFlags[2];
};

constexpr TeamCutscene teamCutscenes[] = {
    { ENTR_LOST_WOODS_SOUTH_EXIT, SCENE_LOST_WOODS, 0xFFF0, { EVENTCHKINF_SPOKE_TO_SARIA_ON_BRIDGE, -1 } },
    { ENTR_HYRULE_FIELD_PAST_BRIDGE_SPAWN,
      SCENE_HYRULE_FIELD,
      0xFFF1,
      { EVENTCHKINF_ZELDA_FLED_HYRULE_CASTLE, EVENTCHKINF_DRAWBRIDGE_OPENED_AFTER_ZELDA_FLED } },
    { ENTR_KOKIRI_FOREST_OUTSIDE_DEKU_TREE, SCENE_KOKIRI_FOREST, 0xFFFD, { EVENTCHKINF_MET_DEKU_TREE, -1 } },
    { ENTR_KOKIRI_FOREST_OUTSIDE_DEKU_TREE, SCENE_KOKIRI_FOREST, 0xFFFD, { EVENTCHKINF_MET_DEKU_TREE, -1 } },
    { ENTR_DEKU_TREE_ENTRANCE, SCENE_DEKU_TREE, 0xFFFD, { EVENTCHKINF_ENTERED_DEKU_TREE, -1 } },
    { ENTR_KOKIRI_FOREST_0, SCENE_KOKIRI_FOREST, 0xFFF1,
      { EVENTCHKINF_OBTAINED_KOKIRI_EMERALD_DEKU_TREE_DEAD, EVENTCHKINF_USED_DEKU_TREE_BLUE_WARP } },
    { ENTR_KOKIRI_FOREST_DEKU_TREE_BLUE_WARP, SCENE_KOKIRI_FOREST, 0, { -1, -1 } },
    { ENTR_CASTLE_COURTYARD_ZELDA_0, SCENE_CASTLE_COURTYARD_ZELDA, 0, { -1, -1 } },
    { ENTR_GRAVEYARD_ENTRANCE, SCENE_GRAVEYARD, 0xFFFD, { EVENTCHKINF_DESTROYED_ROYAL_FAMILY_TOMB, -1 } },
    { ENTR_ROYAL_FAMILYS_TOMB_0, SCENE_ROYAL_FAMILYS_TOMB, 0xFFFD, { -1, -1 } },
    { -1, SCENE_ID_MAX, 0, { -1, -1 } }, // ROOM_SCRIPTED uses the sender's scene and script.
    { ENTR_KOKIRI_FOREST_OUTSIDE_LINKS_HOUSE, SCENE_KOKIRI_FOREST, 0xFFFD, { -1, -1 } },
    { ENTR_DEATH_MOUNTAIN_TRAIL_BOTTOM_EXIT, SCENE_DEATH_MOUNTAIN_TRAIL, 0xFFF1,
      { EVENTCHKINF_USED_DODONGOS_CAVERN_BLUE_WARP, -1 } },
    { ENTR_DEATH_MOUNTAIN_TRAIL_DODONGO_BLUE_WARP, SCENE_DEATH_MOUNTAIN_TRAIL, 0, { -1, -1 } },
};

int GetBossWarpScene(int id) {
    switch (id) {
        case GOHMA_EXIT_STORY:
        case GOHMA_EXIT_RETURN:
            return SCENE_DEKU_TREE_BOSS;
        case DODONGO_EXIT_STORY:
        case DODONGO_EXIT_RETURN:
            return SCENE_DODONGOS_CAVERN_BOSS;
        default:
            return SCENE_ID_MAX;
    }
}

bool IsBossWarp(int id) {
    return GetBossWarpScene(id) != SCENE_ID_MAX;
}

bool IsBossWarpStory(int id) {
    return id == GOHMA_EXIT_STORY || id == DODONGO_EXIT_STORY;
}

bool IsTombCutscene(int id) {
    return id == ROYAL_TOMB_OPENING || id == SUN_SONG_LESSON;
}

bool IsRoomCutscene(int id) {
    return id == ZELDA_MEETING || id == SUN_SONG_LESSON || id == ROOM_SCRIPTED || id == SARIA_GREETING;
}

bool IsSariaGreetingLocation(const PlayState* play, int room) {
    return play->sceneNum == SCENE_LINKS_HOUSE ||
           (play->sceneNum == SCENE_KOKIRI_FOREST && play->roomCtx.curRoom.num == room);
}

bool IsDialogueAdvanceMode(int mode) {
    return mode == MSGMODE_TEXT_AWAIT_INPUT || mode == MSGMODE_TEXT_AWAIT_NEXT || mode == MSGMODE_TEXT_DONE;
}

bool IsSceneScript(std::string_view asset, int scene) {
    constexpr std::string_view prefix = "__OTR__scenes/";
    if (scene < 0 || scene >= SCENE_ID_MAX || asset.size() > 160 || !asset.starts_with(prefix)) {
        return false;
    }
    asset.remove_prefix(prefix.size());
    const auto layout = asset.substr(0, asset.find('/'));
    if (layout != "shared" && layout != "nonmq" && layout != "mq") {
        return false;
    }
    asset.remove_prefix(layout.size() + 1);
    const auto directory = asset.substr(0, asset.find('/'));
    if (directory != gSceneTable[scene].sceneFile.fileName || directory.size() == asset.size()) {
        return false;
    }
    asset.remove_prefix(directory.size() + 1);
    return !asset.empty() && asset.find_first_not_of(
                                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") ==
                                std::string_view::npos;
}

const char* GetTombCutsceneSegment(int id) {
    if (id == SUN_SONG_LESSON) {
        return gSunSongGraveSunSongTeachCs;
    }
    return LINK_IS_ADULT ? spot02_scene_Cs_003C80 : spot02_scene_Cs_005020;
}

bool IsAssetSegment(void* segment, const char* asset) {
    return segment != nullptr && ResourceMgr_OTRSigCheck(static_cast<char*>(segment)) &&
           std::strcmp(static_cast<const char*>(segment), asset) == 0;
}

bool IsCurrentCutscene(int id) {
    const auto& cutscene = teamCutscenes[id];
    if (gPlayState->sceneNum != cutscene.scene || gSaveContext.cutsceneIndex != cutscene.index) {
        return false;
    }
    const auto segment = gPlayState->csCtx.segment;
    switch (id) {
        case SARIA_GREETING:
            return IsAssetSegment(segment, gSpot04Cs_10E20);
        case DEKU_TREE_MEETING:
            return segment == D_808BCE20;
        case DEKU_TREE_QUESTION:
            return segment == D_808BD2A0;
        case DEKU_TREE_ENTRANCE:
            return IsAssetSegment(segment, gDekuTreeIntroCs);
        case ROYAL_TOMB_OPENING:
        case SUN_SONG_LESSON:
            return IsAssetSegment(segment, GetTombCutsceneSegment(id));
        default:
            return gSaveContext.entranceIndex == cutscene.entrance;
    }
}
} // namespace

bool Anchor::CanShareCutscenes() {
    return CanShareTombCutscenes() && LINK_IS_CHILD;
}

bool Anchor::CanShareTombCutscenes(bool requirePlayer) {
    return roomState.syncItemsAndFlags && CVarGetInteger(CVAR_REMOTE_ANCHOR("FollowTeamCutscenes"), 0) &&
           IsSaveLoaded(requirePlayer) && !IS_RANDO &&
           !CVarGetInteger(CVAR_ENHANCEMENT("TimeSavers.SkipCutscene.Story"), 0);
}

bool Anchor::CanUseTeamCutscene(int id) {
    if (IsBossWarp(id)) {
        return CanFollowBossWarp();
    }
    if (id == SARIA_GREETING && CVarGetInteger(CVAR_ENHANCEMENT("TimeSavers.SkipCutscene.Entrances"), 0)) {
        return false;
    }
    return (IsTombCutscene(id) || id == ROOM_SCRIPTED) ? CanShareTombCutscenes() : CanShareCutscenes();
}

bool Anchor::IsInTeamScriptRoom() const {
    return gPlayState->sceneNum == teamScriptScene && gPlayState->roomCtx.curRoom.num == teamCutsceneRoom &&
           gSaveContext.linkAge == teamScriptAge &&
           (ResourceMgr_IsSceneMasterQuest(teamScriptScene) != 0) == teamScriptMasterQuest;
}

bool Anchor::CanFollowBossWarp() {
    return isConnected && roomState.syncItemsAndFlags && IsSaveLoaded() && LINK_IS_CHILD && !IS_RANDO && !IS_BOSS_RUSH;
}

void Anchor::AnnounceTeamCutscene(int id) {
    teamCutsceneInputs.clear();
    teamCutsceneStep = 0;
    teamCutsceneLeader = ownClientId;
    teamCutsceneStartedAt = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
    teamCutsceneInputReady = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    nlohmann::json payload = { { "type", TEAM_CUTSCENE },
                               { "targetTeamId", CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default") },
                               { "cutscene", id }, { "startedAt", teamCutsceneStartedAt } };
    if (IsRoomCutscene(id)) {
        payload["room"] = teamCutsceneRoom;
    }
    if (id == ROOM_SCRIPTED) {
        payload["scene"] = teamScriptScene;
        payload["age"] = teamScriptAge;
        payload["masterQuest"] = teamScriptMasterQuest;
        payload["asset"] = teamScriptAsset;
    }
    SendJsonToRemote(payload);
}

void Anchor::StartZeldaMeeting(PlayState* play) {
    if (!isConnected || !CanShareCutscenes() || play != gPlayState || play->sceneNum != SCENE_CASTLE_COURTYARD_ZELDA ||
        activeTeamCutscene >= 0 || pendingTeamCutscene >= 0) {
        return;
    }
    teamCutsceneRoom = play->roomCtx.curRoom.num;
    activeTeamCutscene = ZELDA_MEETING;
    AnnounceTeamCutscene(ZELDA_MEETING);
}

bool Anchor::ForceSunSongPartTwo(PlayState* play) {
    if (!isConnected || play != gPlayState || activeTeamCutscene != SUN_SONG_LESSON || sunLessonPartTwo ||
        gSaveContext.entranceIndex != ENTR_ROYAL_FAMILYS_TOMB_1 || !CanShareTombCutscenes(false)) {
        return false;
    }
    sunLessonPartTwo = true;
    Flags_SetEventChkInf(EVENTCHKINF_LEARNED_SUNS_SONG);
    Cutscene_SetSegment(play, const_cast<char*>(gSunSongGraveSunSongTeachPart2Cs));
    gSaveContext.cutsceneTrigger = 2;
    gSaveContext.showTitleCard = false;
    return true;
}

bool Anchor::WillPlayRoyalTombOpening(PlayState* play) {
    return isConnected && play == gPlayState &&
           (pendingTeamCutscene == ROYAL_TOMB_OPENING || activeTeamCutscene == ROYAL_TOMB_OPENING);
}

extern "C" s32 Anchor_ForceSunSongPartTwo(PlayState* play) {
    return Anchor::Instance != nullptr && Anchor::Instance->ForceSunSongPartTwo(play);
}

extern "C" s32 Anchor_WillPlayRoyalTombOpening(PlayState* play) {
    return Anchor::Instance != nullptr && Anchor::Instance->WillPlayRoyalTombOpening(play);
}

void Anchor::FinishZeldaMeeting() {
    if (activeTeamCutscene == ZELDA_MEETING) {
        ResetTeamCutscene();
    }
}

extern "C" void Anchor_StartZeldaMeeting(PlayState* play) {
    if (Anchor::Instance != nullptr) {
        Anchor::Instance->StartZeldaMeeting(play);
    }
}

extern "C" void Anchor_FinishZeldaMeeting(void) {
    if (Anchor::Instance != nullptr) {
        Anchor::Instance->FinishZeldaMeeting();
    }
}

bool Anchor::UseSharedBossPortal() {
    return CanFollowBossWarp() && (gPlayState->sceneNum == SCENE_DEKU_TREE_BOSS ||
                                   gPlayState->sceneNum == SCENE_DODONGOS_CAVERN_BOSS);
}

int Anchor::StartBossWarp(bool story) {
    if (!UseSharedBossPortal() || activeTeamCutscene >= 0) {
        return -1;
    }
    if (pendingTeamCutscene >= 0) {
        return GetBossWarpScene(pendingTeamCutscene) == gPlayState->sceneNum ? IsBossWarpStory(pendingTeamCutscene) : -1;
    }
    if (gPlayState->sceneNum == SCENE_DODONGOS_CAVERN_BOSS) {
        pendingTeamCutscene = story ? DODONGO_EXIT_STORY : DODONGO_EXIT_RETURN;
    } else {
        pendingTeamCutscene = story ? GOHMA_EXIT_STORY : GOHMA_EXIT_RETURN;
    }
    teamCutsceneDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    AnnounceTeamCutscene(pendingTeamCutscene);
    return story;
}

int Anchor::CommitBossWarp() {
    if (!UseSharedBossPortal() || GetBossWarpScene(pendingTeamCutscene) != gPlayState->sceneNum) {
        return -1;
    }
    if (teamCutsceneLeader == ownClientId && !bossWarpCommitted) {
        bossWarpCommitted = true;
        nlohmann::json payload = { { "type", TEAM_CUTSCENE },
                                   { "targetTeamId", CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default") },
                                   { "cutscene", pendingTeamCutscene }, { "startedAt", teamCutsceneStartedAt },
                                   { "committed", true } };
        SendJsonToRemote(payload);
    }
    return IsBossWarpStory(pendingTeamCutscene);
}

int Anchor::GetBossWarpRequest() {
    if (!UseSharedBossPortal() || GetBossWarpScene(pendingTeamCutscene) != gPlayState->sceneNum ||
        teamCutsceneLeader == ownClientId ||
        !IsSharedTeamMember(teamCutsceneLeader) || gSaveContext.health <= 0 || Play_IsLivePause(gPlayState) ||
        Play_InCsMode(gPlayState) || gPlayState->transitionTrigger != TRANS_TRIGGER_OFF ||
        gPlayState->transitionMode != TRANS_MODE_OFF || gSaveContext.nextCutsceneIndex != 0xFFEF ||
        !GameInteractor::IsPlayerInControl()) {
        return -1;
    }
    return IsBossWarpStory(pendingTeamCutscene);
}

extern "C" s32 Anchor_UseSharedBossPortal(void) {
    return Anchor::Instance != nullptr && Anchor::Instance->UseSharedBossPortal();
}

extern "C" s32 Anchor_StartBossWarp(s32 story) {
    return Anchor::Instance == nullptr ? -1 : Anchor::Instance->StartBossWarp(story != 0);
}

extern "C" s32 Anchor_GetBossWarpRequest(void) {
    return Anchor::Instance == nullptr ? -1 : Anchor::Instance->GetBossWarpRequest();
}

extern "C" s32 Anchor_CommitBossWarp(void) {
    return Anchor::Instance == nullptr ? -1 : Anchor::Instance->CommitBossWarp();
}

void Anchor::ResetTeamCutscene() {
    pendingTeamCutscene = -1;
    activeTeamCutscene = -1;
    teamCutsceneRoom = -1;
    teamScriptScene = -1;
    teamScriptAge = -1;
    teamScriptAsset.clear();
    sunLessonPartTwo = false;
    teamCutsceneLeader = 0;
    teamCutsceneStartedAt = 0;
    teamCutsceneInputReady = {};
    teamCutsceneInputs.clear();
    teamCutsceneStep = 0;
    lastCutsceneInput = {};
    bossWarpCommitted = false;
}

void Anchor::OnScriptedCutsceneStart() {
    if (activeTeamCutscene == ZELDA_MEETING || activeTeamCutscene == SUN_SONG_LESSON) {
        return;
    }
    const bool bossWarp = IsBossWarp(pendingTeamCutscene) && CanFollowBossWarp();
    if (!bossWarp && !CanShareTombCutscenes()) {
        return;
    }

    for (int id = 0; id < ARRAY_COUNT(teamCutscenes); ++id) {
        if (id == ZELDA_MEETING || id == ROOM_SCRIPTED || !CanUseTeamCutscene(id) ||
            (bossWarp ? (id != pendingTeamCutscene || !IsBossWarpStory(id)) : IsBossWarp(id)) ||
            !IsCurrentCutscene(id)) {
            continue;
        }

        const bool following = pendingTeamCutscene == id &&
                               (teamCutsceneLeader == ownClientId || IsSharedTeamMember(teamCutsceneLeader));
        playedTeamCutscenes |= 1u << id;
        activeTeamCutscene = id;
        pendingTeamCutscene = -1;
        if (id == SUN_SONG_LESSON || id == SARIA_GREETING) {
            teamCutsceneRoom = gPlayState->roomCtx.curRoom.num;
        }
        if (id == SUN_SONG_LESSON) {
            teamCutsceneDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
        }
        lastCutsceneInput = {};
        if (following) {
            for (s16 flag : teamCutscenes[id].eventFlags) {
                if (flag >= 0) {
                    GameInteractor::RawAction::SetFlag(FLAG_EVENT_CHECK_INF, flag);
                }
            }
            return;
        }

        AnnounceTeamCutscene(id);
        return;
    }

    if (activeTeamCutscene >= 0 || gSaveContext.cutsceneIndex != 0xFFFD) {
        return;
    }
    auto* segment = static_cast<char*>(gPlayState->csCtx.segment);
    const bool following = pendingTeamCutscene == ROOM_SCRIPTED && IsInTeamScriptRoom();
    if (following && teamScriptResource != nullptr && segment == teamScriptResource->GetRawPointer()) {
        activeTeamCutscene = ROOM_SCRIPTED;
        pendingTeamCutscene = -1;
        return;
    }
    if (segment == nullptr || !ResourceMgr_OTRSigCheck(segment) || !IsSceneScript(segment, gPlayState->sceneNum)) {
        return;
    }
    const bool alreadyAnnounced = following && teamScriptAsset == segment;
    if (pendingTeamCutscene >= 0 && !alreadyAnnounced) {
        return;
    }
    activeTeamCutscene = ROOM_SCRIPTED;
    pendingTeamCutscene = -1;
    if (!alreadyAnnounced) {
        teamCutsceneRoom = gPlayState->roomCtx.curRoom.num;
        teamScriptScene = gPlayState->sceneNum;
        teamScriptAge = gSaveContext.linkAge;
        teamScriptMasterQuest = ResourceMgr_IsSceneMasterQuest(teamScriptScene) != 0;
        teamScriptAsset = segment;
        AnnounceTeamCutscene(ROOM_SCRIPTED);
    }
}

void Anchor::HandlePacket_TeamCutscene(nlohmann::json payload) {
    const auto id = payload.at("cutscene").get<int>();
    const auto leader = payload.at("clientId").get<uint32_t>();
    const auto startedAt = payload.value("startedAt", int64_t{ 0 });
    if (id < 0 || id >= ARRAY_COUNT(teamCutscenes) || !CanUseTeamCutscene(id) || !IsSharedTeamMember(leader) ||
        startedAt <= 0) {
        return;
    }
    std::string asset;
    const int room = IsRoomCutscene(id) ? payload.value("room", -1) : -1;
    if (IsRoomCutscene(id)) {
        const int scene = id == ROOM_SCRIPTED ? payload.value("scene", -1) : teamCutscenes[id].scene;
        if (id == SARIA_GREETING ? (room < 0 || !IsSariaGreetingLocation(gPlayState, room))
                                 : (gPlayState->sceneNum != scene || room != gPlayState->roomCtx.curRoom.num)) {
            return;
        }
        if (id == ROOM_SCRIPTED) {
            asset = payload.value("asset", std::string{});
            if (payload.value("age", -1) != gSaveContext.linkAge ||
                payload.value("masterQuest", false) != (ResourceMgr_IsSceneMasterQuest(scene) != 0) ||
                !IsSceneScript(asset, scene)) {
                return;
            }
        }
    }
    const int current = activeTeamCutscene >= 0 ? activeTeamCutscene : pendingTeamCutscene;
    const bool committed = IsBossWarp(id) && payload.value("committed", false);
    if (committed && current == id && leader == teamCutsceneLeader && startedAt == teamCutsceneStartedAt) {
        bossWarpCommitted = true;
        return;
    }
    if (current >= 0) {
        const bool samePendingWarp = activeTeamCutscene < 0 && IsBossWarp(id) &&
                                     GetBossWarpScene(current) == GetBossWarpScene(id);
        if ((current != id && !samePendingWarp) ||
            (id == ROOM_SCRIPTED && (teamScriptAsset != asset || !IsInTeamScriptRoom())) ||
            std::make_pair(startedAt, leader) >= std::make_pair(teamCutsceneStartedAt, teamCutsceneLeader)) {
            return;
        }
        teamCutsceneLeader = leader;
        teamCutsceneStartedAt = startedAt;
        if (samePendingWarp) {
            pendingTeamCutscene = id;
            bossWarpCommitted = committed;
        }
        teamCutsceneInputs.clear();
        lastCutsceneInput = {};
        return;
    }
    if (id != DEKU_TREE_QUESTION && !IsBossWarp(id) && (playedTeamCutscenes & (1u << id))) {
        return;
    }

    pendingTeamCutscene = id;
    if (IsRoomCutscene(id)) {
        teamCutsceneRoom = room;
    }
    if (id == ROOM_SCRIPTED) {
        teamScriptScene = gPlayState->sceneNum;
        teamScriptAge = gSaveContext.linkAge;
        teamScriptMasterQuest = ResourceMgr_IsSceneMasterQuest(teamScriptScene) != 0;
        teamScriptAsset = std::move(asset);
    }
    bossWarpCommitted = committed;
    teamCutsceneLeader = leader;
    teamCutsceneStartedAt = startedAt;
    teamCutsceneDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
}

void Anchor::HandlePacket_TeamCutsceneInput(const nlohmann::json& payload) {
    const auto leader = payload.at("clientId").get<uint32_t>();
    const int id = payload.at("cutscene").get<int>();
    if (!IsSharedTeamMember(leader) || leader != teamCutsceneLeader || id < 0 ||
        (id != pendingTeamCutscene && id != activeTeamCutscene) || teamCutsceneInputs.size() >= 256 ||
        payload.value("startedAt", int64_t{ 0 }) != teamCutsceneStartedAt) {
        return;
    }
    const int choice = payload.at("choice").get<int>();
    const int step = payload.value("step", -1);
    if (choice >= 0 && choice < 3 && step >= -1 && step <= 4096) {
        teamCutsceneInputs.push_back({ payload.at("text").get<int>(), payload.at("mode").get<int>(), choice,
                                       payload.at("skip").get<bool>(), step });
    }
}

void Anchor::HandleTeamCutsceneInput(PlayState* play, bool* should, bool* skipTextbox) {
    if (activeTeamCutscene < 0 || play != gPlayState ||
        (play->csCtx.state == CS_STATE_IDLE && activeTeamCutscene != ZELDA_MEETING)) {
        return;
    }

    const bool skip = skipTextbox != nullptr;
    auto& message = play->msgCtx;
    const auto stamp = std::make_tuple(play->gameplayFrames, static_cast<int>(message.textId),
                                       static_cast<int>(message.msgMode), skip);
    if (teamCutsceneLeader == ownClientId) {
        if (std::chrono::steady_clock::now() < teamCutsceneInputReady) {
            *should = false;
            return;
        }
        if (*should && lastCutsceneInput != stamp) {
            lastCutsceneInput = stamp;
            nlohmann::json payload;
            payload["type"] = TEAM_CUTSCENE_INPUT;
            payload["targetTeamId"] = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
            payload["cutscene"] = activeTeamCutscene;
            payload["startedAt"] = teamCutsceneStartedAt;
            payload["text"] = message.textId;
            payload["mode"] = message.msgMode;
            payload["choice"] = std::clamp<int>(message.choiceIndex, 0, 2);
            payload["skip"] = skip;
            payload["step"] = teamCutsceneStep;
            SendJsonToRemote(payload);
            if (!skip) {
                ++teamCutsceneStep;
            }
        }
        return;
    }

    *should = false;
    if (lastCutsceneInput == stamp) {
        return;
    }
    const auto matchesPage = [&](const TeamCutsceneInput& event) {
        return event.text == message.textId && (event.step < 0 || event.step == teamCutsceneStep);
    };
    std::erase_if(teamCutsceneInputs, [&](const TeamCutsceneInput& event) {
        return (event.step >= 0 && event.step < teamCutsceneStep) || (!skip && event.skip && matchesPage(event));
    });
    const auto input = std::find_if(teamCutsceneInputs.begin(), teamCutsceneInputs.end(), matchesPage);
    if (input == teamCutsceneInputs.end()) {
        return;
    }
    if (skip) {
        // A real B skip affects every page in this textbox, even if the viewer is already waiting at a break.
        if (input->skip) {
            *skipTextbox = true;
            teamCutsceneInputs.erase(input);
            *should = true;
        } else if (message.msgMode == MSGMODE_TEXT_DISPLAYING && input->mode != MSGMODE_TEXT_AWAIT_INPUT) {
            // Reveal the current page without bypassing the wait that will consume the queued advance.
            *skipTextbox = false;
            *should = true;
        }
    } else if (input->mode == message.msgMode ||
               (IsDialogueAdvanceMode(input->mode) && IsDialogueAdvanceMode(message.msgMode))) {
        message.choiceIndex = input->choice;
        lastCutsceneInput = stamp;
        teamCutsceneInputs.erase(input);
        ++teamCutsceneStep;
        *should = true;
    }
}

void Anchor::FollowPendingTeamCutscene() {
    if (activeTeamCutscene >= 0) {
        const bool zelda = activeTeamCutscene == ZELDA_MEETING;
        const bool zeldaScene = gPlayState != nullptr && (gPlayState->sceneNum == SCENE_CASTLE_COURTYARD_ZELDA ||
            (gSaveContext.entranceIndex == ENTR_CUTSCENE_MAP_0 && gSaveContext.cutsceneIndex == 0xFFF7));
        const bool betweenSunLessons = activeTeamCutscene == SUN_SONG_LESSON && !sunLessonPartTwo &&
            gPlayState != nullptr && gPlayState->sceneNum == SCENE_ROYAL_FAMILYS_TOMB &&
            std::chrono::steady_clock::now() < teamCutsceneDeadline;
        if (!CanUseTeamCutscene(activeTeamCutscene) ||
            (teamCutsceneLeader != ownClientId && !IsSharedTeamMember(teamCutsceneLeader)) ||
            (zelda ? !zeldaScene : (!betweenSunLessons && gPlayState->csCtx.state == CS_STATE_IDLE &&
                                    gSaveContext.cutsceneTrigger == 0))) {
            ResetTeamCutscene();
        }
        return;
    }
    if (pendingTeamCutscene < 0) {
        return;
    }
    const bool bossWarp = IsBossWarp(pendingTeamCutscene);
    if (!CanUseTeamCutscene(pendingTeamCutscene) ||
        (teamCutsceneLeader != ownClientId && !IsSharedTeamMember(teamCutsceneLeader)) ||
        (pendingTeamCutscene == ROOM_SCRIPTED && !IsInTeamScriptRoom()) ||
        (pendingTeamCutscene == SARIA_GREETING && !IsSariaGreetingLocation(gPlayState, teamCutsceneRoom)) ||
        std::chrono::steady_clock::now() >= teamCutsceneDeadline) {
        ResetTeamCutscene();
        return;
    }
    if (gSaveContext.health <= 0 || gPlayState->transitionMode != TRANS_MODE_OFF ||
        gPlayState->transitionTrigger != TRANS_TRIGGER_OFF || gSaveContext.cutsceneTrigger != 0 ||
        gSaveContext.cutsceneIndex >= 0xFFF0 || gSaveContext.nextCutsceneIndex != 0xFFEF) {
        return;
    }
    if (Play_IsLivePause(gPlayState)) {
        auto& pause = gPlayState->pauseCtx;
        if (pause.state < 4) {
            pause.state = 0;
            R_PAUSE_MENU_MODE = 0;
            R_UPDATE_RATE = 3;
            func_800F64E0(0);
        } else if (pause.state < 0x12) {
            pause.state = 0x13;
        }
        return;
    }
    if (!GameInteractor::IsPlayerInControl() || Play_InCsMode(gPlayState)) {
        return;
    }

    const int id = pendingTeamCutscene;
    if (id == ROOM_SCRIPTED) {
        if (gPlayState->csCtx.state != CS_STATE_IDLE) {
            return;
        }
        auto script = std::dynamic_pointer_cast<SOH::Cutscene>(ResourceMgr_GetResourceByNameHandlingMQ(teamScriptAsset.c_str()));
        if (script == nullptr || script->GetRawPointer() == nullptr || script->GetPointerSize() < 2 * sizeof(CutsceneData)) {
            ResetTeamCutscene();
            return;
        }
        teamScriptResource = std::move(script);
        Cutscene_SetSegment(gPlayState, teamScriptResource->GetRawPointer());
        gSaveContext.cutsceneTrigger = 1;
        return;
    }
    const auto& cutscene = teamCutscenes[id];
    if (id == SARIA_GREETING && gPlayState->sceneNum == SCENE_KOKIRI_FOREST) {
        if (Actor* saria = Actor_Find(&gPlayState->actorCtx, ACTOR_EN_SA, ACTORCAT_NPC);
            saria != nullptr && saria->init == nullptr && saria->update != nullptr) {
            EnSa_StartTeamGreeting(saria, gPlayState);
        }
        return;
    }
    if (id == ZELDA_MEETING) {
        if (gPlayState->sceneNum != SCENE_CASTLE_COURTYARD_ZELDA || gPlayState->roomCtx.curRoom.num != teamCutsceneRoom) {
            ResetTeamCutscene();
        } else if (Actor* zelda = Actor_Find(&gPlayState->actorCtx, ACTOR_EN_ZL4, ACTORCAT_NPC);
                   zelda != nullptr && zelda->init == nullptr && zelda->update != nullptr &&
                   EnZl4_StartTeamMeeting(zelda, gPlayState)) {
            activeTeamCutscene = id;
            pendingTeamCutscene = -1;
        }
        return;
    }
    if (id == SUN_SONG_LESSON && (gPlayState->sceneNum != cutscene.scene ||
        gPlayState->roomCtx.curRoom.num != teamCutsceneRoom)) {
        ResetTeamCutscene();
        return;
    }
    if (bossWarp) {
        const bool story = IsBossWarpStory(id);
        const int returnId = id == GOHMA_EXIT_STORY ? GOHMA_EXIT_RETURN : DODONGO_EXIT_RETURN;
        const bool skippedStory = story && CVarGetInteger(CVAR_ENHANCEMENT("TimeSavers.SkipCutscene.Story"), 0) &&
                                  gSaveContext.entranceIndex == teamCutscenes[returnId].entrance;
        if (gPlayState->sceneNum == cutscene.scene && ((!story && gSaveContext.entranceIndex == cutscene.entrance) ||
                                                       skippedStory)) {
            ResetTeamCutscene();
        } else if (teamCutsceneLeader != ownClientId && bossWarpCommitted &&
                   gPlayState->sceneNum != GetBossWarpScene(id)) {
            if (story) {
                const int reward = id == DODONGO_EXIT_STORY ? ITEM_GORON_RUBY : ITEM_KOKIRI_EMERALD;
                GameInteractor_Should(VB_PLAY_BLUE_WARP_CS, true, cutscene.eventFlags[0]);
                if ((id != DODONGO_EXIT_STORY || !CHECK_QUEST_ITEM(QUEST_GORON_RUBY)) &&
                    GameInteractor_Should(VB_GIVE_ITEM_FROM_BLUE_WARP, true, reward)) {
                    Item_Give(gPlayState, reward);
                }
                for (s16 flag : cutscene.eventFlags) {
                    if (flag >= 0) {
                        Flags_SetEventChkInf(flag);
                    }
                }
                gSaveContext.nextCutsceneIndex = cutscene.index;
            }
            gSaveContext.respawnFlag = 0;
            gPlayState->nextEntranceIndex = cutscene.entrance;
            gPlayState->transitionType = TRANS_TYPE_FADE_WHITE_SLOW;
            gSaveContext.nextTransitionType = TRANS_TYPE_FADE_WHITE;
            gPlayState->transitionTrigger = TRANS_TRIGGER_START;
        }
        return;
    }
    if (gPlayState->sceneNum == cutscene.scene) {
        if (id == DEKU_TREE_MEETING || id == DEKU_TREE_QUESTION) {
            Actor* mouth = Actor_Find(&gPlayState->actorCtx, ACTOR_BG_TREEMOUTH, ACTORCAT_BG);
            if (mouth != nullptr && mouth->update != nullptr) {
                BgTreemouth_StartConversation(reinterpret_cast<BgTreemouth*>(mouth), gPlayState, id == DEKU_TREE_MEETING);
                return;
            }
        } else if (IsTombCutscene(id)) {
            Cutscene_SetSegment(gPlayState, const_cast<char*>(GetTombCutsceneSegment(id)));
            gSaveContext.cutsceneTrigger = 1;
            return;
        } else if (id == DEKU_TREE_ENTRANCE) {
            Cutscene_SetSegment(gPlayState, const_cast<char*>(gDekuTreeIntroCs));
            gSaveContext.cutsceneTrigger = 2;
            return;
        }
    }

    gSaveContext.respawnFlag = 0;
    if (id == SARIA_BRIDGE || id == ZELDA_ESCAPE) {
        gSaveContext.nextCutsceneIndex = cutscene.index;
    }
    gPlayState->nextEntranceIndex = cutscene.entrance;
    gPlayState->transitionType = TRANS_TYPE_FADE_BLACK_FAST;
    gPlayState->transitionTrigger = TRANS_TRIGGER_START;
}
