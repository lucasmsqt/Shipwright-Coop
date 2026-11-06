#pragma once

#ifdef __cplusplus

#include "soh/Network/Network.h"
#include <libultraship/bridge/consolevariablebridge.h>
#include <ship/window/gui/GuiWindow.h>
#include <spdlog/spdlog.h>
#include <array>
#include <chrono>
#include <deque>
#include <tuple>
#include <queue>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

extern "C" {
#include "variables.h"
#include "z64.h"
}
#include "soh/Network/Anchor/GohmaSync.h"
#include "soh/Network/Anchor/RedeadSync.h"
#include "soh/Network/Anchor/ScrubShot.h"

namespace Ship {
class IResource;
}

void DummyPlayer_Init(Actor* actor, PlayState* play);
void DummyPlayer_Update(Actor* actor, PlayState* play);
void DummyPlayer_Draw(Actor* actor, PlayState* play);
void DummyPlayer_Destroy(Actor* actor, PlayState* play);

typedef struct {
    uint32_t clientId;
    std::string name;
    Color_RGB8 color;
    std::string clientVersion;
    std::string teamId;
    bool online;
    bool self;
    uint32_t seed;
    bool isSaveLoaded;
    bool isGameComplete;
    s16 sceneNum;
    s8 curRoomNum;
    s32 entranceIndex;

    // Only available in PLAYER_UPDATE packets
    s32 linkAge;
    PosRot posRot;
    std::chrono::steady_clock::time_point lastPlayerUpdate;
    Vec3s jointTable[24];
    u8 movementFlags;
    Vec3s prevTransl;
    Vec3s upperLimbRot;
    s8 currentBoots;
    s8 currentShield;
    s8 currentTunic;
    u32 stateFlags1;
    u32 stateFlags2;
    u8 buttonItem0;
    s8 itemAction;
    s8 heldItemAction;
    u8 modelGroup;
    s8 invincibilityTimer;
    f32 unk_85C;
    s16 stickBurnTimer;
    s16 unk_862;
    s8 actionVar1;
    u8 ocarinaNote;
    f32 ocarinaModulator;
    s8 ocarinaBend;

    // Ptr to the dummy player
    Player* player;
} AnchorClient;

typedef struct {
    uint32_t ownerClientId;
    u8 pvpMode;           // 0 = off, 1 = on, 2 = on with friendly fire
    u8 showLocationsMode; // 0 = none, 1 = team, 2 = all
    u8 teleportMode;      // 0 = off, 1 = team, 2 = all
    u8 syncItemsAndFlags; // 0 = off, 1 = on
} RoomState;

class Anchor : public Network {
  private:
    uint32_t spawningDummyPlayerForClientId = 0;
    bool shouldRefreshActors = false;
    bool justLoadedSave = false;
    bool isHandlingUpdateTeamState = false;
    bool isProcessingIncomingPacket = false;
    int pendingTeamCutscene = -1;
    int activeTeamCutscene = -1;
    int teamCutsceneRoom = -1;
    int teamScriptScene = -1;
    int teamScriptAge = -1;
    bool teamScriptMasterQuest = false;
    std::string teamScriptAsset;
    std::shared_ptr<Ship::IResource> teamScriptResource;
    bool sunLessonPartTwo = false;
    uint32_t teamCutsceneLeader = 0;
    int64_t teamCutsceneStartedAt = 0;
    std::chrono::steady_clock::time_point teamCutsceneInputReady;
    struct TeamCutsceneInput {
        int text;
        int mode;
        int choice;
        bool skip;
    };
    std::deque<TeamCutsceneInput> teamCutsceneInputs;
    std::tuple<uint32_t, int, int, bool> lastCutsceneInput{};
    unsigned int playedTeamCutscenes = 0;
    bool gohmaWarpCommitted = false;
    std::chrono::steady_clock::time_point teamCutsceneDeadline;
    using NpcArea = std::tuple<int, int, int, bool>;
    using EnemyKey = std::array<int, 4>;
    using RoomObjectKey = std::array<int, 3>;
    struct RoomObjectState {
        uint64_t version;
        Vec3f position;
        int phase;
        int detail;
        std::chrono::steady_clock::time_point received{};
    };
    using RoomObjects = std::map<RoomObjectKey, RoomObjectState>;
    struct StalchildSample {
        Vec3f position;
        Vec3s rotation;
        s16 params;
        s16 phase;
        s16 health;
        float offset;
        bool attacking;
    };
    struct GalleryTarget {
        Vec3f position;
        s16 color;
        s16 yaw;
    };
    struct GallerySample {
        std::array<std::optional<GalleryTarget>, 2> targets;
        uint16_t markers = 0;
    };
    struct EnemyPose {
        Vec3f position;
        Vec3s worldRot;
        Vec3s shapeRot;
        float scale;
        int phase = -1;
        std::optional<RedeadMotion> redead;
        std::optional<ScrubMotion> scrub;
    };
    struct NpcPosition {
        s16 actorId;
        s16 params;
        Vec3f position;
        s16 yaw;
        s16 headAngle = 0;
        s16 patrolPhase = -1;
    };
    struct NpcGaze {
        s16 actorId;
        s16 params;
        s16 distanceBand;
        bool talking;
        Vec3s head;
        Vec3s torso;
    };
    struct WorldPeer {
        int scene = SCENE_ID_MAX;
        int room = -1;
        int age = -1;
        bool masterQuest = false;
        bool syncNpcs = false;
        bool hasClock = false;
        u16 dayTime = 0;
        u16 skyboxTime = 0;
        uint32_t visit = 0;
        std::chrono::steady_clock::time_point entered;
        std::chrono::steady_clock::time_point updated;
        std::map<int, NpcPosition> npcs;
        std::map<int, NpcGaze> gazes;
        std::map<NpcArea, uint32_t> npcAuthorities;
        std::vector<EnemyKey> emergedBabas;
        bool syncCombat = false;
        std::map<EnemyKey, EnemyPose> enemyPoses;
        RoomObjects objects;
        std::optional<GohmaTeamState> gohma;
        std::optional<std::map<int, StalchildSample>> stalchildren;
        std::optional<GallerySample> gallery;

