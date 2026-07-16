# mod-bot-dungeon-queue

This module makes bot parties form up, enter dungeons, and clear them without human involvement. Every 30 seconds it checks for available bots, groups them by level bracket, teleports them into a shared dungeon instance, and enables autonomous navigation and combat through [mod-dungeon-clear](https://github.com/Thug-Dracula/mod-dungeon-clear).

Bosses are killed, trash is pulled, deaths are recovered, and completed groups exit and re-queue. The system handles full wipes (instant release, retry up to 4 times) and individual deaths (wait for resurrection, or release after 6 minutes).

**This is beta software.** Multiple dungeons have been fully cleared (Shadowfang Keep, Deadmines, Stormwind Stockade). Several features are incomplete (underwater navigation, click mechanics, instance sharing is ~88% reliable). See [Known issues](#known-issues) below.

---

`mod-bot-dungeon-queue` is an AzerothCore C++ module that orchestrates autonomous bot dungeon clearing. It depends on [mod-playerbots](https://github.com/azerothcore/mod-playerbots) (the bot AI framework) and [mod-dungeon-clear](https://github.com/Thug-Dracula/mod-dungeon-clear) (the dungeon navigation/pathfinding layer).

The high-level flow:

1. **Queue loop** (every 30s) — scans all online bots, groups them by faction and level bracket (5-level windows), forms a party of 5 (1 tank + 1 healer + 3 DPS via `GetBotRole`).
2. **Instance binding** — a fresh `InstanceSave` is pre-allocated and all 5 members are permanently bound to it. This ensures AzerothCore's `PlayerGetDestinationInstanceId()` resolves to the same instance for every member.
3. **Staggered teleport** — tank teleports first via `Player::TeleportTo()` to the dungeon entrance. Followers are queued (`m_pendingFollows`) and teleported on the next `OnUpdate` tick. This gives the tank's map transfer time to settle before followers arrive.
4. **DC enablement** — `mod-dungeon-clear` is enabled on the tank only (`DcRunState.enabled = true`). Followers keep `enabled = false` and resolve the leader via `PartyTank` cross-bot lookup, then use DC's built-in follow/assist system.
5. **Death handling** — individual deaths wait 6 minutes for resurrection; full party wipes trigger immediate spirit release. A DC watchdog (runs every heartbeat) re-enables dungeon clear on the tank after `mod-dungeon-clear` disables it on death.
6. **Wipe counter** — tracks wipes per instance via a `std::unordered_map<instanceId, int>` (`g_wipeCount`). After `MaxWipesBeforeEvict` (default 4), the group is teleported home.
7. **Completion exit** — checks `InstanceSave::GetCompletedEncounterMask()` against DBC `DungeonEncounterEntry` indices. When all bits match, the group is teleported home.
8. **Stuck cleanup** — periodic sweep (every `StuckCleanupInterval`) teleports solo bots still lodged in dungeon maps back to their home city.
9. **LFG interference prevention** — bots set a `pending_dungeon` flag that `LfgActions::JoinLFG()` checks before re-queuing them through the stock LFG system.

---

## What works

Dungeon clearing is proven end-to-end across multiple sessions and group compositions:

| Dungeon | Status |
|---|---|
| Shadowfang Keep (33) | **Full clear confirmed** — 8/8 bosses. Multiple groups have done it. |
| Deadmines (36) | **Full clear confirmed** — 7/7 bosses (VanCleef killed). |
| Stormwind Stockade (34) | **Full clear confirmed** — 5/5 bosses. |
| Blackfathom Deeps (48) | **Active progress** — bots navigate deeper each session. Underwater entry partially handled. |
| Razorfen Downs (129) | Wired — entrance coords set but full-clear untested. |
| Uldaman (70) | Wired — entrance coords set but full-clear untested. |
| All others | Wired with entrance coords; clear status unknown. |

**Working features:**

- Group formation by level bracket (tank/healer/DPS detection)
- Shared instance binding (perm bind + deferred tick, ~88% success — occasional race conditions in async `HandleMoveWorldportAck`)
- Dungeon navigation and boss fights (via mod-dungeon-clear)
- Party-combat override — DPS attack when ANY party member is in combat (not just the tank)
- Forced-advance timeout (20s) — prevents indefinite idle after pull aborts
- Tank-only DC leadership — only the tank gets `DcRunState.enabled = true`; followers resolve leader via `PartyTank`
- Death recovery (individual 6-min timer + full-party instant release)
- Wipe counter (configurable max wipes before eviction)
- Dungeon completion detection and auto-exit
- Stuck cleanup (solo survivors in dungeon instances)
- LFG interference prevention (`pending_dungeon` flag + `LeaveLfg` call)
- DC watchdog — re-enables DC on live tanks after death-disable
- Follow distance raised to 15 yards
- Click mechanics wired: SFK courtyard (Alliance/Horde), Deadmines Iron Clad Door (Defias Cannon)

---

## Known issues

### Instance sharing reliability (~88%)
Some followers still resolve a separate instance despite pre-binding. The deferred-tick approach (`m_pendingFollows`: tank teleports immediately, followers in next `OnUpdate`) gives the tank's transfer a head start, but occasional races in `HandleMoveWorldportAck` cause split instances. This is an AzerothCore async teleport timing issue — not easily fixable without engine changes.

### BFD underwater navigation
Blackfathom Deeps has an underwater entrance at `(-151.89, 106.96, -39.87)`. Bots can drown or fail to path through swim zones. The `NavmeshSnap` system can't snap to valid walkable polys if the bot is swimming. A dedicated swim-state handler or adjusted entrance coords may be needed.

### Click mechanics
Several dungeons require NPC/gameobject interaction sequences (e.g., Uldaman stone pillars, Maraudon's portal, BRD bar/pub). These need to be registered as `DungeonEvent` entries in mod-dungeon-clear's event system. Currently only SFK courtyard (door) and Deadmines (goblin cannon door) are wired.

### Navmesh gaps in specific dungeons
Ragefire Chasm (389), Wailing Caverns (43), and Razorfen Kraul (47) are removed from BotDungeonPorts due to missing/partial navmesh tiles. If you have a full client, regenerate the navmeshes and add them back.

### Death-disable re-enable
`mod-dungeon-clear` disables `DcRunState.enabled` on any party death (to prevent death-loops). The DC watchdog in the heartbeat re-enables it within ~60s, but there's a window where the group sits idle. This is a safety feature of mod-dungeon-clear, not something we can fully bypass.

---

## Future features (planned)

- **Water/swim handling** — entrance coords for BFD and other aquatic zones, or a bot swim-state override.
- **Complete click mechanic coverage** — register all missing DungeonEvent entries (Uldaman, Maraudon, BRD, etc.).
- **Improve instance sharing** — investigate sync enhancements or a retry mechanism for failed followers.
- **Instance selection by gear/bot power** — don't just match by level; consider gear score.
- **Persistence** — save/restore groups across server restarts.
- **Dashboard** — web or in-game status showing active groups, progress, last wipe.
- **Evasion handling** — detect bots stuck in evasion (mob ran to spawn, bot can't reach it) and force-advance.

---

## Configuration

Copy `conf/mod_bot_dungeon_queue.conf.dist` to `mod_bot_dungeon_queue.conf` and edit:

| Option | Default | Description |
|---|---|---|
| `BotDungeonQueue.Enable` | `1` | Master switch (0 = off, 1 = on) |
| `BotDungeonQueue.QueueInterval` | `30` | Seconds between queue checks |
| `BotDungeonQueue.MinLevel` | `15` | Minimum level to be eligible |
| `BotDungeonQueue.MaxBotsPct` | `50` | Max % of online bots in dungeons at once |
| `BotDungeonQueue.RespectBgQueue` | `1` | Skip bots queued for battlegrounds |
| `BotDungeonQueue.StuckCleanupInterval` | `300` | Seconds between stuck sweeps |
| `BotDungeonQueue.StuckCleanupGracePeriod` | `300` | Seconds before a freshly-teleported group is evicted |
| `BotDungeonQueue.MaxWipesBeforeEvict` | `4` | Max party wipes before teleporting home |

### Required worldserver.conf settings

```ini
playerbots.randomBotJoinLfg = 1
AccountInstancesPerHour = 100
PartyMaxSpread = 200
PullDynamicPartyLag = 3
PullDynamicMaxLeeroyMobs = 1
RestHealthPct = 1
RestManaPct = 1
```

---

## How navmesh affects this mod

Bot pathfinding in dungeons depends on navigation mesh (navmesh) data — files that tell the AI where it can walk, swim, or jump. These files are generated from a WoW 3.3.5a client using the standard `mapextractor → vmap4assembler → mmaps_generator` pipeline.

**If you're using this module for the first time:**

1. You need a **full, genuine WoW 3.3.5a (Patch 3.3.5, build 12340)** installation on the machine running the server.
2. Run `mapextractor`, `vmap4assembler`, and `mmaps_generator` from your core build. The resulting `.mmap`/`.mmtile` files go into `env/dist/data/mmaps/`.
3. Dungeons with missing or incomplete navmeshes will cause bots to stand still. If a particular dungeon doesn't work, check whether its navmesh tiles exist.

We extracted our navmeshes from a ChromieCraft client, which was missing several maps (RFC, WC, RFK). If you use a complete official client, all maps should generate correctly.

---

## Build & deploy

The compiled binary at `build.host/src/server/apps/worldserver` is bind-mounted into the worldserver container for fast deploys.

```bash
cd /opt/azerothcore/azerothcore-wotlk

# Full rebuild (all modules)
sudo docker exec acore-fast bash -c 'cd /azerothcore/build.host && make -j$(nproc) worldserver'

# Copy to host (bind-mount serves it to the container)
sudo docker cp acore-fast:/azerothcore/build.host/src/server/apps/worldserver build.host/src/server/apps/worldserver

# Restart worldserver
sudo docker compose -f docker-compose.yml -f docker-compose.override.yml restart ac-worldserver
```

---

## Dependencies

- [mod-playerbots](https://github.com/azerothcore/mod-playerbots) — bot AI framework. Required.
- [mod-dungeon-clear](https://github.com/Thug-Dracula/mod-dungeon-clear) — dungeon navigation, pull, combat, and boss logic. Required.

---

## Repository

https://github.com/Thug-Dracula/mod-bot-dungeon-queue
