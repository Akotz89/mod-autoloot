# mod-autoloot
This module automatically loots creatures as soon as they are killed.

*It is based on an old TrinityCore C++ script. Unfortunately, I could not find the original author. If they come forward, I will gladly add proper attribution to this project.*

## Server-specific compatibility revision

This checkout keeps the catalogue module's automatic-on-death behavior while
preserving AzerothCore's normal group ownership and item-storage rules. It is
configured through `AutoLoot.Enable` for eligible human and bot players.
It handles direct, combat-pet and party kills under core loot eligibility, and
keeps items assigned to another companion or awaiting a group roll. The
automation is disabled by default. Gold-cap refusals preserve corpse gold.
