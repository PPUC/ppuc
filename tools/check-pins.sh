#!/bin/bash
#
# check-pins.sh - resolve and verify the PPUC dependency pin chain.
#
# The real inter-repo version contract lives in each repository's
# platforms/config.sh. Those pins are transitive: ppuc pins libppuc, which pins
# io-boards; ppuc pins libsdldmd, which pins libdmdutil. This script walks that
# chain from ppuc's config.sh and reports:
#
#   * the resolved dependency tree
#   * whether each pinned commit is reachable from its repository's main branch
#   * whether a local checkout differs from what is pinned
#
# Usage:
#   tools/check-pins.sh [--workspace DIR] [--offline] [--no-color]
#
#   --workspace DIR  where sibling checkouts live (default: parent of this repo)
#   --offline        skip all network access; verify only against local clones
#   --no-color       plain output, e.g. for CI logs
#
# Exit status:
#   0  every pin resolved and verified
#   1  at least one problem found (pin not on main, or local drift)
#   2  the chain could not be resolved (network/tooling failure)
#
# Requires: bash, git, curl (unless --offline).

set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
WORKSPACE="$(cd "${REPO_ROOT}/.." && pwd -P)"
OFFLINE=0
USE_COLOR=1
PROBLEMS=0
RESOLVE_FAILED=0
CACHE_DIR=""

while [ $# -gt 0 ]; do
   case "$1" in
      --workspace) WORKSPACE="$(cd "$2" && pwd -P)"; shift 2 ;;
      --offline)   OFFLINE=1; shift ;;
      --no-color)  USE_COLOR=0; shift ;;
      -h|--help)   sed -n '2,26p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
      *)           echo "Unknown option: $1" >&2; exit 2 ;;
   esac
done

if [ "${USE_COLOR}" = "1" ] && [ -t 1 ]; then
   C_DIM=$'\033[2m'; C_RED=$'\033[31m'; C_GREEN=$'\033[32m'
   C_YELLOW=$'\033[33m'; C_BOLD=$'\033[1m'; C_OFF=$'\033[0m'
else
   C_DIM=""; C_RED=""; C_GREEN=""; C_YELLOW=""; C_BOLD=""; C_OFF=""
fi

cleanup() { [ -n "${CACHE_DIR}" ] && rm -rf "${CACHE_DIR}"; }
trap cleanup EXIT
CACHE_DIR="$(mktemp -d)"

# --- repository table -------------------------------------------------------
# name|github slug|config.sh path relative to repo root ("-" if none to walk)

repo_slug() {
   case "$1" in
      ppuc)          echo "PPUC/ppuc" ;;
      libppuc)       echo "PPUC/libppuc" ;;
      libsdldmd)     echo "PPUC/libsdldmd" ;;
      io-boards)     echo "PPUC/io-boards" ;;
      libdmdutil)    echo "PPUC/libdmdutil" ;;
      vpinball)      echo "PPUC/vpinball" ;;
      pinmame)       echo "mkalkbrenner/pinmame" ;;
      *)             echo "" ;;
   esac
}

# Repositories whose own config.sh we can walk for further pins.
walkable_config() {
   case "$1" in
      libppuc|libsdldmd|libdmdutil) echo "platforms/config.sh" ;;
      *)                            echo "" ;;
   esac
}

# --- helpers ----------------------------------------------------------------

read_pin() {
   # read_pin <config file> <VAR NAME>
   local file="$1" var="$2" line value
   [ -f "${file}" ] || return 1
   line="$(grep -E "^[[:space:]]*${var}=" "${file}" 2>/dev/null | head -1)"
   [ -n "${line}" ] || return 0
   value="${line#*=}"
   value="${value%%#*}"
   value="${value//[$'\t\r\n ']/}"
   value="${value#\"}"; value="${value%\"}"
   value="${value#\'}"; value="${value%\'}"
   printf '%s' "${value}"
}

