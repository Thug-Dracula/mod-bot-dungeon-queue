#include "BotDungeonQueue.h"

#include "ScriptMgr.h"
#include "Log.h"
#include "Player.h"
#include "PlayerScript.h"
#include "WorldScript.h"
#include "Group.h"
#include "InstanceSaveMgr.h"
#include "LFG.h"
#include "LFGMgr.h"
#include "Map.h"
#include "MapMgr.h"
#include "DBCStores.h"
#include "GameTime.h"

#include <map>

#include "Playerbots.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "RandomPlayerbotMgr.h"
#include "AiFactory.h"

#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"
#include "Ai/Dungeon/DungeonClear/Util/DcLeaderSignal.h"
#include "Ai/Dungeon/DungeonClear/Util/NavmeshSnap.h"
#include "DcStrategyGate.h"

using namespace lfg;

namespace
{
    std::unordered_map<ObjectGuid, uint32> g_dungeonEntryTimeMs;
    std::unordered_map<ObjectGuid, bool> g_snapInProgress;
    std::map<uint32, uint32> g_wipeCount;
}

std::vector<DungeonPort> const BotDungeonPorts = {
    {15, 26,  36,  -16.4f,    -383.07f,    61.78f,   1.86f},    // Deadmines — FULL NAVMESH
    {16, 30,  33, -229.135f,  2109.18f,    76.8898f, 1.267f},   // Shadowfang Keep — FULL NAVMESH
    {19, 40,  48, -151.89f,    106.96f,   -39.87f,   4.53f},    // Blackfathom Deeps — FULL NAVMESH
    {20, 45,  34,   54.23f,      0.28f,   -18.34f,   6.26f},    // Stormwind Stockade — FULL NAVMESH
    {37, 55, 129, 2529.98f,    1044.96f,    46.59f,   1.77f},   // Razorfen Downs — FULL NAVMESH (enemies ~35-40)
    {28, 55,  70,    5.25f,     -2.64f,     1.93f,   0.02f},    // Uldaman — FULL NAVMESH
    {29, 60,  90, -7599.52f,   806.29f,   1569.7f,   6.10f},    // Zul'Farak — FULL NAVMESH
    {30, 55, 349, -173.53f,     30.69f,    -6.48f,   0.04f},    // Maraudon — FULL NAVMESH
    {31, 55, 109, 4610.12f,     -896.64f,   30.45f,  5.98f},    // Sunken Temple — FULL NAVMESH
    {35, 60, 289, -471.51f,    -266.27f,   169.59f,  5.71f},    // Blackrock Depths — FULL NAVMESH
    {40, 60, 230,  149.63f,     -41.87f,    36.74f,  0.24f},    // Blackrock Spire — FULL NAVMESH
    {45, 60, 201, -5430.22f,   -2946.53f,   92.08f,  5.37f},    // Stratholme — FULL NAVMESH
    {45, 60, 329, 2758.35f,     -404.31f,   115.14f, 2.53f},    // Hellfire Ramparts — FULL NAVMESH
    {48, 60, 269, -11831.0f,    -4819.0f,    0.55f,  0.14f},    // Caverns of Time — FULL NAVMESH
    // No navmesh for these — DungeonSpawnGraph fallback:
    {15, 25, 389,    3.81f,     -14.82f,   -17.84f,   4.39f},    // Ragefire Chasm — partial navmesh
    // {20, 45,  43, -146.49f,   -265.56f,    17.22f,   1.18f},  // Wailing Caverns
    // {20, 50,  47, 1043.64f,   -474.57f,   -44.04f,   5.07f},  // Razorfen Kraul
};

namespace BotDungeonQueueConfig
{
    bool Enable() { return sConfigMgr->GetOption<bool>("BotDungeonQueue.Enable", true); }
    uint32 QueueInterval() { return sConfigMgr->GetOption<uint32>("BotDungeonQueue.QueueInterval", 30); }
    uint32 MinLevel() { return sConfigMgr->GetOption<uint32>("BotDungeonQueue.MinLevel", 15); }
    uint32 MaxBotsPct() { return sConfigMgr->GetOption<uint32>("BotDungeonQueue.MaxBotsPct", 50); }
    bool RespectBgQueue() { return sConfigMgr->GetOption<bool>("BotDungeonQueue.RespectBgQueue", true); }
    bool WhisperReplies() { return sConfigMgr->GetOption<bool>("BotDungeonQueue.WhisperReplies", true); }
    bool TeleportOutOnDeath() { return sConfigMgr->GetOption<bool>("BotDungeonQueue.TeleportOutOnDeath", true); }
    uint32 MaxWipesBeforeEvict() { return sConfigMgr->GetOption<uint32>("BotDungeonQueue.MaxWipesBeforeEvict", 4); }
    uint32 StuckCleanupInterval() { return sConfigMgr->GetOption<uint32>("BotDungeonQueue.StuckCleanupInterval", 300); }
    uint32 StuckCleanupGracePeriod() { return sConfigMgr->GetOption<uint32>("BotDungeonQueue.StuckCleanupGracePeriod", 300); }
}

