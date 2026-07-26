/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

#include "Player.h"
#include "ScriptMgr.h"

class ALPlayer : public PlayerScript
{
public:
    ALPlayer() : PlayerScript("ALPlayer", {
        PLAYERHOOK_ON_CREATURE_KILL
    }) { }

    void OnPlayerCreatureKill(Player* player, Creature* creature) override
    {
        if (player->GetGroup() || !player->isAllowedToLoot(creature))
            return;

        Loot* loot = &creature->loot;
        if (loot->isLooted() || loot->empty())
            return;

        uint32 gold = loot->gold;
        if (gold > 0)
        {
            player->ModifyMoney(gold);
            player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_LOOT_MONEY, loot->gold);
    
            WorldPacket data(SMSG_LOOT_MONEY_NOTIFY, 4 + 1);
            data << uint32(loot->gold);
            data << uint8(1); 
            player->GetSession()->SendPacket(&data);
        }


        uint8 maxSlot = loot->GetMaxSlotInLootFor(player);
        for (uint32 i = 0; i < maxSlot; ++i) 
        {
            LootItem* item = loot->LootItemInSlot(i, player);
            if(player->AddItem(item->itemid, item->count))
            {
                item->is_looted = true;
                player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_LOOT_ITEM, item->itemid, item->count);
                player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_LOOT_TYPE, loot->loot_type, item->count);
                player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_LOOT_EPIC_ITEM, item->itemid, item->count);
            }
            else
                player->SendItemRetrievalMail(item->itemid, item->count);
        }

        loot->clear();
    }
};

void AddALPlayerScripts()
{
    new ALPlayer();
}