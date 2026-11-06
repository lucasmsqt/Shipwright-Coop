#include <nlohmann/json.hpp>

#include "soh/Network/Anchor/Anchor.h"
#include "soh/Enhancements/randomizer/static_data.h"
#include "soh/Notification/Notification.h"
#include "soh/SohGui/ImGuiUtils.h"
#include "soh/Enhancements/item-tables/ItemTableManager.h"
#include <algorithm>

extern "C" {
#include "functions.h"
#include "macros.h"
extern PlayState* gPlayState;
}

/**
 * GIVE_ITEM
 */

uint8_t incomingIceTrapsFromAnchor = 0;

void Anchor::MergeInventoryUpgrades(Inventory& inventory, uint32_t local, uint32_t remote) {
    constexpr std::pair<int, int> tools[] = {
        { UPG_QUIVER, ITEM_BOW }, { UPG_BOMB_BAG, ITEM_BOMB }, { UPG_BULLET_BAG, ITEM_SLINGSHOT },
        { UPG_STICKS, ITEM_STICK }, { UPG_NUTS, ITEM_NUT },
    };
    for (int type = UPG_QUIVER; type <= UPG_NUTS; ++type) {
        const uint32_t mask = gUpgradeMasks[type];
        const int shift = gUpgradeShifts[type];
        const uint32_t oldLevel = (local & mask) >> shift;
        const uint32_t peerLevel = (remote & mask) >> shift;
        const uint32_t level = std::max(oldLevel, peerLevel <= 3 ? peerLevel : oldLevel);
        inventory.upgrades = (inventory.upgrades & ~mask) | ((level << shift) & mask);
    }
    for (const auto [type, item] : tools) {
        if ((inventory.upgrades & gUpgradeMasks[type]) != 0 && inventory.items[SLOT(item)] == ITEM_NONE) {
            inventory.items[SLOT(item)] = static_cast<u8>(item);
        }
    }
}

void Anchor::SendPacket_GiveItem(u16 modId, s16 getItemId) {
    // Entrance cutscenes can grant items before the player actor is spawned.
    if (!IsSaveLoaded(false) || isProcessingIncomingPacket || !roomState.syncItemsAndFlags) {
        return;
    }

    if (modId == MOD_RANDOMIZER && getItemId == RG_ICE_TRAP && incomingIceTrapsFromAnchor > 0) {
        incomingIceTrapsFromAnchor = MAX(incomingIceTrapsFromAnchor - 1, 0);
        return;
    }

    // Require an initialized scene to rule out the temporary Master Sword from the final Ganon fight.
    if (modId == MOD_RANDOMIZER && getItemId == RG_MASTER_SWORD &&
        (!IsSaveLoaded() || gPlayState->sceneNum == SCENE_GANON_BOSS)) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = GIVE_ITEM;
    payload["targetTeamId"] = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
    payload["addToQueue"] = true;
    payload["modId"] = modId;
    payload["getItemId"] = getItemId;
    payload["upgrades"] = gSaveContext.inventory.upgrades;

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_GiveItem(nlohmann::json payload) {
    if (!IsSaveLoaded() || !roomState.syncItemsAndFlags) {
        return;
    }

    uint32_t clientId = payload.at("clientId").get<uint32_t>();
    AnchorClient& client = clients[clientId];
    u16 modId = payload.at("modId").get<u16>();
    u16 getItemId = payload.at("getItemId").get<u16>();
    const uint32_t priorUpgrades = gSaveContext.inventory.upgrades;

    GetItemEntry getItemEntry;
    if (modId == MOD_NONE) {
        getItemEntry = ItemTableManager::Instance->RetrieveItemEntry(MOD_NONE, getItemId);
    } else {
        getItemEntry = Rando::StaticData::RetrieveItem(static_cast<RandomizerGet>(getItemId)).GetGIEntry_Copy();
    }

    if (getItemEntry.modIndex == MOD_NONE) {
        // A late Fairy Ocarina packet must not overwrite the Ocarina of Time.
        if (getItemEntry.itemId == ITEM_OCARINA_FAIRY && INV_CONTENT(ITEM_OCARINA_TIME) == ITEM_OCARINA_TIME) {
            return;
        }
        if (getItemEntry.getItemId == GI_SWORD_BGS) {
            gSaveContext.bgsFlag = true;
        }
        Item_Give(gPlayState, static_cast<u8>(getItemEntry.itemId));
    } else if (getItemEntry.modIndex == MOD_RANDOMIZER) {
        if (getItemEntry.getItemId == RG_ICE_TRAP) {
            gSaveContext.ship.pendingIceTrapCount++;
            incomingIceTrapsFromAnchor++;
        } else {
            Randomizer_Item_Give(gPlayState, getItemEntry);
        }
    }

    if (payload.contains("upgrades")) {
        MergeInventoryUpgrades(gSaveContext.inventory, priorUpgrades, payload.at("upgrades").get<uint32_t>());
    } else {
        MergeInventoryUpgrades(gSaveContext.inventory, priorUpgrades, gSaveContext.inventory.upgrades);
    }

    // Full heal if getting a heart container or piece
    if (getItemEntry.gid == GID_HEART_CONTAINER || getItemEntry.gid == GID_HEART_PIECE) {
        gSaveContext.healthAccumulator = 0x140;
    }

    // Handle if the player gets a 4th heart piece (usually handled in z_message)
    s32 heartPieces = (s32)(gSaveContext.inventory.questItems & 0xF0000000) >> (QUEST_HEART_PIECE + 4);
    if (heartPieces >= 4) {
        gSaveContext.inventory.questItems &= ~0xF0000000;
        gSaveContext.inventory.questItems += (heartPieces % 4) << (QUEST_HEART_PIECE + 4);
        gSaveContext.healthCapacity += 0x10 * (heartPieces / 4);
        gSaveContext.health += 0x10 * (heartPieces / 4);
    }

    if (getItemEntry.getItemCategory != ITEM_CATEGORY_JUNK) {
        if (getItemEntry.modIndex == MOD_NONE) {
            Notification::Emit({
                .itemIcon = GetTextureForItemId(getItemEntry.itemId),
                .prefix = client.name,
                .message = "found",
                .suffix = SohUtils::GetItemName(getItemEntry.itemId),
            });
        } else if (getItemEntry.modIndex == MOD_RANDOMIZER) {
            Notification::Emit({
                .prefix = client.name,
                .message = "found",
                .suffix = Rando::StaticData::RetrieveItem((RandomizerGet)getItemEntry.getItemId).GetName().english,
            });
        }
    }
}