static bool IsBotEligible(Player* bot)
{
    if (!bot || !bot->IsInWorld() || bot->isDead() || bot->GetGroup())
        return false;
    if (bot->GetLevel() < BotDungeonQueueConfig::MinLevel())
        return false;
    if (sLFGMgr->GetState(bot->GetGUID()) != lfg::LFG_STATE_NONE)
        return false;
    if (BotDungeonQueueConfig::RespectBgQueue() && bot->InBattlegroundQueue())
        return false;
    if (bot->IsInFlight())
        return false;
    if (bot->GetMap() && bot->GetMap()->IsDungeon())
        return false;
    return true;
}

static uint8 GetBotRole(Player* bot)
{
    uint8 tab = AiFactory::GetPlayerSpecTab(bot);
    switch (bot->getClass())
    {
        case CLASS_DRUID:
            if (tab == 2) return PLAYER_ROLE_HEALER;
            if (tab == 1) return PLAYER_ROLE_TANK;
            return PLAYER_ROLE_DAMAGE;
        case CLASS_PALADIN:
            if (tab == 1) return PLAYER_ROLE_TANK;
            if (tab == 0) return PLAYER_ROLE_HEALER;
            return PLAYER_ROLE_DAMAGE;
        case CLASS_PRIEST:
            return (tab == 2) ? PLAYER_ROLE_DAMAGE : PLAYER_ROLE_HEALER;
        case CLASS_SHAMAN:
            return (tab == 2) ? PLAYER_ROLE_HEALER : PLAYER_ROLE_DAMAGE;
        case CLASS_WARRIOR:
            return (tab == 2) ? PLAYER_ROLE_TANK : PLAYER_ROLE_DAMAGE;
        case CLASS_DEATH_KNIGHT:
            return (tab == 0) ? PLAYER_ROLE_TANK : PLAYER_ROLE_DAMAGE;
        case CLASS_HUNTER:
        case CLASS_ROGUE:
        case CLASS_MAGE:
        case CLASS_WARLOCK:
        default:
            return PLAYER_ROLE_DAMAGE;
    }
}

static void TeleportGroupHome(Group* group)
{
    if (!group)
        return;
    for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
    {
        Player* m = itr->GetSource();
        if (!m || !m->IsInWorld() || m->IsBeingTeleported())
            continue;
        m->TeleportTo(m->m_homebindMapId, m->m_homebindX, m->m_homebindY, m->m_homebindZ, 0.0f);
    }
    // Disband so evicted bots become eligible for re-queuing
    group->Disband();
}

static void EnableDcOn(Player* p, bool isTank = false)
{
    PlayerbotAI* ai = GET_PLAYERBOT_AI(p);
    if (!ai) return;
    if (!ai->HasStrategy("dungeon clear", BOT_STATE_NON_COMBAT))
        ai->ChangeStrategy("+dungeon clear", BOT_STATE_NON_COMBAT);
    if (!ai->HasStrategy("dungeon clear combat", BOT_STATE_COMBAT))
        ai->ChangeStrategy("+dungeon clear combat", BOT_STATE_COMBAT);
    // Tank strategy is class-specific and only registered for the combat engine
    if (isTank)
    {
        bool had = ai->HasStrategy("tank", BOT_STATE_COMBAT);
        bool hadBear = ai->HasStrategy("bear", BOT_STATE_COMBAT);
        if (!had)
            ai->ChangeStrategy("+tank", BOT_STATE_COMBAT);
        bool now = ai->HasStrategy("tank", BOT_STATE_COMBAT);
        bool nowBear = ai->HasStrategy("bear", BOT_STATE_COMBAT);
        LOG_INFO("playerbots", "mod-bot-dungeon-queue: tank strat for {}: had={}/{} now={}/{}",
                 p->GetName(), had, hadBear, now, nowBear);
    }
    DcRunState& rs = DcRun::Of(ai->GetAiObjectContext());
    rs = DcRunState{};
    // Enable DC for ALL party members so the non-combat multiplier suppresses
    // wander and proactive-engage for everyone. The advance trigger is gated by
    // IsDungeonClearLeader which correctly resolves the tank — followers with
    // enabled=true won't try to drive but will stay with the group instead of
    // falling into New RPG. Only the actual tank gets the tank strategy.
    rs.enabled = true;
}

class BotDungeonQueueWorldScript : public WorldScript
{
public:
    BotDungeonQueueWorldScript() : WorldScript("BotDungeonQueueWorldScript") { }

    void OnStartup() override
    {
        if (!BotDungeonQueueConfig::Enable())
            return;
        LOG_INFO("playerbots", "mod-bot-dungeon-queue: enabled (queue every {}s, cleanup every {}s)",
                 BotDungeonQueueConfig::QueueInterval(),
                 BotDungeonQueueConfig::StuckCleanupInterval());
    }

