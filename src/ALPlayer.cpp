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
#include "World.h"

#include <limits>
#include <vector>
#include "LootReceipt.h"

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
        return config.IsTarget(player) && !player->HasPlayerFlag(PLAYER_FLAGS_NO_PLAY_TIME) &&
            player->IsInWorld() && player->GetMap() == victim->GetMap() &&
            player->IsAtLootRewardDistance(victim) && player->isAllowedToLoot(victim);
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

    LootReceipt::Money receipt("autoloot", loot, player);
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
    {
        receipt.Recipients(playersNear);
        receipt.Balances(playersNear);
        receipt.End("no_recipients", loot->gold);
        return;
    }

    sScriptMgr->OnPlayerBeforeLootMoney(player, loot);
    receipt.Post(loot->gold);

    receipt.Recipients(playersNear);

    struct Payment
    {
        Player* recipient;
        uint32 amount;
        uint32 previousMoney;
    };
    std::vector<Payment> payments;
    uint32 goldPerPlayer = uint32(loot->gold / playersNear.size());
    LootReceipt::Emit("loot_v1 money_share op={} gold={} count={} share={} remainder={}", receipt.id, loot->gold, playersNear.size(), goldPerPlayer, loot->gold % playersNear.size());
    uint32 trialMoneyCap = sWorld->getIntConfig(CONFIG_TRIAL_MONEY_CAP);
    for (Player* member : playersNear)
    {
        if (member->HasPlayerFlag(PLAYER_FLAGS_NO_PLAY_TIME))
        {
            LootReceipt::Emit("loot_v1 money_skip op={} guid={} reason=no_play_time", receipt.id, member->GetGUID().GetRawValue());
            continue;
        }

        uint32 finalGold = goldPerPlayer;
        if (member->HasPlayerFlag(PLAYER_FLAGS_PARTIAL_PLAY_TIME))
            finalGold /= 2;
        if (!finalGold)
        {
            LootReceipt::Emit("loot_v1 money_skip op={} guid={} reason=zero_share", receipt.id, member->GetGUID().GetRawValue());
            continue;
        }

        uint64 total = uint64(member->GetMoney()) + finalGold;
        if (finalGold > uint32(std::numeric_limits<int32>::max()) || total >= uint64(MAX_MONEY_AMOUNT) ||
            (trialMoneyCap && member->GetSession()->IsTrialAccount() && total > trialMoneyCap))
        {
            member->SendEquipError(EQUIP_ERR_TOO_MUCH_GOLD, nullptr, nullptr);
            receipt.Balances(playersNear);
            receipt.End("preflight_refused", loot->gold);
            return;
        }
        LootReceipt::Emit("loot_v1 money_plan op={} guid={} amount={}", receipt.id, member->GetGUID().GetRawValue(), finalGold);
        payments.push_back({ member, finalGold, member->GetMoney() });
    }
    if (payments.empty())
    {
        receipt.Balances(playersNear);
        receipt.End("no_payments", loot->gold);
        return;
    }

    // shortcut: custom money hooks need transactional payouts; SetMoney cannot undo their external/achievement effects.
    for (Payment const& payment : payments)
    {
        uint32 const receiptBefore = payment.recipient->GetMoney();
        bool const receiptAccepted = payment.recipient->ModifyMoney(int32(payment.amount));
        receipt.Credit(payment.recipient->GetGUID().GetRawValue(), payment.amount,
            receiptBefore, payment.recipient->GetMoney(), receiptAccepted, "modify");
        if (!receiptAccepted)
        {
            for (Payment const& rollback : payments)
                if (rollback.recipient->GetMoney() != rollback.previousMoney)
                {
                    uint32 const receiptRollbackBefore = rollback.recipient->GetMoney();
                    rollback.recipient->SetMoney(rollback.previousMoney);
                    receipt.Credit(rollback.recipient->GetGUID().GetRawValue(), 0,
                        receiptRollbackBefore, rollback.recipient->GetMoney(), true, "rollback");
                }
            receipt.Balances(playersNear);
            receipt.End("modify_refused_rollback", loot->gold);
            return;
        }
    }

    uint32 const receiptRemoved = loot->gold;
    loot->gold = 0;
    loot->NotifyMoneyRemoved();
    for (Payment const& payment : payments)
    {
        payment.recipient->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_LOOT_MONEY, payment.amount);
        WorldPacket data(SMSG_LOOT_MONEY_NOTIFY, 4 + 1);
        data << uint32(payment.amount);
        data << uint8(playersNear.size() > 1 ? 0 : 1);
        payment.recipient->SendDirectMessage(&data);
    }
    receipt.Balances(playersNear);
    receipt.End("removed", loot->gold, receiptRemoved);

}

void AutoLootItems(Player* player, Creature* victim)
{
    Loot* loot = &victim->loot;
    uint32 itemCount = uint32(loot->items.size());
    uint32 maxSlot = loot->GetMaxSlotInLootFor(player);

    for (uint32 slot = 0; slot < maxSlot; ++slot)
    {
        // Quest, free-for-all, and conditional per-player items follow the
        // same StoreLootItem path as a normal client loot request.
        if (slot >= itemCount)
        {
            LootItem* item = loot->LootItemInSlot(slot, player);
            if (!item || (item->follow_loot_rules && item->is_blocked))
                continue;

            InventoryResult result;
            player->StoreLootItem(uint8(slot), loot, result);
            continue;
        }

        LootItem& item = loot->items[slot];
        if (item.is_looted || item.is_blocked)
            continue;

        // Round-robin, master-loot, and completed group-roll allocations must
        // remain available to the player or companion selected by the core.
        if (Group* group = player->GetGroup())
            if (!item.freeforall && item.conditions.empty() && loot->roundRobinPlayer &&
                loot->roundRobinPlayer != player->GetGUID() &&
                (group->GetLootMethod() == ROUND_ROBIN ||
                 (item.is_underthreshold && (group->GetLootMethod() == GROUP_LOOT ||
                                            group->GetLootMethod() == NEED_BEFORE_GREED ||
                                            group->GetLootMethod() == MASTER_LOOT))))
                continue;

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
