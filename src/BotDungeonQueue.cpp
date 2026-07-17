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
#include "UnitScript.h"

#include <map>
#include <unordered_map>

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

// ── Combat log monitor ──────────────────────────────────────────────────
namespace CombatMonitor
{
    struct Stats
    {
        uint32 damageDealt{0};
        uint32 damageTaken{0};
        uint32 healingDone{0};
        uint32 healsCast{0};
    };

    static std::unordered_map<uint64, Stats> s_stats;
    static std::mutex s_mutex;

    void Reset()
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_stats.clear();
    }

    void AddDamage(uint64 guid, uint32 dmg)
    {
        if (!guid || !dmg) return;
        std::lock_guard<std::mutex> lock(s_mutex);
        s_stats[guid].damageDealt += dmg;
    }

    void AddDamageTaken(uint64 guid, uint32 dmg)
    {
        if (!guid || !dmg) return;
        std::lock_guard<std::mutex> lock(s_mutex);
        s_stats[guid].damageTaken += dmg;
    }

    void AddHealing(uint64 guid, uint32 heal)
    {
        if (!guid || !heal) return;
        std::lock_guard<std::mutex> lock(s_mutex);
        s_stats[guid].healingDone += heal;
        s_stats[guid].healsCast += 1;
    }

    Stats const* Get(uint64 guid)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        auto it = s_stats.find(guid);
        return it != s_stats.end() ? &it->second : nullptr;
    }

    class CombatUnitScript : public UnitScript
    {
    public:
        CombatUnitScript() : UnitScript("BotDungeonQueueCombatLog") {}

        void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
        {
            if (!damage || !attacker || !victim) return;
            if (attacker->GetGUID().IsPlayer())
                AddDamage(attacker->GetGUID().GetCounter(), damage);
            if (victim->GetGUID().IsPlayer())
                AddDamageTaken(victim->GetGUID().GetCounter(), damage);
        }

        void OnHeal(Unit* healer, Unit* /*reciever*/, uint32& gain) override
        {
            if (!gain || !healer) return;
            if (healer->GetGUID().IsPlayer())
                AddHealing(healer->GetGUID().GetCounter(), gain);
        }
    };

    // Registered later in OnStartup to avoid static init ordering issues
    static CombatUnitScript* s_script = nullptr;

    void Init()
    {
        if (!s_script)
            s_script = new CombatUnitScript();
    }
}
// ─────────────────────────────────────────────────────────────────────────

namespace
{
    std::unordered_map<ObjectGuid, uint32> g_dungeonEntryTimeMs;
    std::unordered_map<ObjectGuid, bool> g_snapInProgress;
    std::map<uint32, uint32> g_wipeCount;
    std::set<uint32> g_wipeCounted;  // instances where wipe was counted this death-cycle
    uint32 g_rrCounter = 0;  // round-robin counter for dungeon selection
    std::unordered_map<ObjectGuid, DungeonPort> g_pendingGhostTeleports;
    std::map<uint32, uint32> g_resurrectionHold;  // instanceId -> hold start (game time secs)
    // Tracks last-known position of each bot for stranding detection.
    // Key: bot GUID. Value: packed position (x*1000, y*1000, z*1000) from
    // the previous heartbeat. If a follower hasn't moved for one full
    // heartbeat cycle while the tank is far away, teleport them to the tank.
    std::unordered_map<ObjectGuid, Position> g_lastPosition;
}

// Apply permanent Water Breathing to a bot. The breath timer (getMaxTimer)
// returns DISABLED_MIRROR_TIMER when the player has any SPELL_AURA_MOD_WATER_BREATHING
// aura, making them immune to drowning regardless of underwater duration.
// We cast spell 131 (Water Breathing) and set its duration to -1 (permanent)
// so it never expires and never needs refreshing.
static void ApplyPermanentWaterBreathing(Player* player)
{
    if (!player || player->HasAuraType(SPELL_AURA_MOD_WATER_BREATHING))
        return;
    player->CastSpell(player, 131, true);
    if (Aura* aura = player->GetAura(131, player->GetGUID()))
    {
        aura->SetDuration(-1);
        aura->SetMaxDuration(-1);
    }
}

constexpr uint32 RES_HOLD_TIMEOUT_SECS = 30u;