        NpcArea GetArea() const { return { scene, room, age, masterQuest }; }
    };
    WorldPeer localWorldPeer;
    uint32_t worldVisit = 0;
    std::map<uint32_t, WorldPeer> worldPeers;
    std::chrono::steady_clock::time_point nextWorldUpdate;
    uint32_t npcAuthority = 0;
    uint32_t enemyPoseAuthority = 0;
    NpcArea roomObjectArea{ SCENE_ID_MAX, -1, -1, false };
    RoomObjects roomObjects;
    uint32_t transientRoomSequence = 0;
    std::set<RoomObjectKey> spawnedTreeSkulltulas;
    bool treeSkulltulasDirty = false;
    uint32_t stalchildSerial = 0;
    bool dynamicReplicasActive = false;
    bool gallerySpectating = false;
    using EnemyDamageOrigin = std::pair<uint32_t, uint64_t>;
    enum class BabaEmergence { Dormant, Animate, Emerged };
    struct SharedEnemyState {
        std::map<EnemyDamageOrigin, int32_t> totals;
        int32_t pendingDamage = 0;
        int32_t pendingReaction = 0;
        PosRot pendingReactionPose{};
        BabaEmergence emergence = BabaEmergence::Dormant;
        bool defeated = false;
    };
    std::map<EnemyKey, SharedEnemyState> sharedEnemies;
    s16 combatScene = SCENE_ID_MAX;
    s8 combatRoom = -1;
    uint64_t combatVisit = 0;
    uint32_t scrubShotSerial = 0;
    uint32_t playerArrowSerial = 0;
    struct EnemyHit {
        uint32_t sender;
        NpcArea area;
        int actorId;
        int actorIndex;
        int shape;
        int colliderSlot;
        int elementSlot;
        ColliderTouch touch;
        Vec3s position;
        int arrowType;
        PosRot arrowPose;
        std::chrono::steady_clock::time_point expires;
    };
    std::vector<EnemyHit> enemyHits;
    std::queue<nlohmann::json> incomingPacketQueue;
    std::mutex incomingPacketQueueMutex;
    std::queue<nlohmann::json> outgoingPacketQueue;
    std::mutex outgoingPacketQueueMutex;