    void OnUpdate(uint32 diff) override
    {
        if (!BotDungeonQueueConfig::Enable())
            return;

        if (!m_startupCleanupDone)
        {
            m_startupCleanupDone = true;
            g_dungeonEntryTimeMs.clear();
            LOG_INFO("playerbots", "mod-bot-dungeon-queue: startup cleanup — removing all bots from dungeons");
            for (PlayerBotMap::const_iterator it = sRandomPlayerbotMgr.GetPlayerBotsBegin();
                 it != sRandomPlayerbotMgr.GetPlayerBotsEnd(); ++it)
            {
                Player* bot = it->second;
                if (bot && bot->IsInWorld() && bot->GetMap() && bot->GetMap()->IsDungeon())
                    bot->TeleportTo(bot->m_homebindMapId, bot->m_homebindX, bot->m_homebindY, bot->m_homebindZ, bot->GetOrientation());
            }
            return;
        }

        // Delayed cleanup: catch bots that weren't in-world during the first pass
        if (!m_delayedCleanupDone)
        {
            m_delayedCleanupDone = true;
            for (PlayerBotMap::const_iterator it = sRandomPlayerbotMgr.GetPlayerBotsBegin();
                 it != sRandomPlayerbotMgr.GetPlayerBotsEnd(); ++it)
            {
                Player* bot = it->second;
                if (bot && bot->IsInWorld() && bot->GetMap() && bot->GetMap()->IsDungeon())
                {
                    LOG_INFO("playerbots", "mod-bot-dungeon-queue: delayed cleanup teleporting {} home from stale map {}",
                             bot->GetName(), bot->GetMapId());
                    bot->TeleportTo(bot->m_homebindMapId, bot->m_homebindX, bot->m_homebindY, bot->m_homebindZ, bot->GetOrientation());
                }
            }
        }

        m_queueTimer += diff;
        m_cleanupTimer += diff;
        m_skullSyncTimer += diff;
        m_heartbeatTimer += diff;

        if (m_heartbeatTimer >= 60000)
        {
            m_heartbeatTimer = 0;
            LOG_INFO("playerbots", "mod-bot-dungeon-queue: heartbeat OK");
            // Log real-time positions and DC state of bots in dungeons
            for (PlayerBotMap::const_iterator it = sRandomPlayerbotMgr.GetPlayerBotsBegin();
                 it != sRandomPlayerbotMgr.GetPlayerBotsEnd(); ++it)
            {
                Player* bot = it->second;
                if (!bot || !bot->IsInWorld())
                    continue;
                Map* m = bot->GetMap();
                if (!m || !m->IsDungeon())
                    continue;
                InstanceMap* im = m->ToInstanceMap();
                uint32 instId = im ? im->GetInstanceId() : 0;
                PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
                bool isTank = ai && PlayerbotAI::IsTank(bot);
                bool dcEnabled = false;
                bool dcPaused = false;
                bool isLeader = false;
                bool hasBear = false;
                if (ai)
                {
                    DcRunState& rs = DcRun::Of(ai->GetAiObjectContext());
                    dcEnabled = rs.enabled;
                    dcPaused = rs.paused;
                    isLeader = DcLeaderSignal::IsDungeonClearLeader(bot);
                    hasBear = ai->HasStrategy("tank", BOT_STATE_COMBAT) || ai->HasStrategy("bear", BOT_STATE_COMBAT);
                }
                LOG_INFO("playerbots", "  dc {}: map={} inst={} ({:.1f}, {:.1f}, {:.1f}) tank={} en={} pause={} lead={} bear={}",
                         bot->GetName(), m->GetId(), instId,
                         bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                         isTank, dcEnabled, dcPaused, isLeader, hasBear);
            }
        }

        // DC watchdog: re-enable dungeon clear on the tank if it was
        // disabled by death but the tank is alive and still in a dungeon.
        for (PlayerBotMap::const_iterator it = sRandomPlayerbotMgr.GetPlayerBotsBegin();
             it != sRandomPlayerbotMgr.GetPlayerBotsEnd(); ++it)
        {
            Player* bot = it->second;
            if (!bot || !bot->IsInWorld() || !bot->IsAlive())
                continue;
            Map* m = bot->GetMap();
            if (!m || !m->IsDungeon())
                continue;
            if (!PlayerbotAI::IsTank(bot))
                continue;
            PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
            if (!ai)
                continue;
            DcRunState& rs = DcRun::Of(ai->GetAiObjectContext());
            if (!rs.enabled)
            {
                LOG_INFO("playerbots", "mod-bot-dungeon-queue: DC watchdog re-enabling {} in map {}",
                         bot->GetName(), m->GetId());
                rs = DcRunState{};
                rs.enabled = true;
                if (!ai->HasStrategy("dungeon clear", BOT_STATE_NON_COMBAT))
                    ai->ChangeStrategy("+dungeon clear", BOT_STATE_NON_COMBAT);
                if (!ai->HasStrategy("dungeon clear combat", BOT_STATE_COMBAT))
                    ai->ChangeStrategy("+dungeon clear combat", BOT_STATE_COMBAT);
                if (!ai->HasStrategy("tank", BOT_STATE_COMBAT))
                    ai->ChangeStrategy("+tank", BOT_STATE_COMBAT);
            }
        }

        // Tank strategy re-add: teleport engine init can strip the tank strategy
        // even when DcRunState.enabled stays true. Re-add for any tank-role bot
        // that's missing it. Uses GetBotRole (spec tab) for detection since
        // PlayerbotAI::IsTank might fail without the strategy present.
        for (PlayerBotMap::const_iterator it = sRandomPlayerbotMgr.GetPlayerBotsBegin();
             it != sRandomPlayerbotMgr.GetPlayerBotsEnd(); ++it)
        {
            Player* bot = it->second;
            if (!bot || !bot->IsInWorld() || !bot->IsAlive())
                continue;
            Map* m = bot->GetMap();
            if (!m || !m->IsDungeon())
                continue;
            if (!(GetBotRole(bot) & PLAYER_ROLE_TANK))
                continue;
            PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
            if (!ai)
                continue;
            bool hasTankStrat = ai->HasStrategy("tank", BOT_STATE_COMBAT) ||
                                ai->HasStrategy("bear", BOT_STATE_COMBAT) ||
                                ai->HasStrategy("blood", BOT_STATE_COMBAT);
            if (!hasTankStrat)
            {
                LOG_INFO("playerbots", "mod-bot-dungeon-queue: re-adding tank strategy for {} in map {}",
                         bot->GetName(), m->GetId());
                ai->ChangeStrategy("+tank", BOT_STATE_COMBAT);
            }
        }

        if (m_skullSyncTimer >= 100)
        {
            m_skullSyncTimer = 0;
            for (PlayerBotMap::const_iterator it = sRandomPlayerbotMgr.GetPlayerBotsBegin();
                 it != sRandomPlayerbotMgr.GetPlayerBotsEnd(); ++it)
            {
                Player* bot = it->second;
                if (!bot || !bot->IsInWorld())
                    continue;
                Map* m = bot->GetMap();
                if (!m || !m->IsDungeon())
                    continue;
                Group* g = bot->GetGroup();
                if (!g || g->isLFGGroup())
                    continue;
                if (g->GetLeaderGUID() != bot->GetGUID())
                    continue;
                PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
                if (!ai || !PlayerbotAI::IsTank(bot))
                    continue;
                Unit* target = bot->GetSelectedUnit();
                if (target && target->IsInCombat() && target->GetTypeId() == TYPEID_UNIT)
                    g->SetTargetIcon(0, bot->GetGUID(), target->GetGUID());
            }
        }

        if (m_queueTimer >= BotDungeonQueueConfig::QueueInterval() * 1000)
        {
            m_queueTimer = 0;
            RunQueueCheck();
        }

        if (m_cleanupTimer >= BotDungeonQueueConfig::StuckCleanupInterval() * 1000)
        {
            LOG_INFO("playerbots", "mod-bot-dungeon-queue: cleanup FIRING (timer={})", m_cleanupTimer);
            m_cleanupTimer = 0;
            RunStuckCleanup();
        }

        // Death-release sweep
        for (PlayerBotMap::const_iterator it = sRandomPlayerbotMgr.GetPlayerBotsBegin();
             it != sRandomPlayerbotMgr.GetPlayerBotsEnd(); ++it)
        {
            Player* bot = it->second;
            if (!bot || bot->IsAlive() || bot->HasPlayerFlag(PLAYER_FLAGS_GHOST))
                continue;
            Map* m = bot->GetMap();
            if (!m || !m->IsDungeon())
                continue;
            uint32 deathTime = sRandomPlayerbotMgr.GetValue(bot->GetGUID().GetCounter(), "death_time");
            if (!deathTime)
                continue;

            Group* group = bot->GetGroup();
            bool fullWipe = true;
            if (group)
            {
                for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                {
                    Player* mbr = ref->GetSource();
                    if (mbr && mbr != bot && mbr->IsAlive() && mbr->GetMapId() == bot->GetMapId())
                    {
                        fullWipe = false;
                        break;
                    }
                }
            }

            uint32 const elapsed = GameTime::GetGameTime().count() - deathTime;
            if (!fullWipe && elapsed < 360)
                continue;

            LOG_INFO("playerbots", "mod-bot-dungeon-queue: {} dead {} in {} ({}) — releasing spirit",
                     bot->GetName(), fullWipe ? "with all party dead" : "for 6min",
                     m->GetId(), fullWipe ? "full wipe" : "timeout");
            WorldPacket data(CMSG_REPOP_REQUEST);
            data << uint8(0);
            bot->GetSession()->HandleRepopRequestOpcode(data);
        }

        // Dungeon-complete sweep: teleport group home when all bosses are dead
        // Also logs bot positions for diagnostics.
        for (PlayerBotMap::const_iterator it = sRandomPlayerbotMgr.GetPlayerBotsBegin();
             it != sRandomPlayerbotMgr.GetPlayerBotsEnd(); ++it)
        {
            Player* bot = it->second;
            if (!bot || !bot->IsInWorld())
                continue;
            Map* m = bot->GetMap();
            if (!m || !m->IsDungeon())
                continue;

            // Check the encounter mask via the InstanceScript
            InstanceMap* im = m->ToInstanceMap();
            if (!im)
                continue;
            InstanceScript* script = im->GetInstanceScript();
            if (!script)
                continue;
            InstanceSave* save = sInstanceSaveMgr->GetInstanceSave(im->GetInstanceId());
            if (!save)
                continue;

            uint32 const mask = save->GetCompletedEncounterMask();
            if (!mask)
                continue; // no boss has been killed yet

            // Build the "all bits set" mask from actual DBC encounter indices.
            // Indices are NOT contiguous starting at 0 — they match DungeonEncounter.dbc.
            uint32 allBits = 0;
            for (uint32 i = 0; i < sDungeonEncounterStore.GetNumRows(); ++i)
                if (DungeonEncounterEntry const* enc = sDungeonEncounterStore.LookupEntry(i))
                    if (enc->mapId == m->GetId())
                        allBits |= (1u << enc->encounterIndex);

            if (!allBits)
                continue;

            if ((mask & allBits) != allBits)
                continue; // some bosses still alive

            LOG_INFO("playerbots", "mod-bot-dungeon-queue: dungeon {} complete for {} (mask {:08x} all {:08x}), teleporting group home",
                     m->GetId(), bot->GetName(), mask, allBits);
            if (Group* g = bot->GetGroup())
                TeleportGroupHome(g);
            else
                bot->TeleportTo(bot->m_homebindMapId, bot->m_homebindX, bot->m_homebindY, bot->m_homebindZ, 0.0f);
        }

        // Deferred follower teleports (m_pendingFollows from RunQueueCheck).
        // One tick after the tank, its map entry completed, so followers'
        // step 1 perm bind resolves to the shared instance.
        if (!m_pendingFollows.empty())
        {
            auto follows = std::move(m_pendingFollows);
            for (auto& pt : follows)
            {
                LOG_INFO("playerbots", "mod-bot-dungeon-queue: deferred teleport for {} followers to map {}",
                         pt.members.size(), pt.mapId);
                for (Player* m : pt.members)
                {
                    if (!m)
                    {
                        LOG_INFO("playerbots", "  null player, skipping");
                        continue;
                    }
                    if (!m->IsInWorld())
                    {
                        LOG_INFO("playerbots", "  {} NOT in world, skipping", m->GetName());
                        continue;
                    }
                    Difficulty diff = m->GetDifficulty(sMapStore.LookupEntry(pt.mapId)->IsRaid());
                    auto* selfBind = sInstanceSaveMgr->PlayerGetBoundInstance(m->GetGUID(), pt.mapId, diff);
                    LOG_INFO("playerbots", "  {} teleporting map {} diff {} selfBind={} perm={}",
                             m->GetName(), pt.mapId, diff,
                             selfBind ? std::to_string(selfBind->save->GetInstanceId()).c_str() : "NULL",
                             selfBind ? selfBind->perm : false);
                    m->TeleportTo(pt.mapId, pt.x, pt.y, pt.z, pt.o);
                }
            }
        }
    }

private:
    uint32 m_queueTimer = 0;
    uint32 m_cleanupTimer = 0;
    uint32 m_skullSyncTimer = 0;
    uint32 m_heartbeatTimer = 0;
    bool m_startupCleanupDone = false;
    bool m_delayedCleanupDone = false;

