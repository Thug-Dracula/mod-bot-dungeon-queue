# mod-bot-dungeon-queue — AzerothCore module build registration.
#
# Automates bot-only dungeon groups: queues bots by level bracket, teleports
# them into matching dungeons, and manages the full lifecycle (clear → exit,
# death → exit, stuck cleanup). Requires mod-playerbots and mod-dungeon-clear.
#
# Sources are auto-collected from src/ (recursively) by AzerothCore's module
# system; AC_ADD_SCRIPT registers the script-loader path.
# Editing this file forces CMake re-glob on next build.

AC_ADD_SCRIPT("${CMAKE_CURRENT_LIST_DIR}/src")

AC_ADD_CONFIG_FILE("${CMAKE_CURRENT_LIST_DIR}/conf/mod_bot_dungeon_queue.conf.dist")