    nlohmann::json PrepClientState();
    nlohmann::json PrepRoomState();
    void RegisterHooks();
    void RefreshClientActors();
    void SetDummyPlayerClientId(const Actor* actor, uint32_t clientId);
    bool IsSharedTeamMember(uint32_t clientId) const;
    bool CanShareCutscenes();
    bool CanShareTombCutscenes(bool requirePlayer = true);
    bool CanUseTeamCutscene(int id);
    bool CanFollowGohmaWarp();
    void AnnounceTeamCutscene(int id);
    void OnScriptedCutsceneStart();
    bool IsInTeamScriptRoom() const;
    void FollowPendingTeamCutscene();
    void ResetTeamCutscene();
    void HandleTeamCutsceneInput(PlayState* play, bool skip, bool* should);
    bool CanShareCombat();
    static void MergeInventoryUpgrades(Inventory& inventory, uint32_t local, uint32_t remote);
    void ResetCombatRoom();
    void UpdateCombatRoom();
    void UpdateBabaEmergence(Actor* actor, bool* should, bool* alreadyEmerged);
    bool ShouldKeepBabaEmerged(Actor* actor);
    void SetBabaEmerged(const EnemyKey& key, BabaEmergence emergence);
    void ReadBabaEmergenceSnapshot(const nlohmann::json& payload, WorldPeer& peer);
    void AddEnemyPoseSnapshot(nlohmann::json& payload);
    void ReadEnemyPoseSnapshot(const nlohmann::json& payload, WorldPeer& peer);
    bool IsScrubShotPeer(uint32_t id, const WorldPeer& peer);
    uint32_t FindScrubController(Actor* scrub);
    void SendPacket_EnemyDamage(Actor* actor, int32_t damage, int32_t reaction);
    void ConsumeSharedEnemyDamage(Actor* actor, int32_t* damage, int32_t* reaction, PosRot* pose);
    void QueueEnemyDamage(const EnemyKey& key, int32_t damage, int32_t reaction, EnemyDamageOrigin origin,
                          int32_t total, const PosRot* pose = nullptr);
    void AddEnemyDamageSnapshot(nlohmann::json& payload);
    void ReadEnemyDamageSnapshot(const nlohmann::json& payload);
    uint32_t GetNpcCoordinator() const;
    uint32_t ChooseNpcAuthority(const NpcArea& area) const;
    void ApplyTeamNpcPositions();
    void ApplyTeamNpcGaze();
    void AddNpcGazeSnapshot(nlohmann::json& payload);
    void ReadNpcGazeSnapshot(const nlohmann::json& payload, WorldPeer& peer);
    void UpdateWorldState();
    void ApplyTeamFieldTime();
    const WorldPeer* FindFieldClockPeer(bool entering) const;
    void ResetWorldState();
    bool CanShareRoomObjects();
    NpcArea GetRoomObjectArea();
    void UpdateRoomObjects();
    void AddRoomObjectSnapshot(nlohmann::json& payload);
    void ReadRoomObjectSnapshot(const nlohmann::json& payload, WorldPeer& peer);
    void MergeRoomObject(const RoomObjectKey& key, const RoomObjectState& state);
    void PublishRoomObject(const RoomObjectKey& key, const Vec3f& position, int phase, int detail);
    void ApplyTreeSkulltulas();
    static RoomObjectState DecodeRoomObject(const nlohmann::json& entry);
    static bool IsValidRoomObject(const RoomObjectKey& key, const RoomObjectState& state);
    static RoomObjectKey GetRoomObjectKey(const Actor* actor);
    void AddGohmaSnapshot(nlohmann::json& payload);
    void ReadGohmaSnapshot(const nlohmann::json& payload, WorldPeer& peer);
    uint32_t GetStalchildAuthority() const;
    void AddDynamicActorSnapshot(nlohmann::json& payload);
    void ReadDynamicActorSnapshot(const nlohmann::json& payload, WorldPeer& peer);
    void UpdateDynamicActors();
    void ClearDynamicActors();