fetch_config() {
   # fetch_config <repo name> <sha> -> prints path to a local copy, or fails
   local name="$1" sha="$2" rel slug out
   rel="$(walkable_config "${name}")"
   [ -n "${rel}" ] || return 1

   local local_dir="${WORKSPACE}/${name}"
   if [ -d "${local_dir}/.git" ]; then
      out="${CACHE_DIR}/${name}-${sha}.sh"
      if git -C "${local_dir}" cat-file -e "${sha}^{commit}" 2>/dev/null &&
         git -C "${local_dir}" show "${sha}:${rel}" >"${out}" 2>/dev/null; then
         echo "${out}"; return 0
      fi
   fi

   [ "${OFFLINE}" = "1" ] && return 1
   slug="$(repo_slug "${name}")"
   [ -n "${slug}" ] || return 1
   out="${CACHE_DIR}/${name}-${sha}.sh"
   if curl -fsSL --max-time 20 \
        "https://raw.githubusercontent.com/${slug}/${sha}/${rel}" -o "${out}" 2>/dev/null; then
      echo "${out}"; return 0
   fi
   return 1
}

on_main() {
   # on_main <repo name> <sha> -> "yes" | "no" | "unknown"
   local name="$1" sha="$2" slug status local_dir="${WORKSPACE}/$1"

   if [ -d "${local_dir}/.git" ] && git -C "${local_dir}" cat-file -e "${sha}^{commit}" 2>/dev/null; then
      local main_ref=""
      for candidate in origin/main origin/master main master; do
         if git -C "${local_dir}" rev-parse --verify --quiet "${candidate}" >/dev/null 2>&1; then
            main_ref="${candidate}"; break
         fi
      done
      if [ -n "${main_ref}" ]; then
         if git -C "${local_dir}" merge-base --is-ancestor "${sha}" "${main_ref}" 2>/dev/null; then
            echo "yes"; return
         fi
         echo "no"; return
      fi
   fi

   [ "${OFFLINE}" = "1" ] && { echo "unknown"; return; }
   slug="$(repo_slug "${name}")"
   [ -n "${slug}" ] || { echo "unknown"; return; }

   # GitHub compare: main...<sha> is "identical" or "behind" when sha is an ancestor.
   status="$(curl -fsSL --max-time 20 \
      "https://api.github.com/repos/${slug}/compare/main...${sha}" 2>/dev/null |
      grep -m1 '"status"' | sed -E 's/.*"status"[[:space:]]*:[[:space:]]*"([^"]+)".*/\1/')"
   case "${status}" in
      identical|behind) echo "yes" ;;
      ahead|diverged)   echo "no" ;;
      *)                echo "unknown" ;;
   esac
}

local_state() {
   # local_state <repo name> <pinned sha> -> "NONE|" | "MATCH|<text>" | "DRIFT|<text>"
   # Runs in a command substitution, so it must not mutate counters; the caller
   # decides what counts as a problem based on the returned tag.
   local name="$1" sha="$2" dir="${WORKSPACE}/$1"
   [ -d "${dir}/.git" ] || { printf 'NONE|'; return; }

   local head branch short_head ahead n
   head="$(git -C "${dir}" rev-parse HEAD 2>/dev/null)" || { printf 'NONE|'; return; }
   branch="$(git -C "${dir}" rev-parse --abbrev-ref HEAD 2>/dev/null)"
   short_head="${head:0:8}"

   if [ "${head:0:${#sha}}" = "${sha}" ] || [ "${sha:0:${#head}}" = "${head}" ]; then
      printf 'MATCH|local %s @ %s (matches pin)' "${branch}" "${short_head}"
      return
   fi

   ahead=""
   if git -C "${dir}" cat-file -e "${sha}^{commit}" 2>/dev/null; then
      n="$(git -C "${dir}" rev-list --count "${sha}..HEAD" 2>/dev/null)"
      [ -n "${n}" ] && [ "${n}" != "0" ] && ahead=", ${n} commit(s) ahead of pin"
   fi
   printf 'DRIFT|local %s @ %s differs from pin%s' "${branch}" "${short_head}" "${ahead}"
}