    struct PendingTeleport
    {
        std::vector<Player*> members;
        uint32 mapId;
        float x, y, z, o;
    };
    std::vector<PendingTeleport> m_pendingFollows;

    void RunQueueCheck()
    {
        if (!sPlayerbotAIConfig.randomBotJoinLfg)
            return;

        RandomPlayerbotMgr& mgr = sRandomPlayerbotMgr;

        uint32 totalOnline = 0;
        uint32 inDungeon = 0;
        LOG_INFO("playerbots", "mod-bot-dungeon-queue: queue tick");
        for (auto it = mgr.GetPlayerBotsBegin(); it != mgr.GetPlayerBotsEnd(); ++it)
        {
            Player* bot = it->second;
            if (!bot || !bot->IsInWorld())
                continue;
            ++totalOnline;
            if (bot->GetMap() && bot->GetMap()->IsDungeon())
                ++inDungeon;
        }

        uint32 maxPct = BotDungeonQueueConfig::MaxBotsPct();
        if (maxPct > 0 && totalOnline > 0)
        {
            uint32 maxDungeon = (totalOnline * maxPct) / 100;
            if (inDungeon >= maxDungeon)
                return;
        }

        std::map<TeamId, std::map<uint8, std::vector<Player*>>> botsByBracket;

        for (auto it = mgr.GetPlayerBotsBegin(); it != mgr.GetPlayerBotsEnd(); ++it)
        {
            Player* bot = it->second;
            if (!IsBotEligible(bot))
                continue;
            TeamId team = bot->GetTeamId();
            uint8 bracket = (bot->GetLevel() / 5) * 5;
            botsByBracket[team][bracket].push_back(bot);
        }

        for (auto& [team, bracketMap] : botsByBracket)
        {
            for (auto& [bracket, bots] : bracketMap)
            {
                if (bots.size() < 5)
                    continue;
                std::vector<Player*> tanks, healers, dps;
                for (Player* bot : bots)
                {
                    uint8 roles = GetBotRole(bot);
                    if (roles & PLAYER_ROLE_TANK)
                        tanks.push_back(bot);
                    else if (roles & PLAYER_ROLE_HEALER)
                        healers.push_back(bot);
                    else
                        dps.push_back(bot);
                }

                while (tanks.size() >= 1 && healers.size() >= 1 && dps.size() >= 3)
                {
                    Player* tank = tanks.back(); tanks.pop_back();
                    Player* healer = healers.back(); healers.pop_back();
                    std::vector<Player*> groupDps;
                    for (uint8 i = 0; i < 3; ++i)
                    {
                        groupDps.push_back(dps.back());
                        dps.pop_back();
                    }

                    Group* grp = new Group();
                    if (!grp->Create(tank))
                    {
                        delete grp;
                        continue;
                    }

                    auto Fail = [grp]() { grp->Disband(); };

                    if (!grp->AddMember(healer))
                    {
                        Fail();
                        continue;
                    }
                    for (Player* d : groupDps)
                    {
                        if (!grp->AddMember(d))
                        {
                            Fail();
                            break;
                        }
                    }
                    if (grp->GetMembersCount() < 5)
                        continue;

                    uint8 minLvl = tank->GetLevel(), maxLvl = tank->GetLevel();
                    for (Player* p : {healer, groupDps[0], groupDps[1], groupDps[2]})
                    {
                        if (p->GetLevel() < minLvl) minLvl = p->GetLevel();
                        if (p->GetLevel() > maxLvl) maxLvl = p->GetLevel();
                    }

                    DungeonPort const* selected = nullptr;
                    std::vector<DungeonPort const*> matches;
                    for (auto const& d : BotDungeonPorts)
                        if (minLvl >= d.minLvl && maxLvl <= d.maxLvl)
                            matches.push_back(&d);
                    if (!matches.empty())
                        selected = matches[urand(0, matches.size() - 1)];

                    if (!selected)
                    {
                        LOG_INFO("playerbots", "mod-bot-dungeon-queue: no dungeon for levels {}-{}, disbanding",
                                 minLvl, maxLvl);
                        Fail();
                        continue;
                    }

                    // Sync difficulty, unbind old binds, pre-bind all members
                    bool isRaid = sMapStore.LookupEntry(selected->mapId)->IsRaid();
                    Difficulty const d = tank->GetDifficulty(isRaid);
                    for (Player* m : {tank, healer, groupDps[0], groupDps[1], groupDps[2]})
                    {
                        sInstanceSaveMgr->PlayerUnbindInstance(m->GetGUID(), selected->mapId, d, true, m);
                        if (isRaid)
                            m->SetRaidDifficulty(d);
                        else
                            m->SetDungeonDifficulty(d);
                    }

                    uint32 instId = sMapMgr->GenerateInstanceId();
                    InstanceSave* save = sInstanceSaveMgr->AddInstanceSave(selected->mapId, instId, d, false);
                    if (save)
                        for (Player* m : {tank, healer, groupDps[0], groupDps[1], groupDps[2]})
                            sInstanceSaveMgr->PlayerBindToInstance(m->GetGUID(), save, true, m);

                    LOG_INFO("playerbots", "mod-bot-dungeon-queue: pre-bound all 5 to inst {} map {} diff {}",
                             instId, selected->mapId, d);
                    for (Player* m : {tank, healer, groupDps[0], groupDps[1], groupDps[2]})
                    {
                        auto* check = sInstanceSaveMgr->PlayerGetBoundInstance(m->GetGUID(), selected->mapId, d);
                        LOG_INFO("playerbots", "  bind {} -> save={} perm={}",
                                 m->GetName(),
                                 check ? std::to_string(check->save->GetInstanceId()).c_str() : "NULL",
                                 check ? check->perm : false);
                    }

                    // Prevent LFG from interfering — remove all members from its queue
                    for (Player* m : {tank, healer, groupDps[0], groupDps[1], groupDps[2]})
                    {
                        sRandomPlayerbotMgr.SetValue(m->GetGUID().GetCounter(), "pending_dungeon", 1);
                        sLFGMgr->LeaveLfg(m->GetGUID());
                    }

                    // Teleport the tank first (creates the instance map).
                    // Followers are deferred to the next tick so the tank's
                    // map entry (and perm→temp downgrade in AddPlayerToMap)
                    // completes before they resolve their own perm bind.
                    if (!tank->TeleportTo(selected->mapId, selected->x, selected->y, selected->z, selected->o))
                    {
                        LOG_INFO("playerbots", "mod-bot-dungeon-queue: teleport failed for tank {}, disbanding",
                                 tank->GetName());
                        Fail();
                        continue;
                    }

                    g_dungeonEntryTimeMs[tank->GetGUID()] = getMSTime();
                    g_dungeonEntryTimeMs[healer->GetGUID()] = getMSTime();
                    for (Player* d2 : groupDps)
                        g_dungeonEntryTimeMs[d2->GetGUID()] = getMSTime();

                    // Defer followers to the next world tick
                    m_pendingFollows.push_back({
                        {healer, groupDps[0], groupDps[1], groupDps[2]},
                        selected->mapId, selected->x, selected->y, selected->z, selected->o
                    });

                    LOG_INFO("playerbots", "mod-bot-dungeon-queue: teleported group '{}' to map {} (levels {}-{})",
                             tank->GetName(), selected->mapId, minLvl, maxLvl);

                    // Enable DC for all members; only the tank gets the tank strategy
                    for (Player* m : {tank, healer, groupDps[0], groupDps[1], groupDps[2]})
                        EnableDcOn(m, m == tank);
                }
            }
        }
    }