std::vector<DungeonPort> const BotDungeonPorts = {
    // ─── Classic 1-60 dungeons ──────────────────────────────────────────
    {20, 30,  34,   54.23f,      0.28f,   -18.34f,   6.26f},    // Stormwind Stockade
    {15, 26,  36,  -16.4f,    -383.07f,    61.78f,   1.86f},    // Deadmines
    {16, 30,  33, -229.135f,  2109.18f,    76.8898f, 1.267f},   // Shadowfang Keep
    {20, 45,  43, -163.49f,    132.9f,    -73.66f,   5.83f},    // Wailing Caverns
    {20, 50,  47, 1943.0f,   1544.63f,     82.0f,    1.38f},    // Razorfen Kraul
    {19, 40,  48, -151.89f,    106.96f,   -39.87f,   4.53f},    // Blackfathom Deeps
    {15, 25, 389,    3.81f,     -14.82f,   -17.84f,   4.39f},    // Ragefire Chasm
    {24, 45,  90, -332.22f,     -2.28f,   -150.86f,   2.77f},    // Gnomeregan
    {26, 50, 189, 855.683f,   1321.5f,     18.6709f, 0.001747f}, // Scarlet Monastery
    {37, 55, 129, 2592.55f,   1107.5f,     51.29f,   4.74f},    // Razorfen Downs
    {28, 55,  70, -226.8f,     49.09f,    -46.03f,   1.39f},    // Uldaman
    {29, 60, 209, 1213.52f,   841.59f,      8.93f,   6.09f},    // Zul'Farrak
    {30, 55, 349,  752.91f,   -616.53f,   -33.11f,   1.37f},    // Maraudon
    {31, 55, 109, -319.24f,     99.9f,    -131.85f,   3.19f},    // Sunken Temple
    {40, 60, 230, 456.929f,    34.0923f,   -68.0896f, 4.71239f}, // Blackrock Depths
    {40, 60, 229,   78.5083f, -225.044f,    49.839f,  5.1f},    // Blackrock Spire
    {45, 60, 329, 3593.15f,  -3646.56f,    138.5f,   5.33f},    // Stratholme
    {40, 60, 289,  196.37f,    127.05f,    134.91f,  6.09f},    // Scholomance
    {40, 60, 429,   31.5609f,  159.45f,     -3.4777f, 0.01f},   // Dire Maul

    // ─── TBC 60-70 dungeons ────────────────────────────────────────────
    {60, 62, 543, -1355.24f,  1641.12f,     68.2491f, 0.6687f}, // Hellfire Ramparts
    {60, 62, 542,   -3.9967f,  14.6363f,   -44.8009f, 4.88748f},// The Blood Furnace
    {62, 64, 547,  120.101f, -131.957f,     -0.801547f, 1.47574f},// The Slave Pens
    {63, 65, 546,    9.71391f, -16.2008f,   -2.75334f, 5.57082f},// The Underbog
    {66, 68, 560, 2741.87f,   1315.25f,     14.0423f, 2.96016f}, // Old Hillsbrad Foothills
    {70, 70, 554,  -28.906f,    0.680314f,  -1.81282f, 0.0345509f},// The Mechanar
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
    // The stock playerbots LFG system queues bots independently — clear
    // any stale LFG state so our module can pick them up.
    if (sLFGMgr->GetState(bot->GetGUID()) != lfg::LFG_STATE_NONE)
        sLFGMgr->LeaveLfg(bot->GetGUID());
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
        sLFGMgr->LeaveLfg(m->GetGUID());
        m->TeleportTo(m->m_homebindMapId, m->m_homebindX, m->m_homebindY, m->m_homebindZ, 0.0f);
    }
    // Disband so evicted bots become eligible for re-queuing
    group->Disband();
}