    void HandlePacket_AllClientState(nlohmann::json payload);
    void HandlePacket_ConsumeAdultTradeItem(nlohmann::json payload);
    void HandlePacket_DamagePlayer(nlohmann::json payload);
    void HandlePacket_DisableAnchor(nlohmann::json payload);
    void HandlePacket_EnemyDamage(nlohmann::json payload);
    void HandlePacket_EnemyEmerged(const nlohmann::json& payload);
    void HandlePacket_EnemyHit(const nlohmann::json& payload, bool arrow);
    void HandlePacket_PlayerArrow(const nlohmann::json& payload);
    void HandlePacket_ScrubShot(const nlohmann::json& payload);
    void HandlePacket_EntranceDiscovered(nlohmann::json payload);
    void HandlePacket_GameComplete(nlohmann::json payload);
    void HandlePacket_GiveItem(nlohmann::json payload);
    void HandlePacket_OcarinaSfx(nlohmann::json payload);
    void HandlePacket_PlayerSfx(nlohmann::json payload);
    void HandlePacket_PlayerUpdate(const nlohmann::json& payload);
    void HandlePacket_RequestTeamState(nlohmann::json payload);
    void HandlePacket_RequestTeleport(nlohmann::json payload);
    void HandlePacket_ServerMessage(nlohmann::json payload);
    void HandlePacket_SetCheckStatus(nlohmann::json payload);
    void HandlePacket_SetFlag(nlohmann::json payload);
    void HandlePacket_TeamCutscene(nlohmann::json payload);
    void HandlePacket_TeamCutsceneInput(const nlohmann::json& payload);
    void HandlePacket_TeleportTo(nlohmann::json payload);
    void HandlePacket_UnsetFlag(nlohmann::json payload);
    void HandlePacket_UpdateBeansCount(nlohmann::json payload);
    void HandlePacket_UpdateClientState(nlohmann::json payload);
    void HandlePacket_UpdateDungeonItems(nlohmann::json payload);
    void HandlePacket_UpdateRoomState(nlohmann::json payload);
    void HandlePacket_UpdateTeamState(nlohmann::json payload);
    void HandlePacket_WorldState(const nlohmann::json& payload);

  public:
    uint32_t ownClientId;
    inline static const std::string clientVersion = (char*)gGitCommitHash;

    // Packet types //
    inline static const std::string ALL_CLIENT_STATE = "ALL_CLIENT_STATE";
    inline static const std::string DAMAGE_PLAYER = "DAMAGE_PLAYER";
    inline static const std::string DISABLE_ANCHOR = "DISABLE_ANCHOR";
    inline static const std::string ENEMY_DAMAGE = "ENEMY_DAMAGE";
    inline static const std::string ENEMY_EMERGED = "ENEMY_EMERGED";
    inline static const std::string ENEMY_MELEE_HIT = "ENEMY_MELEE_HIT";
    inline static const std::string ENEMY_ARROW_HIT = "ENEMY_ARROW_HIT";
    inline static const std::string PLAYER_ARROW = "PLAYER_ARROW";
    inline static const std::string SCRUB_SHOT = "SCRUB_SHOT";
    inline static const std::string ROOM_OBJECT = "ROOM_OBJECT";
    inline static const std::string ENTRANCE_DISCOVERED = "ENTRANCE_DISCOVERED";
    inline static const std::string GAME_COMPLETE = "GAME_COMPLETE";
    inline static const std::string GIVE_ITEM = "GIVE_ITEM";
    inline static const std::string HANDSHAKE = "HANDSHAKE";
    inline static const std::string OCARINA_SFX = "OCARINA_SFX";
    inline static const std::string PLAYER_SFX = "PLAYER_SFX";
    inline static const std::string PLAYER_UPDATE = "PLAYER_UPDATE";
    inline static const std::string REQUEST_TEAM_STATE = "REQUEST_TEAM_STATE";
    inline static const std::string REQUEST_TELEPORT = "REQUEST_TELEPORT";
    inline static const std::string SERVER_MESSAGE = "SERVER_MESSAGE";
    inline static const std::string SET_CHECK_STATUS = "SET_CHECK_STATUS";
    inline static const std::string SET_FLAG = "SET_FLAG";
    inline static const std::string TEAM_CUTSCENE = "TEAM_CUTSCENE";
    inline static const std::string TEAM_CUTSCENE_INPUT = "TEAM_CUTSCENE_INPUT";
    inline static const std::string TELEPORT_TO = "TELEPORT_TO";
    inline static const std::string UNSET_FLAG = "UNSET_FLAG";
    inline static const std::string UPDATE_BEANS_COUNT = "UPDATE_BEANS_COUNT";
    inline static const std::string UPDATE_CLIENT_STATE = "UPDATE_CLIENT_STATE";
    inline static const std::string UPDATE_DUNGEON_ITEMS = "UPDATE_DUNGEON_ITEMS";
    inline static const std::string UPDATE_ROOM_STATE = "UPDATE_ROOM_STATE";
    inline static const std::string UPDATE_TEAM_STATE = "UPDATE_TEAM_STATE";
    inline static const std::string WORLD_STATE = "WORLD_STATE";

    static Anchor* Instance;
    std::map<uint32_t, AnchorClient> clients;
    RoomState roomState;

