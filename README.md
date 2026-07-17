# mod-bot-dungeon-queue

This module makes bot parties form up, enter dungeons, and clear them without human involvement. Every 30 seconds it checks for available bots, groups them by level bracket, teleports them into a shared dungeon instance, and enables autonomous navigation and combat through [mod-dungeon-clear](https://github.com/Thug-Dracula/mod-dungeon-clear).

Bosses are killed, trash is pulled, deaths are recovered, and completed groups exit and re-queue. The system handles full wipes (instant release, retry up to 4 times) and individual deaths (resurrection hold up to 30s, then ghost teleport back to dungeon entrance for corpse-run).

**This is beta software — expect crashes.** Multiple dungeons have been fully cleared (Shadowfang Keep, Deadmines, Stormwind Stockade, Ragefire Chasm). Several issues remain (intermittent worldserver crash, ~88% instance sharing reliability, missing click mechanics). See [Known issues](#known-issues) below.

---

`mod-bot-dungeon-queue` is an AzerothCore C++ module that orchestrates autonomous bot dungeon clearing. It depends on [mod-playerbots](https://github.com/azerothcore/mod-playerbots) (the bot AI framework) and [mod-dungeon-clear](https://github.com/Thug-Dracula/mod-dungeon-clear) (the dungeon navigation/pathfinding layer).

The high-level flow:

1. **Queue loop** (every 30s) — scans all online bots, groups them by faction and level bracket (5-level windows), forms a party of 5 (1 tank + 1 healer + 3 DPS via `GetBotRole`).
2. **Instance binding** — a fresh `InstanceSave` is pre-allocated and all 5 members are permanently bound to it. This ensures AzerothCore's `PlayerGetDestinationInstanceId()` resolves to the same instance for every member.
3. **Staggered teleport** — tank teleports first via `Player::TeleportTo()` to the dungeon entrance. Followers are queued (`m_pendingFollows`) and teleported on the next `OnUpdate` tick. This gives the tank's map transfer time to settle before followers arrive.
4. **DC enablement** — `mod-dungeon-clear` is enabled on the tank only (`DcRunState.enabled = true`). Followers keep `enabled = false` and resolve the leader via `PartyTank` cross-bot lookup, then use DC's built-in follow/assist system.
5. **Death handling** — when a res class (priest/paladin/shaman/druid) is alive, DC pauses up to 30s for resurrection. No res available → immediate ghost release. Ghosts teleport home (step 1) then back to dungeon entrance (step 2) for corpse-run. Spirit healer rez is prevented — bots corpse-run instead. A DC watchdog re-enables dungeon clear after death.
6. **Wipe counter** — tracks wipes per instance via `g_wipeCount`. Only full-party wipes increment the counter (solo deaths do not consume a retry). After `MaxWipesBeforeEvict` (default 4), the group is teleported home.
7. **Completion exit** — checks `InstanceSave::GetCompletedEncounterMask()` against DBC `DungeonEncounterEntry` indices. When all bits match, the group is teleported home.
8. **Orphaned survivor sweep** — if a group's tank is dead and no res is available, alive members are teleported home and the group disbands instead of roaming alone.
9. **Stuck cleanup** — periodic sweep (every `StuckCleanupInterval`) teleports solo bots still lodged in dungeon maps back to their home city.
10. **LFG interference prevention** — bots set a `pending_dungeon` flag that `LfgActions::JoinLFG()` checks before re-queuing them through the stock LFG system.

---

## What works

Dungeon clearing is proven end-to-end across multiple sessions and group compositions. 25 dungeons are wired and navigable:

| Dungeon | Status |
|---|---|
| Shadowfang Keep (33) | **Full clear confirmed** — 8/8 bosses. Multiple groups. |
| Deadmines (36) | **Full clear confirmed** — 7/7 bosses (VanCleef killed). |
| Stormwind Stockade (34) | **Full clear confirmed** — 5/5 bosses. |
| Ragefire Chasm (389) | **Full clear confirmed** — 4/4 bosses. |
| Blackfathom Deeps (48) | **Active progress** — bots navigate and engage bosses. |
| Wailing Caverns (43) | Wired — navmesh now available. |
| Razorfen Kraul (47) | Wired — navmesh now available. |
| Razorfen Downs (129) | Wired with entrance coords. |
| Uldaman (70) | Wired — full-clear untested. |
| Gnomeregan (90) | Wired — navmesh available. |
| Scarlet Monastery (189) | Wired — navmesh available. |
| Zul'Farrak (209) | Wired — entrance coords fixed (was pointing to Gnomeregan). |
| Blackrock Depths (230) | Wired — entrance coords fixed (was pointing to Scholomance). |
| Blackrock Spire (229) | Wired — entrance coords fixed (was pointing to BRD). |
| Stratholme (329) | Wired — entrance coords fixed (was pointing to nonexistent map). No vmap data (DungeonSpawnGraph fallback). |
| Scholomance (289) | Wired — navmesh available. |
| Dire Maul (429) | Wired — navmesh available. |
| Maraudon (349) | Wired — entrance coords fixed. |
| Sunken Temple (109) | Wired — entrance coords fixed. |
| Hellfire Ramparts (543) | Wired — entrance coords fixed (was pointing to Stratholme). |
| Blood Furnace (542) | Wired — TBC. |
| Slave Pens (547) | Wired — TBC. |
| Underbog (546) | Wired — TBC. |
| Old Hillsbrad (560) | Wired — entrance coords fixed (was pointing to nonexistent map). |
| Mechanar (554) | Wired — TBC. |

**Working features:**

- Group formation by level bracket (tank/healer/DPS detection)
- Shared instance binding (perm bind + deferred tick, ~88% success)
- Dungeon navigation and boss fights (via mod-dungeon-clear)
- Party-combat override — DPS attack when ANY party member is in combat
- Forced-advance timeout (20s) — prevents indefinite idle after pull aborts
- Tank-only DC leadership — only the tank drives advance
- **Death recovery** — resurrection hold (30s pause if res class alive), ghost teleport step 1→2 back to dungeon entrance, corpse-run instead of spirit healer rez
- **Single-death wipe counting** — only full party wipes consume a retry; solo deaths skip the counter
- **No-tank assist fallback** — DPS assist each other when tank is dead
- **AnyGroupMemberInCombat distance fix** — distant party aggro detected regardless of tank range
- **Camp combat override** — party defends when patrols aggro at camp (even during passive hold)
- D Dungeon completion detection and auto-exit
- Orphaned survivor sweep — evicts groups with dead tank + no res
- Stuck cleanup (solo survivors in dungeon instances)
- LFG interference prevention (`pending_dungeon` flag + `LeaveLfg` call)
- DC watchdog — re-enables DC on live tanks after death-disable
- Follow distance raised to 15 yards
- Click mechanics wired: SFK courtyard (Alliance/Horde), Deadmines Iron Clad Door (Defias Cannon)

---

## Known issues

### Intermittent worldserver crash (exit code 0, no crash dump)
The worldserver exits cleanly (exit code 0) at irregular intervals — sometimes after 20 minutes, sometimes after 2 hours. No segfault, no abort signal, no OOM. The process simply stops. Likely a null dereference or assertion in an async path (heartbeat, assist, ghost teleport). An LD_PRELOAD hook is deployed to catch `exit()` and signal handlers, but the crash bypasses both — this suggests the crash is in a thread that calls `_Exit()` or `pthread_exit()` directly, or a segfault in a non-main thread that kills the process before the handler runs. **Root cause unknown.**

### Instance sharing reliability (~88%)
Some followers still resolve a separate instance despite pre-binding. The deferred-tick approach (`m_pendingFollows`: tank teleports immediately, followers in next `OnUpdate`) gives the tank's transfer a head start, but occasional races in `HandleMoveWorldportAck` cause split instances. This is an AzerothCore async teleport timing issue.

### BFD underwater navigation
Blackfathom Deeps has an underwater entrance at `(-151.89, 106.96, -39.87)`. Bots can drown or fail to path through swim zones. The `NavmeshSnap` system can't snap to valid walkable polys if the bot is swimming.

### Click mechanics
Several dungeons require NPC/gameobject interaction sequences (e.g., Uldaman stone pillars, Maraudon's portal, BRD bar/pub, BFD torches for Kelris). These need to be registered as `DungeonEvent` entries in mod-dungeon-clear's event system. Currently only SFK courtyard (door) and Deadmines (goblin cannon door) are wired.

### Stratholme has no vmap data
Map 201 has no `vmtree` or `vmtile` files in the ChromieCraft extraction. It relies on `DungeonSpawnGraph` fallback (navmesh-free pathfinding). Navigation may be unreliable.

### Resurrection hold timeout
When a res class is alive, the tank pauses for 30s. If no resurrection happens within 30s, the dead bot releases ghost and teleports back. In practice, the stock playerbots healer sometimes doesn't cast resurrection in time (busy with other actions, out of range, etc.).

### Orphaned survivors after server crash
After an unclean shutdown, the startup cleanup teleports all bots out of dungeons. Pending ghost teleports (step 2) are lost — the in-memory `g_pendingGhostTeleports` map doesn't survive a restart. Affected bots end up at homebind and may need a manual nudge.

### Stuck bots on invalid map after crash
Bots caught mid-teleport during a server crash land on `Map:4294967295` (MAP_INVALID). The startup cleanup and subsequent stuck cleanup should clear these, but some may fall through cracks and stay in limbo.

### Navmesh gaps in specific maps
Wailing Caverns (43) and Razorfen Kraul (47) now have generated navmesh, but some interior tiles may have "No vertices to build tile" warnings (terrain-only areas). Stratholme (201) has no vmap data at all.

---

## Future features (planned)

- **Fix intermittent worldserver crash** — the #1 priority. Crash dumps aren't being captured despite LD_PRELOAD hooks. Likely a null dereference in an async thread.
- **Complete click mechanic coverage** — register DungeonEvent entries for Uldaman, Maraudon, BRD, BFD torches, etc.
- **Improve instance sharing** — investigate sync enhancements or a retry mechanism for failed followers.
- **Water/swim handling** — entrance coords for BFD and other aquatic zones, or a bot swim-state override.
- **Instance selection by gear/bot power** — don't just match by level; consider gear score.
- **Persistence** — save/restore groups and pending ghost teleports across server restarts.
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
| `BotDungeonQueue.TeleportOutOnDeath` | `1` | Teleport survivors home on wipe |
| `BotDungeonQueue.RespectBgQueue` | `1` | Skip bots queued for battlegrounds |
| `BotDungeonQueue.WhisperReplies` | `1` | Bot chat mode (0=say, 1=whisper) |
| `BotDungeonQueue.MaxWipesBeforeEvict` | `4` | Max party wipes before teleporting home |
| `BotDungeonQueue.StuckCleanupInterval` | `120` | Seconds between stuck sweeps |
| `BotDungeonQueue.StuckCleanupGracePeriod` | `300` | Seconds before a freshly-teleported group is evicted |

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

We extracted our navmeshes from a ChromieCraft client. Most dungeons have full navmesh now (after regeneration from client MPQs). The ChromieCraft Docker image was missing tiles for Stockade, RFC, BFD, and others — these were regenerated successfully. Stratholme (201) has no vmap data at all and relies on DungeonSpawnGraph fallback.

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
