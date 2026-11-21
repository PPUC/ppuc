#!/usr/bin/env bash
#
# Renders ppuc-menu's first frame headless and checks what came out.
#
# The menu is the first thing a machine with several game folders shows, on a
# backbox with no keyboard and nobody watching the boot. A menu that draws a
# black screen, or draws at the wrong size because it ignored the game's
# [Translite] section, leaves a machine that looks dead. This runs it on SDL's
# offscreen driver with two game folders, the second with a translite image,
# and checks the screenshot is a real frame at the translite's size.
#
# Needs no boards (--screenshot never opens them) and no display. Exits 77
# (skip) when the packaged ppuc-menu is not there.
#
# Overrides: PPUC_MENU_BIN, PPUC_MENU_KEEP (keep the screenshot and print where).

set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
BIN="${PPUC_MENU_BIN:-${ROOT}/ppuc/ppuc-menu}"
IMAGE="${ROOT}/../ppuc_games/tmwrp/translite-off.png"

if [ ! -x "${BIN}" ]; then
   echo "SKIP: no ppuc-menu at ${BIN}; run ./build.sh first"
   exit 77
fi

WORK="$(mktemp -d -t ppuc_menu_render.XXXXXX)"
if [ -z "${PPUC_MENU_KEEP:-}" ]; then
   trap 'rm -rf "${WORK}"' EXIT
fi
GAMES="${WORK}/games"
SHOT="${WORK}/menu.png"

game() {
   mkdir -p "${GAMES}/$1"
   cat >"${GAMES}/$1/io-boards.yaml" <<'EOF'
ppucVersion: 1
rom: dummy
serialPort: dummy
platform: SYS6
coinDoorClosedSwitch: 0
gameOnSolenoid: 0
debug: false
boards:
  - { description: 'Cabinet', number: 1, pollEvents: true }
switches:
  - { description: 'Start Button', number: 1, board: 1, port: 3, debounce: 5 }
  - { description: 'Left Flipper Button', number: 2, board: 1, port: 4, debounce: 3 }
  - { description: 'Right Flipper Button', number: 3, board: 1, port: 5, debounce: 3 }
EOF
   printf '%s\n' "$2" >"${GAMES}/$1/ppuc.ini"
}

# The first folder decides the screen: 1280x720, a size no default produces.
# In a window, because fullscreen takes the display's own mode -- as the
# translite does with the same flags -- and the offscreen display is 1920x1080.
game a-original "[Game]
Rom = tmwrp_l2
[Translite]
Width = 1280
Height = 720
Window = true"
game b-lua "[Game]
Engine = script
[Menu]
Title = A Lua game with a title long enough that it has to be shortened to fit on its card
Info = Rules written in Lua, on the same playfield, wired the same way"
if [ -f "${IMAGE}" ]; then
   cp "${IMAGE}" "${GAMES}/a-original/translite-off.png"
   cp "${IMAGE}" "${GAMES}/a-original/translite-on.png"
fi

LOG="${WORK}/menu.log"
SDL_VIDEODRIVER=offscreen "${BIN}" --games-dir "${GAMES}" --no-sound \
   --screenshot "${SHOT}" >"${LOG}" 2>&1
status=$?
if [ "${status}" -ne 0 ] || [ ! -s "${SHOT}" ]; then
   cat "${LOG}"
   echo "FAIL: ppuc-menu exited ${status} without a screenshot"
   exit 1
fi

# Width and height from the PNG header, big-endian at bytes 16 and 20.
read_u32() { od -An -tu1 -v -j "$1" -N4 "${SHOT}" | tr -s ' \n' ' ' | awk '{ print ($1 * 16777216) + ($2 * 65536) + ($3 * 256) + $4 }'; }
width="$(read_u32 16)"
height="$(read_u32 20)"
if [ "${width}" != "1280" ] || [ "${height}" != "720" ]; then
   cat "${LOG}"
   echo "FAIL: screenshot is ${width}x${height}, expected the [Translite] 1280x720"
   exit 1
fi

# A frame of one colour compresses to almost nothing; cards, text and an
# image do not. Generous, so a font change does not turn this red.
size="$(wc -c <"${SHOT}" | tr -dc '0-9')"
if [ "${size}" -lt 8000 ]; then
   cat "${LOG}"
   echo "FAIL: screenshot is ${size} bytes, which is a blank screen"
   exit 1
fi

grep -E '^Menu: (font|plain text|.* renderer)' "${LOG}"
if [ -n "${PPUC_MENU_KEEP:-}" ]; then
   echo "screenshot: ${SHOT}"
fi
echo "PASS: ${width}x${height}, ${size} bytes"