report() {
   # report <indent> <name> <sha> [note]
   local indent="$1" name="$2" sha="$3" note="${4:-}"
   local short="${sha:0:8}" mark status drift

   if [ -z "${sha}" ]; then
      printf '%s%s %s%s\n' "${indent}" "${name}" "${C_RED}UNRESOLVED${C_OFF}" ""
      RESOLVE_FAILED=1
      return
   fi

   if [ -n "$(repo_slug "${name}")" ]; then
      status="$(on_main "${name}" "${sha}")"
      case "${status}" in
         yes)     mark="${C_GREEN}on main${C_OFF}" ;;
         no)      mark="${C_RED}NOT on main${C_OFF}"; PROBLEMS=$((PROBLEMS + 1)) ;;
         unknown) mark="${C_DIM}unverified${C_OFF}" ;;
      esac

      local raw tag text
      raw="$(local_state "${name}" "${sha}")"
      tag="${raw%%|*}"
      text="${raw#*|}"
      case "${tag}" in
         MATCH) drift="${C_DIM}${text}${C_OFF}" ;;
         DRIFT) drift="${C_YELLOW}${text}${C_OFF}"; PROBLEMS=$((PROBLEMS + 1)) ;;
         *)     drift="" ;;
      esac
   else
      mark="${C_DIM}third-party${C_OFF}"
      drift=""
   fi

   printf '%s%-22s %-10s %-24s %s %s\n' \
      "${indent}" "${name}" "${short}" "${mark}" "${drift}" "${note}"
}

# --- walk the chain ---------------------------------------------------------

PPUC_CONFIG="${REPO_ROOT}/platforms/config.sh"
if [ ! -f "${PPUC_CONFIG}" ]; then
   echo "Cannot find ${PPUC_CONFIG}" >&2
   exit 2
fi

PPUC_HEAD="$(git -C "${REPO_ROOT}" rev-parse --short HEAD 2>/dev/null || echo '?')"
PPUC_BRANCH="$(git -C "${REPO_ROOT}" rev-parse --abbrev-ref HEAD 2>/dev/null || echo '?')"

echo
echo "${C_BOLD}PPUC dependency pin chain${C_OFF}"
echo "${C_DIM}workspace: ${WORKSPACE}${C_OFF}"
[ "${OFFLINE}" = "1" ] && echo "${C_DIM}mode: offline (local clones only)${C_OFF}"
echo
echo "ppuc ${C_DIM}(${PPUC_BRANCH} @ ${PPUC_HEAD}, this repo)${C_OFF}"

LIBPPUC_SHA="$(read_pin "${PPUC_CONFIG}" LIBPPUC_SHA)"
LIBSDLDMD_SHA="$(read_pin "${PPUC_CONFIG}" LIBSDLDMD_SHA)"
VPINBALL_SHA="$(read_pin "${PPUC_CONFIG}" VPINBALL_SHA)"
PINMAME_SHA="$(read_pin "${PPUC_CONFIG}" PINMAME_SHA)"

report "  " "libppuc" "${LIBPPUC_SHA}"
if LIBPPUC_CFG="$(fetch_config libppuc "${LIBPPUC_SHA}")"; then
   report "    " "io-boards"     "$(read_pin "${LIBPPUC_CFG}" IO_BOARDS_SHA)"
   report "    " "libserialport" "$(read_pin "${LIBPPUC_CFG}" LIBSERIALPORT_SHA)"
   report "    " "yaml-cpp"      "$(read_pin "${LIBPPUC_CFG}" YAML_CPP_SHA)"
else
   echo "    ${C_RED}could not read libppuc platforms/config.sh at ${LIBPPUC_SHA:0:8}${C_OFF}"
   RESOLVE_FAILED=1
fi

