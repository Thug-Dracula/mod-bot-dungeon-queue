# mod-bot-dungeon-queue

> **⚠️ UNFINISHED — NOT PRODUCTION-READY**  
> This module is actively developed, very buggy, and should not be used in a production environment. Instance sharing is unreliable, DC leadership is inconsistent, and many dungeon interactions are unwired.

Autonomous dungeon queuing and clearing for bot groups. Forms 5-player parties (tank + healer + 3 DPS) by level bracket, teleports them into a shared dungeon instance, and enables [mod-dungeon-clear](https://github.com/Thug-Dracula/mod-dungeon-clear) on every member so the group navigates, engages bosses, and clears the dungeon without player input.

Depends on [mod-playerbots](https://github.com/azerothcore/mod-playerbots) and [mod-dungeon-clear](https://github.com/Thug-Dracula/mod-dungeon-clear).

## How it works

1. **Queue** — every `QueueInterval` (default 30s), eligible bots are grouped by faction + level bracket (5-level segments). Groups of 5 (1 tank / 1 healer / 3 DPS) are formed.
2. **Pre-bind** — a shared `InstanceSave` is pre-allocated and all 5 members are permanently bound to it, so `PlayerGetDestinationInstanceId` resolves to the same instance for every member.
3. **Teleport** — all members are teleported via `Player::TeleportTo` to the entrance coordinates of a matching dungeon. The stagger (tank first, then followers) ensures the tank's map transfer initiates first.
4. **Dungeon Clear** — `mod-dungeon-clear` is enabled on all members. The tank navigates the dungeon, DPS assist on the tank's target (skull sync every 2s), and the group progresses through bosses.
5. **Death handling** — on individual death, waits 6 minutes for a resurrection. On full wipe (all party members dead), releases immediately so the group can regroup and retry.
6. **Stuck cleanup** — every `StuckCleanupInterval`, solo bots still inside instances are teleported home. A `StuckCleanupGracePeriod` exempts freshly-teleported groups.

## Supported dungeons

| Map | Dungeon | Navmesh | Entrance coords |
|---|---|---|---|
| 36 | Deadmines | Full | `(-16.4, -383.07, 61.78)` |
| 33 | Shadowfang Keep | Full | `(-229.14, 2109.18, 76.89)` |
| 389 | Ragefire Chasm | Removed* | `(3.81, -14.82, -17.84)` |
| 48 | Blackfathom Deeps | Full | `(-151.89, 106.96, -39.87)` |
| 34 | Stormwind Stockade | Full | `(54.23, 0.28, -18.34)` |
| 43 | Wailing Caverns | Removed* | `(-146.49, -265.56, 17.22)` |
| 47 | Razorfen Kraul | Removed* | `(1043.64, -474.57, -44.04)` |
| 129 | Razorfen Downs | Full | `(3094.89, 3769.49, 42.88)` |
| 70 | Uldaman | Full | `(5.25, -2.64, 1.93)` |
| 90 | Zul'Farak | Full | `(-7599.52, 806.29, 1569.7)` |
| 349 | Maraudon | Full | `(-173.53, 30.69, -6.48)` |
| 109 | Sunken Temple | Full | `(4610.12, -896.64, 30.45)` |
| 289 | Blackrock Depths | Full | `(-471.51, -266.27, 169.59)` |
| 230 | Blackrock Spire | Full | `(149.63, -41.87, 36.74)` |
| 201 | Stratholme | Full | `(-5430.22, -2946.53, 92.08)` |
| 329 | Hellfire Ramparts | Full | `(2758.35, -404.31, 115.14)` |
| 269 | Caverns of Time | Full | `(-11831.0, -4819.0, 0.55)` |

\* DungeonSpawnGraph fallback — partial navmesh tiles were removed; bots path through creature spawn points instead.

## Configuration

Copy `conf/mod_bot_dungeon_queue.conf.dist` to `mod_bot_dungeon_queue.conf` and adjust:

| Option | Default | Description |
|---|---|---|
| `BotDungeonQueue.Enable` | `true` | Master switch |
| `BotDungeonQueue.QueueInterval` | `30` | Seconds between queue checks |
| `BotDungeonQueue.MinLevel` | `15` | Minimum level to queue |
| `BotDungeonQueue.MaxBotsPct` | `50` | Max % of online bots in dungeons at once |
| `BotDungeonQueue.RespectBgQueue` | `true` | Don't queue bots in BG queues |
| `BotDungeonQueue.StuckCleanupInterval` | `300` | Seconds between stuck cleanups |
| `BotDungeonQueue.StuckCleanupGracePeriod` | `300` | Seconds of grace before cleanup evicts |

## Required worldserver.conf settings

```ini
playerbots.randomBotJoinLfg = 1
AccountInstancesPerHour = 100
PartyMaxSpread = 200
PullDynamicPartyLag = 3
PullDynamicMaxLeeroyMobs = 999
RestHealthPct = 1
RestManaPct = 1
```

## Known issues

- **Instance sharing reliability** — some followers still resolve a separate instance. The pre-allocation + perm bind approach works for most members but occasionally a follower's `PlayerGetDestinationInstanceId` returns 0. Believed to be a race in `HandleMoveWorldportAck` where the cached perm bind isn't found during the async map transfer.
- **LFG interference** — the stock playerbots LFG system can re-queue bots inside a dungeon group. `LeaveLfg` is called on all members at queue time to mitigate this.
- **DC leadership** — `IsDungeonClearLeader` checks `PlayerbotAI::IsTank`, which may disagree with the queue's `GetBotRole` for some specs (e.g., Druids without feral talents). The tank strategy is explicitly added in `EnableDcOn` to align them.
- **Click mechanics** — some dungeons have scripted events requiring NPC/gameobject interaction (e.g., Uldaman stones, SFK courtyard door). These need `DungeonEvent` registrations; SFK and Deadmines are already wired.

## Rebuild & deploy

From the build container (`acore-fast`):

```bash
# Compile single changed file
cd /azerothcore/build.host
make modules/CMakeFiles/modules.dir/mod-bot-dungeon-queue/src/BotDungeonQueue.cpp.o

# Update archive
ar crs modules/libmodules.a modules/CMakeFiles/modules.dir/mod-bot-dungeon-queue/src/BotDungeonQueue.cpp.o

# Relink worldserver
make -j$(nproc) worldserver

# Copy binary
docker cp src/server/apps/worldserver ac-worldserver:/azerothcore/env/dist/bin/worldserver
docker restart ac-worldserver
```
