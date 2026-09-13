/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 *
 * Based on Grim-Batol/mod-autoloot. This server-specific revision preserves
 * AzerothCore's group ownership, roll, item-storage, and money-sharing rules.
 */

#include "Config.h"
#include "Creature.h"
#include "Group.h"
#include "Log.h"
#include "LootMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <vector>

namespace
{
class AutoLootConfig
{
public:
    static AutoLootConfig& Instance()
    {
        static AutoLootConfig instance;
        return instance;
    }

    void Load()
    {
        Enabled = sConfigMgr->GetOption<bool>("AutoLoot.Enable", false);

        LOG_INFO("module.autoloot", "AutoLoot: enabled={}", Enabled);
    }

    bool IsTarget(Player const* player) const
    {
        return player != nullptr;
    }

    bool Enabled = false;
};

std::vector<Player*> FindEligibleTargets(Player* creditedPlayer, Creature* victim)
{
    AutoLootConfig const& config = AutoLootConfig::Instance();

    auto isEligible = [&config, victim](Player* player)
    {
        if (!config.IsTarget(player) || !player->IsInWorld() || player->GetMap() != victim->GetMap() ||
            !player->IsAtLootRewardDistance(victim))
            return false;

        // Each party member has an independent quest/FFA/conditional loot
        // view. Build it before asking the core whether that member can loot.
        victim->loot.FillNotNormalLootFor(player);
        return player->isAllowedToLoot(victim);
    };

    std::vector<Player*> targets;
    if (isEligible(creditedPlayer))
        targets.push_back(creditedPlayer);

    Group* recipientGroup = victim->GetLootRecipientGroup();
    if (!recipientGroup)
        return targets;

    for (GroupReference* itr = recipientGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* member = itr->GetSource();
        if (member != creditedPlayer && isEligible(member))
            targets.push_back(member);
    }

    return targets;
}

void StartGroupLootIfNeeded(Player* player, Creature* victim)
{
    if (victim->loot.loot_type != LOOT_NONE)
        return;

    victim->loot.loot_type = LOOT_CORPSE;

    Group* group = player->GetGroup();
    if (!group)
        return;

    switch (group->GetLootMethod())
    {
        case GROUP_LOOT:
            group->GroupLoot(&victim->loot, victim);
            break;
        case NEED_BEFORE_GREED:
            group->NeedBeforeGreed(&victim->loot, victim);
            break;
        case MASTER_LOOT:
            group->MasterLoot(&victim->loot, victim);
            break;
        default:
            break;
    }
}

void AutoLootMoney(Player* player, Creature* victim)
{
    Loot* loot = &victim->loot;
    if (!loot->gold || player->HasPlayerFlag(PLAYER_FLAGS_NO_PLAY_TIME))
        return;

    Group* group = player->GetGroup();
    std::vector<Player*> playersNear;

    if (group)
    {
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (member && player->IsAtLootRewardDistance(member))
                playersNear.push_back(member);
        }
    }
    else
        playersNear.push_back(player);

    // Never destroy money if the core cannot identify an eligible recipient.
    if (playersNear.empty())
        return;

    sScriptMgr->OnPlayerBeforeLootMoney(player, loot);
    loot->NotifyMoneyRemoved();

    uint32 goldPerPlayer = uint32(loot->gold / playersNear.size());
    for (Player* member : playersNear)
    {
        if (member->HasPlayerFlag(PLAYER_FLAGS_NO_PLAY_TIME))
            continue;

        uint32 finalGold = goldPerPlayer;
        if (member->HasPlayerFlag(PLAYER_FLAGS_PARTIAL_PLAY_TIME))
            finalGold /= 2;

        if (!finalGold)
            continue;

        member->ModifyMoney(finalGold);
        member->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_LOOT_MONEY, finalGold);

        WorldPacket data(SMSG_LOOT_MONEY_NOTIFY, 4 + 1);
        data << uint32(finalGold);
        data << uint8(playersNear.size() > 1 ? 0 : 1);
        member->SendDirectMessage(&data);
    }

    loot->gold = 0;
}

void AutoLootItems(Player* player, Creature* victim)
{
    Loot* loot = &victim->loot;
    // The client fills the per-player quest/FFA/conditional lists while
    // opening the loot window. Server-side auto-loot has no such window, so
    // prepare the same core view before calculating the slot range.
    loot->FillNotNormalLootFor(player);
    uint32 itemCount = uint32(loot->items.size());
    uint32 maxSlot = loot->GetMaxSlotInLootFor(player);

    for (uint32 slot = 0; slot < maxSlot; ++slot)
    {
        // Quest, free-for-all, and conditional per-player items follow the
        // same StoreLootItem path as a normal client loot request.
        if (slot >= itemCount)
        {
            InventoryResult result;
            player->StoreLootItem(uint8(slot), loot, result);
            continue;
        }

        LootItem& item = loot->items[slot];
        if (item.is_looted || item.is_blocked)
            continue;

        // Round-robin, master-loot, and completed group-roll allocations must
        // remain available to the player or companion selected by the core.
        AllowedLooterSet const& allowedLooters = item.GetAllowedLooters();
        if (!allowedLooters.empty() && !allowedLooters.count(player->GetGUID()))
            continue;

        InventoryResult result;
        player->StoreLootItem(uint8(slot), loot, result);
    }
}

void AutoLootCreature(Player* creditedPlayer, Creature* victim)
{
    if (!AutoLootConfig::Instance().Enabled || !victim || victim->IsAlive())
        return;

    std::vector<Player*> players = FindEligibleTargets(creditedPlayer, victim);
    if (players.empty())
        return;

    Loot* loot = &victim->loot;
    for (Player* player : players)
        loot->FillNotNormalLootFor(player);

    if (loot->isLooted())
        return;

    StartGroupLootIfNeeded(players.front(), victim);
    AutoLootMoney(players.front(), victim);
    for (Player* player : players)
        AutoLootItems(player, victim);

    if (loot->isLooted())
    {
        victim->RemoveFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_LOOTABLE);
        victim->AllLootRemovedFromCorpse();
    }

    LOG_INFO("module.autoloot", "AutoLoot: processed corpse {} for {} eligible players after kill credit to {}",
        victim->GetGUID().ToString(), players.size(), creditedPlayer ? creditedPlayer->GetName() : "none");
}
}

class AutoLootWorldScript : public WorldScript
{
public:
    AutoLootWorldScript() : WorldScript("AutoLootWorldScript") { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        AutoLootConfig::Instance().Load();
    }
};

class AutoLootUnitScript : public UnitScript
{
public:
    AutoLootUnitScript() : UnitScript("AutoLootUnitScript", true, { UNITHOOK_ON_UNIT_DEATH }) { }

    void OnUnitDeath(Unit* unit, Unit* killer) override
    {
        if (!unit || !killer)
            return;

        Creature* victim = unit->ToCreature();
        Player* creditedPlayer = killer->GetCharmerOrOwnerPlayerOrPlayerItself();
        AutoLootCreature(creditedPlayer, victim);
    }
};

void AddALPlayerScripts()
{
    new AutoLootWorldScript();
    new AutoLootUnitScript();
}
