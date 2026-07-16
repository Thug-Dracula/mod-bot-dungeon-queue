# mod-bot-dungeon-queue

> **⚠️ IN DEVELOPMENT**  
> Multiple groups have been observed pushing deep into Deadmines (~300yd from entrance, past the first boss Rhahk'zor). Instance sharing, DC leadership, death recovery, and forced-advance timeout are working. Click mechanics (SFK courtyard, Deadmines goblin door) still need DungeonEvent registrations.

Autonomous dungeon queuing and clearing for bot groups. Forms 5-player parties (tank + healer + 3 DPS) by level bracket, teleports them into a shared dungeon instance, and enables [mod-dungeon-clear](https://github.com/Thug-Dracula/mod-dungeon-clear) on every member so the group navigates, engages bosses, and clears the dungeon without player input.

Depends on [mod-playerbots](https://github.com/azerothcore/mod-playerbots) and [mod-dungeon-clear](https://github.com/Thug-Dracula/mod-dungeon-clear).

## How it works

1. **Queue** — every `QueueInterval` (default 30s), eligible bots are grouped by faction + level bracket (5-level segments). Groups of 5 (1 tank / 1 healer / 3 DPS) are formed.
2. **Pre-bind** — a shared `InstanceSave` is pre-allocated and all 5 members are permanently bound to it, so `PlayerGetDestinationInstanceId` resolves to the same instance for every member.
3. **Teleport** — all members are teleported via `Player::TeleportTo` to the entrance coordinates of a matching dungeon. The stagger (tank first, then followers) ensures the tank's map transfer initiates first.
4. **Dungeon Clear** — `mod-dungeon-clear` is enabled on the tank (only). Followers resolve the leader via `PartyTank` and follow/fight through DC's assist system. Skull sync every 2s.
5. **Death handling** — on individual death, waits 6 minutes for a resurrection. On full wipe (all party members dead), releases immediately. A DC watchdog re-enables dungeon clear on the tank after death-disable within ~60s.
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
PullDynamicMaxLeeroyMobs = 1
RestHealthPct = 1
RestManaPct = 1
```

## Known issues

- **Instance sharing reliability** — some followers still resolve a separate instance (~88% success). The pre-allocation + perm bind + deferred tick approach works for most members but occasional races in `HandleMoveWorldportAck` still occur.
- **LFG interference** — the stock playerbots LFG system can re-queue bots inside a dungeon group. `LeaveLfg` + `pending_dungeon` guard mitigate this.
- **DC leadership** — `DcRunState.enabled` is now only set on the tank; followers use `PartyTank` cross-bot resolution. This matches the DC module's design.
- **Click mechanics** — some dungeons have scripted events requiring NPC/gameobject interaction (e.g., Deadmines goblin door, Uldaman stones). SFK courtyard is wired.
- **Death-disable** — `mod-dungeon-clear` disables the run on any party death. The DC watchdog re-enables it within ~60s.
- **Party-combat** — DPS now attacks when ANY groupmate is in combat, not just the tank. Prevents idle-DPS after tank death.

## Rebuild & deploy

The binary at `build.host/src/server/apps/worldserver` is bind-mounted into the worldserver container.

```bash
cd /opt/azerothcore/azerothcore-wotlk

# Full rebuild (all modules)
sudo docker exec acore-fast bash -c 'cd /azerothcore/build.host && make -j$(nproc) worldserver'

# Copy to host (bind-mount serves it to the container)
sudo docker cp acore-fast:/azerothcore/build.host/src/server/apps/worldserver build.host/src/server/apps/worldserver

# Restart worldserver
sudo docker compose -f docker-compose.yml -f docker-compose.override.yml restart ac-worldserver
```