    void RunStuckCleanup()
    {
        RandomPlayerbotMgr& mgr = sRandomPlayerbotMgr;
        uint32 cleaned = 0;
        uint32 const nowMs = getMSTime();
        uint32 const graceMs = BotDungeonQueueConfig::StuckCleanupGracePeriod() * 1000;

        for (auto it = g_dungeonEntryTimeMs.begin(); it != g_dungeonEntryTimeMs.end(); )
        {
            if (getMSTimeDiff(it->second, nowMs) > graceMs * 2)
                it = g_dungeonEntryTimeMs.erase(it);
            else
                ++it;
        }

        for (auto it = mgr.GetPlayerBotsBegin(); it != mgr.GetPlayerBotsEnd(); ++it)
        {
            Player* bot = it->second;
            if (!bot || !bot->IsInWorld())
                continue;
            Map* map = bot->GetMap();
            if (!map || !map->IsDungeon())
                continue;

            auto entryIt = g_dungeonEntryTimeMs.find(bot->GetGUID());
            if (entryIt != g_dungeonEntryTimeMs.end() &&
                getMSTimeDiff(entryIt->second, nowMs) < graceMs)
                continue;

            Group* group = bot->GetGroup();
            if (group)
            {
                uint32 inThisDungeon = 0;
                bool hasLead = false;
                for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                {
                    Player* m = ref->GetSource();
                    if (!m || !m->IsInWorld() || m->GetMapId() != bot->GetMapId())
                        continue;
                    ++inThisDungeon;
                    // Check via spec tab (GetBotRole) — more reliable than
                    // IsDungeonClearLeader which can transiently return false
                    // due to leader cache timing. A tank by spec means the
                    // group has proper leadership even if DC hasn't resolved it.
                    {
                        uint8 role = GetBotRole(m);
                        if (role & PLAYER_ROLE_TANK)
                            hasLead = true;
                    }
                }
                if (inThisDungeon > 1 && hasLead)
                    continue;
            }

            LOG_INFO("playerbots", "mod-bot-dungeon-queue: stuck cleanup teleporting {} ({}) home from map {}",
                     bot->GetName(), group ? "solo survivor" : "ungrouped", bot->GetMapId());
            bot->TeleportTo(bot->m_homebindMapId, bot->m_homebindX, bot->m_homebindY, bot->m_homebindZ, 0.0f);
            // Disband group so evicted bots become eligible for re-queuing
            if (group)
                group->Disband();
            ++cleaned;
        }

        if (cleaned)
            LOG_INFO("playerbots", "mod-bot-dungeon-queue: stuck cleanup teleported {} bot(s) home", cleaned);
    }
};