    void Enable();
    void Disable();
    void OnIncomingJson(nlohmann::json payload);
    void OnConnected();
    void OnDisconnected();
    void ProcessOutgoingPackets();
    void DrawMenu();
    void ProcessIncomingPacketQueue();
    void SendJsonToRemote(nlohmann::json packet);
    bool IsSaveLoaded(bool requirePlayer = true);
    bool CanTeleportTo(uint32_t clientId);
    uint32_t GetDummyPlayerClientId(const Actor* actor);

    void SendPacket_ClearTeamState(std::string teamId);
    void SendPacket_DamagePlayer(u32 clientId, u8 damageEffect, u8 damage);
    void SendPacket_EntranceDiscovered(u16 entranceIndex);
    void SendPacket_GameComplete();
    void SendPacket_GiveItem(u16 modId, s16 getItemId);
    void SendPacket_Handshake();
    void SendPacket_OcarinaSfx(uint8_t note, float modulator, int8_t bend);
    void SendPacket_PlayerSfx(u16 sfxId);
    void SendPacket_PlayerUpdate();
    bool ApplyEnemyPose(Actor* actor, bool gold, int* skulltulaPhase = nullptr, RedeadMotion* redead = nullptr);
    Actor* FindRedeadTarget(Actor* actor);
    bool FindSkulltulaTarget(Actor* actor, s16* yaw);
    void SendEnemyHit(PlayState* play, Collider* defender, ColliderInfo* element);
    void ApplyEnemyHits(PlayState* play, CollisionCheckContext* collisions);
    void SendPlayerArrow(Actor* arrow, bool end);
    bool ShouldSpawnScrubShot(Actor* scrub);
    bool ApplyScrubPose(Actor* actor, ScrubMotion* motion);
    void SendRoomObject(Actor* actor, int phase, int detail);
    int ReadRoomObject(Actor* actor, Vec3f* position, s32* detail, u64* seen);
    void HandlePacket_RoomObject(const nlohmann::json& payload);
    bool ShouldUpdateSharedScrub(Actor* actor);
    void ApplyWorldTimeOnEntry(PlayState* play);
    int ApplyGuardPose(Actor* actor, s16* headAngle);
    void ApplyDampePose(Actor* actor);
    bool ShouldSpawnStalchildren();
    void RegisterStalchild(Actor* actor);
    void SpawnTreeSkulltula(PlayState* play, Vec3f* position, s16 yaw, s16 params);
    void SendDoorOpening(Actor* actor, int leaf, int style);
    int ReadDoorOpening(Actor* actor, int leaf, u64* seen);
    void StartSunStun(PlayState* play);
    int ConsumeSunStun(Actor* actor, u64* seen);
    bool ReadGohmaState(Actor* actor, GohmaTeamState* state, u64* seen);
    uint32_t GetGohmaChildDefeats(const GohmaTeamState* state);
    bool UseSharedGohmaPortal();
    int GetGohmaWarpRequest();
    void StartGohmaWarp(bool story);
    void CommitGohmaWarp();
    void StartZeldaMeeting(PlayState* play);
    void FinishZeldaMeeting();
    bool ForceSunSongPartTwo(PlayState* play);
    bool WillPlayRoyalTombOpening(PlayState* play);
    void SendScrubShot(Actor* actor, int event);
    void SendPacket_RequestTeamState();
    void SendPacket_RequestTeleport(u32 clientId);
    void SendPacket_SetCheckStatus(RandomizerCheck rc);
    void SendPacket_SetFlag(s16 sceneNum, s16 flagType, s16 flag);
    void SendPacket_TeleportTo(u32 clientId);
    void SendPacket_UnsetFlag(s16 sceneNum, s16 flagType, s16 flag);
    void SendPacket_UpdateBeansCount();
    void SendPacket_UpdateClientState();
    void SendPacket_UpdateDungeonItems();
    void SendPacket_UpdateRoomState();
    void SendPacket_UpdateTeamState();
};

typedef enum {
    // Starting at 5 to continue from the last value in the PlayerDamageResponseType enum
    DUMMY_PLAYER_HIT_RESPONSE_STUN = 5,
    DUMMY_PLAYER_HIT_RESPONSE_FIRE,
    DUMMY_PLAYER_HIT_RESPONSE_NORMAL,
} DummyPlayerDamageResponseType;

class AnchorRoomWindow : public Ship::GuiWindow {
  public:
    using GuiWindow::GuiWindow;

    void InitElement() override{};
    void DrawElement() override;
    void Draw() override;
    void UpdateElement() override{};
};

#endif // __cplusplus