report "  " "libsdldmd" "${LIBSDLDMD_SHA}"
if LIBSDLDMD_CFG="$(fetch_config libsdldmd "${LIBSDLDMD_SHA}")"; then
   LIBDMDUTIL_SHA="$(read_pin "${LIBSDLDMD_CFG}" LIBDMDUTIL_SHA)"
   report "    " "libdmdutil" "${LIBDMDUTIL_SHA}"
   if LIBDMDUTIL_CFG="$(fetch_config libdmdutil "${LIBDMDUTIL_SHA}")"; then
      for var in LIBZEDMD_SHA LIBSERUM_SHA LIBVNI_SHA LIBFRAMEUTIL_SHA LIBPUPDMD_SHA SOCKPP_SHA CARGS_SHA; do
         value="$(read_pin "${LIBDMDUTIL_CFG}" "${var}")"
         [ -n "${value}" ] && report "      " "$(echo "${var}" | sed 's/_SHA$//' | tr 'A-Z' 'a-z')" "${value}"
      done
   else
      echo "      ${C_DIM}libdmdutil config.sh not readable; sub-pins not resolved${C_OFF}"
   fi
   report "    " "SDL" "$(read_pin "${LIBSDLDMD_CFG}" SDL_SHA)"
else
   echo "    ${C_RED}could not read libsdldmd platforms/config.sh at ${LIBSDLDMD_SHA:0:8}${C_OFF}"
   RESOLVE_FAILED=1
fi

report "  " "vpinball" "${VPINBALL_SHA}"
report "  " "pinmame"  "${PINMAME_SHA}"

echo
echo "${C_DIM}Third-party pins in ppuc/platforms/config.sh:${C_OFF}"
for var in SDL_IMAGE_SHA SDL_MIXER_SHA FLITE_SHA ESPEAK_NG_SHA LUA_VERSION PINMAME_NVRAM_MAPS_SHA; do
   value="$(read_pin "${PPUC_CONFIG}" "${var}")"
   printf '  %-24s %s\n' "${var}" "${value:-${C_RED}unset${C_OFF}}"
done

# --- firmware version note --------------------------------------------------

IO_BOARDS_DIR="${WORKSPACE}/io-boards"
if [ -f "${IO_BOARDS_DIR}/src/PPUC.h" ]; then
   fw_major="$(grep -Eo 'FIRMWARE_VERSION_MAJOR[[:space:]]+[0-9]+' "${IO_BOARDS_DIR}/src/PPUC.h" | grep -Eo '[0-9]+')"
   fw_minor="$(grep -Eo 'FIRMWARE_VERSION_MINOR[[:space:]]+[0-9]+' "${IO_BOARDS_DIR}/src/PPUC.h" | grep -Eo '[0-9]+')"
   fw_patch="$(grep -Eo 'FIRMWARE_VERSION_PATCH[[:space:]]+[0-9]+' "${IO_BOARDS_DIR}/src/PPUC.h" | grep -Eo '[0-9]+')"
   echo
   echo "${C_DIM}Local io-boards firmware version: ${fw_major}.${fw_minor}.${fw_patch}${C_OFF}"
   echo "${C_DIM}Note: the firmware binary on the boards is versioned independently"
   echo "of the protocol headers this build pins. They can drift.${C_OFF}"
fi

# --- summary ----------------------------------------------------------------

echo
if [ "${RESOLVE_FAILED}" != "0" ]; then
   echo "${C_RED}Chain could not be fully resolved.${C_OFF}"
   exit 2
fi
if [ "${PROBLEMS}" != "0" ]; then
   echo "${C_YELLOW}${PROBLEMS} issue(s) found.${C_OFF}"
   echo "${C_DIM}A pin that is not on main, or a local checkout that differs from"
   echo "its pin, means you are not testing what a normal build produces.${C_OFF}"
   exit 1
fi
echo "${C_GREEN}Pin chain resolved and verified.${C_OFF}"
exit 0
