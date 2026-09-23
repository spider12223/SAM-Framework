#!/usr/bin/env bash
# Run a mod's own tests with nobody at the keyboard, and answer with an exit code.
#
#   tools/run_mod_test.sh HelloTest
#   tools/run_mod_test.sh YourMod --floor 1 --timeout 180
#   tools/run_mod_test.sh YourMod,HelloTest --class barbarian --seed 12345
#
# 0 every check passed        2 the mod never finished and the watchdog stopped it
# 1 something failed          3 the mods could not be loaded
#
# The game still opens a window: it is a real run of the real engine on a real dungeon,
# which is the entire point. It is small and it closes itself.
set -u

MODS=""
CLASS=""
SEED=""
FLOOR=""
TIMEOUT="180"
SIZE="640x480"
GAME="${BARONY_DIR:-/c/Program Files (x86)/Steam/steamapps/common/Barony}"
KEEP_LOG=""

usage() {
	sed -n '2,12p' "$0" | sed 's/^# \?//'
	echo
	echo "  --class <name>     class to start as (default barbarian)"
	echo "  --seed <n>         dungeon seed (default fixed, so a failure repeats)"
	echo "  --floor <n>        take the stairs down to floor n before leaving the mod to it."
	echo "                     Needed by any mod whose checks run on game.on_level_entered,"
	echo "                     because the floor a new game opens on never fires it."
	echo "  --timeout <s>      watchdog, default 180. 0 disables it (do not, unattended)."
	echo "  --size <WxH>       window size, default 640x480"
	echo "  --game <path>      the Barony folder (or set BARONY_DIR)"
	echo "  --keep-log <path>  copy sam_log.txt here afterwards"
	exit 64
}

while [ $# -gt 0 ]; do
	case "$1" in
		--class)    CLASS="$2"; shift 2;;
		--seed)     SEED="$2"; shift 2;;
		--floor)    FLOOR="$2"; shift 2;;
		--timeout)  TIMEOUT="$2"; shift 2;;
		--size)     SIZE="$2"; shift 2;;
		--game)     GAME="$2"; shift 2;;
		--keep-log) KEEP_LOG="$2"; shift 2;;
		-h|--help)  usage;;
		-*)         echo "unknown option: $1" >&2; usage;;
		*)          if [ -z "$MODS" ]; then MODS="$1"; else MODS="$MODS,$1"; fi; shift;;
	esac
done
[ -n "$MODS" ] || usage

EXE="$GAME/barony.exe"
[ -f "$EXE" ] || { echo "no barony.exe at $EXE (pass --game or set BARONY_DIR)" >&2; exit 65; }

# Every named mod has to be installed where the game will look for it. Saying so here beats
# an exit code 3 and a log line, because the fix is "copy the folder" either way.
missing=""
IFS=',' read -ra WANTED <<< "$MODS"
for m in "${WANTED[@]}"; do
	[ -d "$GAME/mods/$m" ] || missing="$missing $m"
done
if [ -n "$missing" ]; then
	echo "not installed in $GAME/mods:$missing" >&2
	echo "copy the mod folder(s) there first." >&2
	exit 66
fi

ARGS=(-samtest="$MODS" -samtesttimeout="$TIMEOUT" -windowed -size="$SIZE")
[ -n "$CLASS" ] && ARGS+=(-samtestclass="$CLASS")
[ -n "$SEED" ]  && ARGS+=(-samtestseed="$SEED")
[ -n "$FLOOR" ] && ARGS+=(-samtestfloor="$FLOOR")

echo "running $MODS (timeout ${TIMEOUT}s)..."
started=$(date +%s)
( cd "$GAME" && ./barony.exe "${ARGS[@]}" >/dev/null 2>&1 )
code=$?
elapsed=$(( $(date +%s) - started ))

LOG="$GAME/sam_log.txt"
[ -n "$KEEP_LOG" ] && cp -f "$LOG" "$KEEP_LOG" 2>/dev/null

echo
if [ -f "$LOG" ]; then
	# What the mod said, then anything the framework complained about. A test mod that
	# deliberately exercises a refusal logs errors on a PASSING run, so these are printed
	# as context, never as a verdict: the exit code is the verdict.
	grep -E "RESULT|  failed:" "$LOG" | sed 's/^/  /'
	errs=$(grep -cE "ERROR " "$LOG")
	warns=$(grep -cE "WARN " "$LOG")
	echo "  framework log: $errs error(s), $warns warning(s)  ($LOG)"
fi

echo
case $code in
	0) echo "PASS  (${elapsed}s)";;
	1) echo "FAIL  (${elapsed}s) - the mod reported a failed check";;
	2) echo "HUNG  (${elapsed}s) - no mod called sam_test_done before the watchdog. A mod whose"
	   echo "      checks run on game.on_level_entered needs --floor 1, and a mod that needs the"
	   echo "      player to move cannot be run this way at all.";;
	3) echo "LOAD  (${elapsed}s) - the mods could not be loaded: a folder that would not mount, or a"
	   echo "      script that failed to parse or errored at load. sam_log.txt says which.";;
	*) echo "EXIT $code  (${elapsed}s) - the game itself failed to start or crashed on the way out";;
esac
exit $code