class BotDungeonQueuePlayerScript : public PlayerScript
{
public:
    BotDungeonQueuePlayerScript() : PlayerScript("BotDungeonQueuePlayerScript") { }

    void OnPlayerMapChanged(Player* player) override
    {
        if (!BotDungeonQueueConfig::Enable())
            return;
        if (!sRandomPlayerbotMgr.IsRandomBot(player))
            return;
        Map* map = player->GetMap();
        if (!map || !map->IsDungeon())
        {
            g_snapInProgress.erase(player->GetGUID());
            return;
        }

        EnableDungeonClear(player);
        NavmeshSnapOnEntry(player);

        PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
        if (!ai)
            return;

        Group* group = player->GetGroup();
        if (!group)
            return;

        ObjectGuid leaderGuid = group->GetLeaderGUID();
        if (leaderGuid != player->GetGUID())
        {
            Player* leader = ObjectAccessor::FindPlayer(leaderGuid);
            if (leader && GET_PLAYERBOT_AI(leader))
            {
                ai->SetMaster(leader);
                if (!ai->HasStrategy("follow", BOT_STATE_NON_COMBAT))
                    ai->ChangeStrategy("+follow", BOT_STATE_NON_COMBAT);
            }
        }

        if (leaderGuid == player->GetGUID() && PlayerbotAI::IsTank(player))
        {
            Unit* target = player->GetSelectedUnit();
            if (target && target->IsInCombat() && target->GetTypeId() == TYPEID_UNIT)
                group->SetTargetIcon(0, player->GetGUID(), target->GetGUID());
        }
    }