static void SetLeaderPaused(Group* group, Player* reference, bool paused)
{
    if (!group)
        return;
    uint32 const refMap = reference ? reference->GetMapId() : uint32(-1);
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* mbr = ref->GetSource();
        if (!mbr || !mbr->IsAlive())
            continue;
        if (refMap != uint32(-1) && mbr->GetMapId() != refMap)
            continue;
        PlayerbotAI* ai = GET_PLAYERBOT_AI(mbr);
        if (!ai || !PlayerbotAI::IsTank(mbr))
            continue;
            DcRun::Of(ai->GetAiObjectContext()).paused = paused;
        return;
    }
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
        CombatMonitor::Init();
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

        // Stale-group cleanup: disband any groups on the world map (bots stuck
        // in groups after eviction from before the disband fix was deployed).
        if (!m_staleGroupCleanupDone)
        {
            m_staleGroupCleanupDone = true;
            uint32 disbanded = 0;
            for (PlayerBotMap::const_iterator it = sRandomPlayerbotMgr.GetPlayerBotsBegin();
                 it != sRandomPlayerbotMgr.GetPlayerBotsEnd(); ++it)
            {
                Player* bot = it->second;
                if (!bot || !bot->IsInWorld())
                    continue;
                Map* m = bot->GetMap();
                if (m && m->IsDungeon())
                    continue;
                Group* group = bot->GetGroup();
                if (!group)
                    continue;
                group->Disband();
                ++disbanded;
            }
            if (disbanded)
                LOG_INFO("playerbots", "mod-bot-dungeon-queue: stale-group cleanup disbanded {} group(s) on world map",
                         disbanded);
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

            // Keep bots immune to drowning while in dungeons
            ApplyPermanentWaterBreathing(bot);

            // Combat summary for this bot
            if (auto const* cs = CombatMonitor::Get(bot->GetGUID().GetCounter()))
            {
                if (cs->damageDealt || cs->damageTaken || cs->healingDone)
                    LOG_INFO("playerbots", "  dmg {}: dealt={} taken={} heal={} ({} casts)",
                             bot->GetName(), cs->damageDealt, cs->damageTaken,
                             cs->healingDone, cs->healsCast);

                // Warning: non-healer bot in combat dealing zero damage —
                // indicates the bot is group-flagged in combat or stuck
                // without a valid target to attack.
                if (bot->IsInCombat() && cs->damageDealt == 0 && cs->damageTaken == 0 && cs->healingDone == 0)
                {
                    bool const assistWanted = ai && DcLeaderSignal::IsLeaderFightAssistWanted(bot);
                    LOG_INFO("playerbots", "  !!! {} inCombat 0/0/0 assist={} en={} pause={}",
                             bot->GetName(), assistWanted ? 1 : 0,
                             dcEnabled ? 1 : 0, dcPaused ? 1 : 0);
                }
            }

            // Diagnose: is this bot's assist-wanted gate passing?
            if (ai)
            {
                bool const res = DcLeaderSignal::IsLeaderFightAssistWanted(bot);
                LOG_INFO("playerbots", "  ast {}: res={} inCombat={} en={} pause={} lead={}",
                         bot->GetName(), res ? 1 : 0,
                         bot->IsInCombat() ? 1 : 0,
                         dcEnabled ? 1 : 0, dcPaused ? 1 : 0, isLeader ? 1 : 0);
            }

            // Stranding detection: if a follower hasn't moved since the last
            // heartbeat while the tank is far away, teleport them to the tank
            // (typical cause: navmesh gap between dungeon levels).
            if (!isLeader && ai && !bot->IsInCombat())
            {
                auto prev = g_lastPosition.find(bot->GetGUID());
                if (prev != g_lastPosition.end())
                {
                    float const dx = bot->GetPositionX() - prev->second.GetPositionX();
                    float const dy = bot->GetPositionY() - prev->second.GetPositionY();
                    float const dz = bot->GetPositionZ() - prev->second.GetPositionZ();
                    bool const moved = (dx*dx + dy*dy + dz*dz) > 1.0f;
                    if (!moved)
                    {
                        Player* tank = DcLeaderSignal::FindLeaderTank(bot);
                        if (tank && tank->IsInWorld() && tank->GetMapId() == bot->GetMapId())
                        {
                            float const dist = bot->GetExactDist2d(tank);
                            if (dist > 40.0f)
                            {
                                LOG_INFO("playerbots", "mod-bot-dungeon-queue: {} stranded {:.0f}yd from tank {} at ({:.1f}, {:.1f}, {:.1f}) — teleporting to tank",
                                         bot->GetName(), dist, tank->GetName(),
                                         tank->GetPositionX(), tank->GetPositionY(),
                                         tank->GetPositionZ());
                                bot->TeleportTo(bot->GetMapId(),
                                                tank->GetPositionX(),
                                                tank->GetPositionY(),
                                                tank->GetPositionZ(),
                                                tank->GetOrientation());
                            }
                        }
                    }
                }
            }
            // Update last known position
            g_lastPosition[bot->GetGUID()] = Position(bot->GetPositionX(),
                                                       bot->GetPositionY(),
                                                       bot->GetPositionZ());
        }

        // Reset combat stats for next 60s window
        CombatMonitor::Reset();

        // DC watchdog: re-enable dungeon clear for EVERY bot in a dungeon
        // whose DC was disabled (by death, restart, or map-change race).
        // Always enable DC for all members so the non-combat multiplier
        // suppresses wander/proactive-engage and keeps the group together.
        // Tank strategy is added only for tank-role bots.
        for (PlayerBotMap::const_iterator it = sRandomPlayerbotMgr.GetPlayerBotsBegin();
             it != sRandomPlayerbotMgr.GetPlayerBotsEnd(); ++it)
        {
            Player* bot = it->second;
            if (!bot || !bot->IsInWorld() || !bot->IsAlive())
                continue;
            Map* m = bot->GetMap();
            if (!m || !m->IsDungeon())
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
                if (PlayerbotAI::IsTank(bot) &&
                    !ai->HasStrategy("tank", BOT_STATE_COMBAT))
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

            // If no one alive in the group can resurrect, release immediately
            // instead of waiting 6 minutes. Res classes: Priest, Paladin, Shaman, Druid.
            bool canResurrect = false;
            if (!fullWipe && group)
            {
                for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                {
                    Player* mbr = ref->GetSource();
                    if (!mbr || mbr == bot || !mbr->IsAlive() || mbr->GetMapId() != bot->GetMapId())
                        continue;
                    switch (mbr->getClass())
                    {
                        case CLASS_PRIEST:
                        case CLASS_PALADIN:
                        case CLASS_SHAMAN:
                        case CLASS_DRUID:
                            canResurrect = true;
                            break;
                    }
                    if (canResurrect)
                        break;
                }
            }

            if (!fullWipe && group)
            {
                InstanceMap* im_map = m->ToInstanceMap();
                uint32 const hold_inst = im_map ? im_map->GetInstanceId() : 0;

                if (!canResurrect)
                {
                    // No res capability — release immediately (clear any stale hold)
                    if (hold_inst && g_resurrectionHold.erase(hold_inst))
                        SetLeaderPaused(group, bot, false);
                    LOG_INFO("playerbots", "mod-bot-dungeon-queue: {} dead in {} — no res available, releasing spirit",
                             bot->GetName(), m->GetId());
                }
                else
                {
                    if (hold_inst && !g_resurrectionHold.count(hold_inst))
                    {
                        // Start resurrection hold — pause tank so healer can
                        // cast without the party moving out of range.
                        g_resurrectionHold[hold_inst] = GameTime::GetGameTime().count();
                        SetLeaderPaused(group, bot, true);
                        LOG_INFO("playerbots", "mod-bot-dungeon-queue: {} dead, res available — hold started for instance {}",
                                 bot->GetName(), hold_inst);
                    }

                    if (hold_inst && g_resurrectionHold.count(hold_inst))
                    {
                        uint32 const holdStart = g_resurrectionHold[hold_inst];
                        if (GameTime::GetGameTime().count() - holdStart < RES_HOLD_TIMEOUT_SECS)
                            continue;

                        // Timed out — release ghost and resume
                        LOG_INFO("playerbots", "mod-bot-dungeon-queue: {} — res hold timed out, releasing spirit",
                                 bot->GetName());
                        g_resurrectionHold.erase(hold_inst);
                        SetLeaderPaused(group, bot, false);
                    }
                }
            }

            // On full wipe, clear any stale hold (everyone dead, no one to unpause)
            if (fullWipe)
            {
                InstanceMap* im_map = m->ToInstanceMap();
                uint32 const hold_inst = im_map ? im_map->GetInstanceId() : 0;
                if (hold_inst && g_resurrectionHold.count(hold_inst))
                    g_resurrectionHold.erase(hold_inst);
            }

            LOG_INFO("playerbots", "mod-bot-dungeon-queue: {} dead {} in {} ({}) — releasing spirit",
                     bot->GetName(), fullWipe ? "with all party dead" : "solo",
                     m->GetId(), fullWipe ? "full wipe" : "solo death");
            // Set pending_teleport flag to prevent AcceptResurrectAction from
            // accepting a spirit healer rez before the ghost release completes.
            // Without this flag, the bot may revive at spirit healer, which means
            // OnPlayerReleasedGhost never fires and the wipe counter is skipped.
            sRandomPlayerbotMgr.SetValue(bot->GetGUID().GetCounter(), "pending_teleport", 1);
            WorldPacket data(CMSG_REPOP_REQUEST);
            data << uint8(0);
            bot->GetSession()->HandleRepopRequestOpcode(data);
        }

        // Orphaned-survivor sweep: when the tank is dead/missing and no res
        // is available, alive party members can't continue. Teleport them
        // home so they can re-queue instead of roaming the dungeon alone
        // until the stuck cleanup evicts them 2 minutes later.
        for (PlayerBotMap::const_iterator it = sRandomPlayerbotMgr.GetPlayerBotsBegin();
             it != sRandomPlayerbotMgr.GetPlayerBotsEnd(); ++it)
        {
            Player* bot = it->second;
            if (!bot || !bot->IsInWorld() || !bot->IsAlive())
                continue;
            Map* m = bot->GetMap();
            if (!m || !m->IsDungeon())
                continue;
            Group* g = bot->GetGroup();
            if (!g)
            {
                // Ungrouped and alone in a dungeon — teleport home immediately
                // instead of waiting for the stuck cleanup (30s delay).
                LOG_INFO("playerbots", "mod-bot-dungeon-queue: orphaned survivor {} ungrouped in map {} — teleporting home",
                         bot->GetName(), m->GetId());
                bot->TeleportTo(bot->m_homebindMapId, bot->m_homebindX,
                                bot->m_homebindY, bot->m_homebindZ, 0.0f);
                continue;
            }

            // Check if this group has a living tank on the same map
            bool hasTank = false;
            for (GroupReference* ref = g->GetFirstMember(); ref; ref = ref->next())
            {
                Player* mbr = ref->GetSource();
                if (!mbr || !mbr->IsAlive() || mbr->GetMapId() != bot->GetMapId())
                    continue;
                PlayerbotAI* ai = GET_PLAYERBOT_AI(mbr);
                if (ai && PlayerbotAI::IsTank(mbr))
                {
                    hasTank = true;
                    break;
                }
            }
            if (hasTank)
            {
                // Tank is alive but check if the group is too small to continue.
                // If Shiloh (tank) returned solo but the rest of the party didn't,
                // the group can't function — evict rather than leaving her alone.
                uint32 inDungeon = 0;
                for (GroupReference* ref = g->GetFirstMember(); ref; ref = ref->next())
                {
                    Player* mbr = ref->GetSource();
                    if (mbr && mbr->IsInWorld() && mbr->GetMapId() == bot->GetMapId())
                        ++inDungeon;
                }
                if (inDungeon >= 3)
                    continue;
                LOG_INFO("playerbots", "mod-bot-dungeon-queue: tank {} alive but only {} members in map {} — group broken, evicting",
                         bot->GetName(), inDungeon, m->GetId());
                // Fall through to wipe counting + eviction below
                goto do_evict;
            }
            else
            {

            // No tank — check if anyone can res the dead tank
            bool canResurrect = false;
            for (GroupReference* ref = g->GetFirstMember(); ref; ref = ref->next())
            {
                Player* mbr = ref->GetSource();
                if (!mbr || !mbr->IsAlive() || mbr->GetMapId() != bot->GetMapId())
                    continue;
                switch (mbr->getClass())
                {
                    case CLASS_PRIEST:
                    case CLASS_PALADIN:
                    case CLASS_SHAMAN:
                    case CLASS_DRUID:
                        canResurrect = true;
                        break;
                }
                if (canResurrect)
                    break;
            }
            if (canResurrect)
                continue;

            } // end else (no tank)

        do_evict:
            // Tank dead/missing, no res available — evict the whole group
            // and count this as a wipe (the group effectively wiped even if
            // some members are still alive — they can't continue without a tank).
            uint32 const maxWipes = BotDungeonQueueConfig::MaxWipesBeforeEvict();
            if (InstanceMap* im_map = m->ToInstanceMap())
            {
                uint32 const instId = im_map->GetInstanceId();
                bool const alreadyCounted = g_wipeCounted.count(instId) > 0;
                if (!alreadyCounted)
                {
                    ++g_wipeCount[instId];
                    g_wipeCounted.insert(instId);
                }
                uint32 wipes = g_wipeCount[instId];
                LOG_INFO("playerbots", "mod-bot-dungeon-queue: orphaned survivor {} in map {} (no tank, no res) — wipe {}/{}",
                         bot->GetName(), m->GetId(), wipes, maxWipes);

                if (wipes >= maxWipes)
                {
                    LOG_INFO("playerbots", "mod-bot-dungeon-queue: max wipes ({}) reached for inst {} — evicting group",
                             maxWipes, instId);
                    g_wipeCount.erase(instId);
                    g_wipeCounted.erase(instId);
                }
            }
            TeleportGroupHome(g);
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

            // Extended completion log
            MapEntry const* mapEntry = sMapStore.LookupEntry(m->GetId());
            char const* mapName = mapEntry ? mapEntry->name[0] : "unknown";
            uint32 const elapsedSec = getMSTimeDiff(g_dungeonEntryTimeMs[bot->GetGUID()], getMSTime()) / 1000;
            uint32 const wipes = g_wipeCount[im->GetInstanceId()];
            LOG_INFO("playerbots", "mod-bot-dungeon-queue: === DUNGEON COMPLETE: {} (map {}) ===",
                     mapName, m->GetId());
            LOG_INFO("playerbots", "mod-bot-dungeon-queue:   completed by {} | time={}s | wipes={}",
                     bot->GetName(), elapsedSec, wipes);
            LOG_INFO("playerbots", "mod-bot-dungeon-queue:   group members:");
            if (Group* g = bot->GetGroup())
            {
                for (GroupReference* ref = g->GetFirstMember(); ref; ref = ref->next())
                {
                    Player* mbr = ref->GetSource();
                    if (!mbr)
                        continue;
                    PlayerbotAI* ai = GET_PLAYERBOT_AI(mbr);
                    char const* role = "?";
                    if (ai && PlayerbotAI::IsTank(mbr)) role = "tank";
                    else if (ai && PlayerbotAI::IsHeal(mbr)) role = "heal";
                    else if (ai) role = "dps";
                    static char const* clsNames[12] = {"","war","pal","hun","rog","pri","dk","sha","mag","warl","monk","dru"};
                    uint8 cls = mbr->getClass();
                    char const* clsName = (cls > 0 && cls < 12) ? clsNames[cls] : "?";
                    LOG_INFO("playerbots", "mod-bot-dungeon-queue:     {} ({}) - {} lvl{}", mbr->GetName(),
                             role, clsName, mbr->GetLevel());
                }
            }
            LOG_INFO("playerbots", "mod-bot-dungeon-queue: ====================================");

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

        // Step 2 ghost teleports: one tick after the homebind far teleport
        // (step 1 in OnPlayerReleasedGhost), the ghost's map transition has
        // completed, so a teleport back to the dungeon entrance is a far
        // teleport that correctly enters the instance layer.
        if (!g_pendingGhostTeleports.empty())
        {
            for (auto it = g_pendingGhostTeleports.begin(); it != g_pendingGhostTeleports.end(); )
            {
                ObjectGuid const& guid = it->first;
                DungeonPort const& dp = it->second;
                Player* p = ObjectAccessor::FindPlayer(guid);
                if (!p || !p->IsInWorld())
                {
                    // Ghost hasn't finished loading into the homebind map yet
                    // — retry next tick.
                    ++it;
                    continue;
                }
                LOG_INFO("playerbots", "mod-bot-dungeon-queue: ghost teleport step 2: teleporting {} from {} to map {} entrance",
                         p->GetName(), p->GetMapId(), dp.mapId);
                p->TeleportTo(dp.mapId, dp.x, dp.y, dp.z, dp.o);
                sRandomPlayerbotMgr.SetValue(p->GetGUID().GetCounter(), "pending_teleport", 0);
                it = g_pendingGhostTeleports.erase(it);
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
    bool m_staleGroupCleanupDone = false;

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
                    {
                        // Global round-robin: start at g_rrCounter and find the
                        // first eligible dungeon, wrapping around the port list.
                        uint32 const portCount = static_cast<uint32>(BotDungeonPorts.size());
                        for (uint32 i = 0; i < portCount; ++i)
                        {
                            uint32 idx = (g_rrCounter + i) % portCount;
                            if (minLvl >= BotDungeonPorts[idx].minLvl && maxLvl <= BotDungeonPorts[idx].maxLvl)
                            {
                                selected = &BotDungeonPorts[idx];
                                g_rrCounter = (idx + 1) % portCount;
                                break;
                            }
                        }
                        // If none matched (shouldn't happen if matches is non-empty),
                        // fall back to the first match.
                        if (!selected)
                            selected = matches[0];
                    }

                    if (!selected)
                    {
                        LOG_INFO("playerbots", "mod-bot-dungeon-queue: no dungeon for levels {}-{}, disbanding",
                                 minLvl, maxLvl);
                        Fail();
                        continue;
                    }

                    LOG_INFO("playerbots", "mod-bot-dungeon-queue: group {} (levels {}-{}) picks map {} ({} matches)",
                             tank->GetName(), minLvl, maxLvl, selected->mapId, matches.size());

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
                {
                    // Appears healthy (tank + followers).  But if the group
                    // has been in the dungeon for > 2× grace with zero boss
                    // kills, it's truly stuck (e.g. BFD no navmesh).  Evict
                    // so accounts can re-queue for a working dungeon.
                    bool const idleStuck =
                        getMSTimeDiff(entryIt->second, nowMs) > graceMs * 2 &&
                        [&]() -> bool {
                            InstanceMap* im = map->ToInstanceMap();
                            if (!im) return false;
                            InstanceScript* script = im->GetInstanceScript();
                            if (!script) return false;
                            InstanceSave* save = sInstanceSaveMgr->GetInstanceSave(im->GetInstanceId());
                            return !save || save->GetCompletedEncounterMask() == 0;
                        }();
                    if (idleStuck)
                        LOG_INFO("playerbots", "mod-bot-dungeon-queue: stuck cleanup evicting {} from map {} (idle {:.0f}s, 0 bosses killed)",
                                 bot->GetName(), bot->GetMapId(),
                                 getMSTimeDiff(entryIt->second, nowMs) / 1000.0f);
                    if (!idleStuck)
                        continue;
                }
            }

            LOG_INFO("playerbots", "mod-bot-dungeon-queue: stuck cleanup teleporting {} ({}) home from map {}",
                     bot->GetName(), group ? "solo survivor" : "ungrouped", bot->GetMapId());
            sLFGMgr->LeaveLfg(bot->GetGUID());
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
        InstanceMap* im = map->ToInstanceMap();
        uint32 const instId = im ? im->GetInstanceId() : 0;
        uint32 const mapId = map->GetId();

        sRandomPlayerbotMgr.SetValue(player->GetGUID().GetCounter(), "death_time",
                                     static_cast<uint32>(GameTime::GetGameTime().count()));
        // Stash the map+instance so OnPlayerReleasedGhost can still find the
        // dungeon even when the spirit healer graveyard lives on the continent
        // map (e.g. BFD) — the ghost appears on Kalimdor, not inside the
        // instance, so player->GetMap().IsDungeon() is false there.
        sRandomPlayerbotMgr.SetValue(player->GetGUID().GetCounter(), "death_map_id", mapId);
        sRandomPlayerbotMgr.SetValue(player->GetGUID().GetCounter(), "death_instance_id", instId);

        // Set pending_teleport flag to block spirit healer rez. The bot should
        // corpse-run instead of rezzing at the spirit healer. This flag is
        // cleared when a player-cast resurrection happens (OnPlayerResurrect).
        sRandomPlayerbotMgr.SetValue(player->GetGUID().GetCounter(), "pending_teleport", 1);

        // Count tank deaths as wipes. The tank dying is the most reliable
        // signal that something went wrong — simpler and more robust than
        // trying to detect "full party wipe" (which fails on dungeons with
        // continental graveyards like BFD where OnPlayerReleasedGhost never
        // runs the normal path).
        if (instId)
        {
            PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
            if (ai && PlayerbotAI::IsTank(player))
            {
                bool const alreadyCounted = g_wipeCounted.count(instId) > 0;
                if (!alreadyCounted)
                {
                    ++g_wipeCount[instId];
                    g_wipeCounted.insert(instId);
                }
                uint32 const maxWipes = BotDungeonQueueConfig::MaxWipesBeforeEvict();
                uint32 wipes = g_wipeCount[instId];
                LOG_INFO("playerbots", "mod-bot-dungeon-queue: tank {} died in map {} — wipe {}/{}",
                         player->GetName(), mapId, wipes, maxWipes);

                // If the group used all its retries on this death, evict them
                // immediately instead of waiting for a ghost release that may
                // never reach OnPlayerReleasedGhost (BFD continental graveyard).
                if (wipes >= maxWipes)
                {
                    LOG_INFO("playerbots", "mod-bot-dungeon-queue: max wipes ({}) reached on tank death for inst {} — evicting group",
                             maxWipes, instId);
                    g_wipeCount.erase(instId);
                    g_wipeCounted.erase(instId);
                    if (Group* g = player->GetGroup())
                        TeleportGroupHome(g);
                }
            }
        }
    }

    void OnPlayerReleasedGhost(Player* player) override
    {
        if (!BotDungeonQueueConfig::Enable())
            return;
        RandomPlayerbotMgr& mgr = sRandomPlayerbotMgr;
        if (!mgr.IsRandomBot(player))
            return;

        // For most dungeons the ghost appears inside the instance (interior
        // graveyard). But some — notably BFD (map 48) and any dungeon whose
        // spirit healer sits on the continent — place the ghost on the world
        // map. Use the death_map_id stashed at death time as a fallback so
        // the ghost-handler still fires for those dungeons.
        Map* map = player->GetMap();
        uint32 deathMapId = 0;
        uint32 deathInstanceId = 0;
        if (!map || !map->IsDungeon())
        {
            deathMapId = mgr.GetValue(player->GetGUID().GetCounter(), "death_map_id");
            deathInstanceId = mgr.GetValue(player->GetGUID().GetCounter(), "death_instance_id");
            if (!deathMapId || !deathInstanceId)
                return;  // no stored death info — nothing we can do
            // Build a fake map-like key for groupmate scanning below:
            // the bot is a ghost on the continent but the rest of the party
            // might still be alive inside the dungeon (partial wipe).
        }

        Group* group = player->GetGroup();
        if (!BotDungeonQueueConfig::TeleportOutOnDeath() || !group)
            return;

        // Resolve the dungeon map/instance ID from either the ghost's current
        // map (interior graveyard) or the stashed death values (continental
        // graveyard like BFD).
        uint32 const mapId = (map && map->IsDungeon()) ? map->GetId() : deathMapId;
        uint32 const instId = [&]() -> uint32
        {
            if (map && map->IsDungeon())
                if (InstanceMap* im = map->ToInstanceMap())
                    return im->GetInstanceId();
            return deathInstanceId;
        }();
        if (!instId)
            return;

        // Determine if this is a genuine full-party wipe. Only count
        // full wipes against the retry limit — solo deaths (one bot
        // dies while the rest of the party is alive) should teleport
        // back and rejoin without consuming a retry.
        bool fullPartyWipe = true;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* mbr = ref->GetSource();
            if (mbr && mbr != player && mbr->IsAlive() &&
                mbr->GetMapId() == mapId)
            {
                fullPartyWipe = false;
                break;
            }
        }

        uint32 const maxWipes = BotDungeonQueueConfig::MaxWipesBeforeEvict();
        bool evict = false;

        if (fullPartyWipe)
        {
            // Only count one wipe per death-cycle, not per bot release.
            // In a full party wipe, 5 bots release nearly simultaneously
            // — without this guard a single wipe consumes all 4 retries.
            bool const alreadyCounted = g_wipeCounted.count(instId) > 0;
            if (!alreadyCounted)
            {
                ++g_wipeCount[instId];
                g_wipeCounted.insert(instId);
            }

            uint32 wipes = g_wipeCount[instId];
            if (wipes >= maxWipes)
                evict = true;
        }

        if (!evict)
        {
            // Step 1: force a far (inter-map) teleport by sending the ghost
            // to homebind.  A same-map near teleport would stay on the
            // overworld layer, not inside the instance, and the stock
            // ReviveFromCorpseAction can't path through the instance
            // portal — the ghost eventually gives up and spirit-healer-
            // resurrects outside the dungeon with res sickness.
            player->TeleportTo(player->m_homebindMapId,
                               player->m_homebindX,
                               player->m_homebindY,
                               player->m_homebindZ, 0.0f);
            LOG_INFO("playerbots", "mod-bot-dungeon-queue: {} released ghost in map {} ({}) — step 1: teleported home, queueing step 2",
                     player->GetName(), mapId,
                     fullPartyWipe
                         ? std::to_string(g_wipeCount[instId]) + "/" + std::to_string(maxWipes) + " wipes"
                         : "solo death, no wipe counted");
            // Step 2 will fire on the next OnUpdate tick and teleport the
            // ghost from homebind into the instance via far teleport.
            for (auto const& dp : BotDungeonPorts)
            {
                if (dp.mapId == mapId)
                {
                    g_pendingGhostTeleports[player->GetGUID()] = dp;
                    sRandomPlayerbotMgr.SetValue(player->GetGUID().GetCounter(), "pending_teleport", 1);
                    break;
                }
            }
            return;
        }

        LOG_INFO("playerbots", "mod-bot-dungeon-queue: max wipes ({}) reached for inst {} — evicting group",
                 maxWipes, instId);
        g_wipeCount.erase(instId);
        g_wipeCounted.erase(instId);
        TeleportGroupHome(group);
    }

    void OnPlayerResurrect(Player* player, float /*restore_percent*/, bool& applySickness) override
    {
        if (!BotDungeonQueueConfig::Enable())
            return;
        if (!sRandomPlayerbotMgr.IsRandomBot(player))
            return;
        sRandomPlayerbotMgr.SetValue(player->GetGUID().GetCounter(), "death_time", 0);
        sRandomPlayerbotMgr.SetValue(player->GetGUID().GetCounter(), "death_map_id", 0);
        sRandomPlayerbotMgr.SetValue(player->GetGUID().GetCounter(), "death_instance_id", 0);
        // Clear pending_teleport flag — the bot has been resurrected (either
        // by spirit healer or player-cast). If it was a spirit healer rez, the
        // respawn-back logic below will teleport the bot to the dungeon
        // entrance if the group is still inside.
        sRandomPlayerbotMgr.SetValue(player->GetGUID().GetCounter(), "pending_teleport", 0);

        Map* map = player->GetMap();
        if (map && map->IsDungeon())
        {
            // ... existing dungeon rez handling ...
            if (InstanceMap* im = map->ToInstanceMap())
            {
                uint32 const instId = im->GetInstanceId();
                g_wipeCounted.erase(instId);

                if (g_resurrectionHold.erase(instId))
                {
                    if (Group* g = player->GetGroup())
                        SetLeaderPaused(g, player, false);
                    LOG_INFO("playerbots", "mod-bot-dungeon-queue: res hold cleared for instance {} — {} resurrected, unpausing",
                             instId, player->GetName());
                }
            }

            EnableDungeonClear(player);
            if (PlayerbotAI* ai = GET_PLAYERBOT_AI(player))
            {
                if (PlayerbotAI::IsTank(player) ||
                    (GetBotRole(player) & PLAYER_ROLE_TANK))
                {
                    if (!ai->HasStrategy("tank", BOT_STATE_COMBAT) &&
                        !ai->HasStrategy("bear", BOT_STATE_COMBAT))
                        ai->ChangeStrategy("+tank", BOT_STATE_COMBAT);
                }
            }
            return;
        }

        // Spirit-healer rez outside a dungeon: if the bot's groupmates are
        // still inside, teleport the bot back to the dungeon entrance so it
        // can corpse-run and rejoin instead of wandering off alone.
        // Also: count this as a wipe since the bot spirit-rezzed instead of
        // corpse-running (the pending_teleport flag should have prevented
        // this, but it's a fallback for race conditions).
        if (!applySickness)
            return;

        Group* group = player->GetGroup();
        if (!group)
            return;

        // Count a wipe for this instance (spirit rez = failed recovery)
        Map* playerMap = player->GetMap();
        bool countedWipe = false;
        if (InstanceMap* im_map = playerMap ? playerMap->ToInstanceMap() : nullptr)
        {
            uint32 const instId = im_map->GetInstanceId();
            bool const alreadyCounted = g_wipeCounted.count(instId) > 0;
            if (!alreadyCounted)
            {
                ++g_wipeCount[instId];
                g_wipeCounted.insert(instId);
            }
            countedWipe = true;
        }

        // Find a groupmate who is still inside a dungeon
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* mbr = ref->GetSource();
            if (!mbr || mbr == player || !mbr->IsInWorld())
                continue;
            Map* mbrMap = mbr->GetMap();
            if (!mbrMap || !mbrMap->IsDungeon())
                continue;

            // Groupmate is still in a dungeon — teleport this bot back
            for (auto const& dp : BotDungeonPorts)
            {
                if (dp.mapId == mbrMap->GetId())
                {
                    player->TeleportTo(dp.mapId, dp.x, dp.y, dp.z, dp.o);
                    EnableDungeonClear(player);
                    LOG_INFO("playerbots", "mod-bot-dungeon-queue: {} spirit-rezzed outside, group still in map {} — teleporting back",
                             player->GetName(), dp.mapId);
                    return;
                }
            }
        }

        // No groupmate found in a dungeon (the whole group wiped outside).
        // Re-enable DC anyway so the bot is ready when it re-enters.
        EnableDungeonClear(player);
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
        // Always enable DC on dungeon entry for ALL party members.
        // Followers with enabled=true let the non-combat multiplier suppress
        // wander/proactive-engage, keeping the group together.
        // The advance trigger is gated by IsDungeonClearLeader, so followers
        // never try to lead — and this pattern matches EnableDcOn.
        if (!rs.enabled && !rs.paused)
        {
            rs = DcRunState{};
            rs.enabled = true;
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
