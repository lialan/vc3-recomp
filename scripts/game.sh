# scripts/game.sh: sourced by the scripts that work on one game. With the
# first argument a folder name under games/ (vc3), that game, and the
# argument shifted off; otherwise vc3. Sets GAME, GAME_ROOT (games/<id>),
# everything in its game.conf, and GAME_DIR: the game's folder
# ($CHIHIRO_GAME_DIR, else $<ENV_PREFIX>_GAME_DIR, else GAME_DIR_DEFAULT).
#   . "$ROOT/scripts/game.sh" "$@"; set -- "${GAME_ARGS[@]}"
GAME=vc3
GAME_ARGS=("$@")
if [ $# -gt 0 ] && [ -f "$ROOT/games/$1/game.conf" ]; then
    GAME=$1
    GAME_ARGS=("${@:2}")
fi
GAME_ROOT="$ROOT/games/$GAME"
[ -f "$GAME_ROOT/game.conf" ] || { echo "no games/$GAME/game.conf" >&2; exit 2; }
. "$GAME_ROOT/game.conf"
_gd_var="${ENV_PREFIX}_GAME_DIR"
GAME_DIR="${CHIHIRO_GAME_DIR:-${!_gd_var:-$GAME_DIR_DEFAULT}}"
unset _gd_var