    void OnPlayerKilledByCreature(Creature* /*killer*/, Player* player) override
    {
        if (!BotDungeonQueueConfig::Enable())
            return;
        if (!sRandomPlayerbotMgr.IsRandomBot(player))
            return;
        Map* map = player->GetMap();
        if (!map || !map->IsDungeon())
            return;
        sRandomPlayerbotMgr.SetValue(player->GetGUID().GetCounter(), "death_time",
                                     static_cast<uint32>(GameTime::GetGameTime().count()));
    }

    void OnPlayerReleasedGhost(Player* player) override
    {
        if (!BotDungeonQueueConfig::Enable())
            return;
        RandomPlayerbotMgr& mgr = sRandomPlayerbotMgr;
        if (!mgr.IsRandomBot(player))
            return;
        Map* map = player->GetMap();
        if (!map || !map->IsDungeon())
            return;
        Group* group = player->GetGroup();
        if (!BotDungeonQueueConfig::TeleportOutOnDeath() || !group)
            return;

        InstanceMap* im = map->ToInstanceMap();
        uint32 const instId = im ? im->GetInstanceId() : 0;
        if (instId)
        {
            uint32& wipes = g_wipeCount[instId];
            ++wipes;
            uint32 const maxWipes = BotDungeonQueueConfig::MaxWipesBeforeEvict();
            LOG_INFO("playerbots", "mod-bot-dungeon-queue: {} released ghost in {} (wipe {}/{})",
                     player->GetName(), map->GetId(), wipes, maxWipes);
            if (wipes < maxWipes)
            {
                // Let the group try again — don't evict yet
                return;
            }
            LOG_INFO("playerbots", "mod-bot-dungeon-queue: max wipes ({}) reached for inst {} — evicting group",
                     maxWipes, instId);
            g_wipeCount.erase(instId);
        }
        TeleportGroupHome(group);
    }

