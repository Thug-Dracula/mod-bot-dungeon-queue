#ifndef BOT_DUNGEON_QUEUE_H
#define BOT_DUNGEON_QUEUE_H

#include "Define.h"
#include <vector>

struct DungeonPort
{
    uint8 minLvl;
    uint8 maxLvl;
    uint32 mapId;
    float x, y, z, o;
};

extern std::vector<DungeonPort> const BotDungeonPorts;

void AddSC_bot_dungeon_queue_module();

#endif