    void OnPlayerResurrect(Player* player, float /*restore_percent*/, bool& /*applySickness*/) override
    {
        if (!BotDungeonQueueConfig::Enable())
            return;
        if (!sRandomPlayerbotMgr.IsRandomBot(player))
            return;
        sRandomPlayerbotMgr.SetValue(player->GetGUID().GetCounter(), "death_time", 0);
    }

private:
    void EnableDungeonClear(Player* player)
    {
        PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
        if (!ai)
            return;

        if (!ai->HasStrategy("dungeon clear", BOT_STATE_NON_COMBAT))
            ai->ChangeStrategy("+dungeon clear", BOT_STATE_NON_COMBAT);
        if (!ai->HasStrategy("dungeon clear combat", BOT_STATE_COMBAT))
            ai->ChangeStrategy("+dungeon clear combat", BOT_STATE_COMBAT);

        DcRunState& rs = DcRun::Of(ai->GetAiObjectContext());
        // Trust the state from EnableDcOn (which uses the queue's GetBotRole
        // for tank detection). PlayerbotAI::IsTank can disagree (e.g., a
        // Feral Druid not in bear form), so don't re-validate here. Only
        // fresh-init if no state exists at all.
        if (!rs.enabled && !rs.paused)
        {
            rs = DcRunState{};
            rs.enabled = PlayerbotAI::IsTank(player);
        }

        sLFGMgr->LeaveLfg(player->GetGUID());
    }

    void NavmeshSnapOnEntry(Player* player)
    {
        auto& guard = g_snapInProgress[player->GetGUID()];
        if (!guard)
        {
            guard = true;
            NavmeshSnap::Result snap = NavmeshSnap::Snap(player, player->GetPositionX(),
                                                         player->GetPositionY(),
                                                         player->GetPositionZ());
            if (snap.ok && snap.distance > 0.5f)
                player->TeleportTo(player->GetMapId(), snap.x, snap.y, snap.z,
                                   player->GetOrientation());
        }
    }
};

void AddSC_bot_dungeon_queue_module()
{
    new BotDungeonQueueWorldScript();
    new BotDungeonQueuePlayerScript();
}
