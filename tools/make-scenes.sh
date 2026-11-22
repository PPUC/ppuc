#!/bin/bash
#
# make-scenes.sh - cut scene clips out of a video for a scoreboard's window.
#
# The clips come out the size of the scoreboard's scene window (1280x720 by
# default) as H.264 a Raspberry Pi 4 decodes in hardware, so PUP plays them
# unscaled and the Pi spends no CPU on them: the cheapest video the machine
# can show. Use footage you have the rights to.
#
# Usage:
#   tools/make-scenes.sh <video> <out folder> --list <scenes.txt> [options]
#   tools/make-scenes.sh <video> <out folder> --random <count> [options]
#
# --list FILE     one scene per line: name start length
#                   saloon      0:12:31.5   6
#                   showdown    1:02:07     8.5
#                 times as seconds, M:SS or H:MM:SS, with decimals; # comments
# --random N      N clips from random places, none overlapping
# --length S      length of a random clip in seconds (default 6)
# --skip-start S  leave the first S seconds out of random picks (default 120:
#                 titles and logos)
# --skip-end S    and the last S seconds (default 300: credits)
# --seed N        the same seed picks the same clips again (default 1)
# --size WxH      the scoreboard window (default 1280x720). The picture is
#                 scaled to cover it and cut to fit, never stretched.
# --fps N         frame rate (default 30)
# --audio         keep the sound (AAC). Off by default: the game has its own.
# --crf N         quality, lower is better and bigger (default 21)
# --prefix TEXT   file name prefix for random clips (default "scene")
# --no-crop       keep black bars that are part of the picture. By default
#                 they are found (a wide film inside a 16:9 file) and cut off
#                 first, so the scene fills the window.
#
# Clips are named <name>.mp4. Existing ones are left alone, so a list can grow
# and be run again; delete a clip to make it again.

set -u

usage() {
   sed -n '3,36p' "$0" | sed 's/^# \{0,1\}//'
   exit 2
}

die() {
   echo "make-scenes: $*" >&2
   exit 1
}

[ $# -ge 2 ] || usage
INPUT="$1"
OUT="$2"
shift 2

LIST=""
RANDOM_COUNT=0
LENGTH=6
SKIP_START=120
SKIP_END=300
SEED=1
SIZE="1280x720"
FPS=30
AUDIO=0
CRF=21
PREFIX="scene"
CROP=1

while [ $# -gt 0 ]; do
   case "$1" in
      --list) LIST="${2:-}"; shift 2 ;;
      --random) RANDOM_COUNT="${2:-}"; shift 2 ;;
      --length) LENGTH="${2:-}"; shift 2 ;;
      --skip-start) SKIP_START="${2:-}"; shift 2 ;;
      --skip-end) SKIP_END="${2:-}"; shift 2 ;;
      --seed) SEED="${2:-}"; shift 2 ;;
      --size) SIZE="${2:-}"; shift 2 ;;
      --fps) FPS="${2:-}"; shift 2 ;;
      --audio) AUDIO=1; shift ;;
      --crf) CRF="${2:-}"; shift 2 ;;
      --prefix) PREFIX="${2:-}"; shift 2 ;;
      --no-crop) CROP=0; shift ;;
      -h|--help) usage ;;
      *) die "unknown option $1" ;;
   esac
done

command -v ffmpeg >/dev/null || die "ffmpeg is not installed"
command -v ffprobe >/dev/null || die "ffprobe is not installed"
[ -f "${INPUT}" ] || die "no such video: ${INPUT}"
if [ -z "${LIST}" ] && [ "${RANDOM_COUNT}" -le 0 ] 2>/dev/null; then
   die "give --list FILE or --random N"
fi
case "${SIZE}" in
   *x*) W="${SIZE%x*}"; H="${SIZE#*x}" ;;
   *) die "--size must be WxH, like 1280x720" ;;
esac
# H.264 wants even sizes.
[ $((W % 2)) -eq 0 ] && [ $((H % 2)) -eq 0 ] || die "--size must be even in both directions"

mkdir -p "${OUT}" || die "cannot create ${OUT}"

# "1:02:07.5" or "62" -> seconds.
to_seconds() {
   echo "$1" | awk -F: '{ s = 0; for (i = 1; i <= NF; i++) s = s * 60 + $i; printf "%.3f", s }'
}

DURATION="$(ffprobe -v error -show_entries format=duration -of default=nw=1:nk=1 "${INPUT}")"
[ -n "${DURATION}" ] || die "cannot read the length of ${INPUT}"

# Black bars burnt into the picture: what cropdetect finds most often in 30 s
# from the middle of the video, where there are no titles. Even numbers, as
# the encoder wants.
CROP_FILTER=""
if [ "${CROP}" -eq 1 ]; then
   middle="$(awk -v d="${DURATION}" 'BEGIN { m = d / 2 - 15; if (m < 0) m = 0; printf "%.1f", m }')"
   detected="$(ffmpeg -hide_banner -nostdin -ss "${middle}" -t 30 -i "${INPUT}" -an \
                  -vf "fps=2,cropdetect=limit=24:round=2:reset=0" -f null - 2>&1 |
               grep -o 'crop=[0-9:]*' | sort | uniq -c | sort -rn | awk 'NR == 1 { print $2 }')"
   size="$(ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=p=0:s=x "${INPUT}")"
   case "${detected}" in
      crop=*)
         cw="$(echo "${detected}" | cut -d= -f2 | cut -d: -f1)"
         ch="$(echo "${detected}" | cut -d: -f2)"
         if [ "${cw}x${ch}" != "${size}" ]; then
            CROP_FILTER="${detected},"
            echo "make-scenes: black bars in the picture; using ${cw}x${ch} of ${size} (--no-crop keeps them)"
         fi
         ;;
   esac
fi

made=0
kept=0

# Cover the window and crop: a 2.35:1 film loses its sides rather than gaining
# black bars inside the frame. High profile, level 4.0: well within what the
# Pi 4's decoder does in hardware. A keyframe every second keeps a jump into
# a clip quick; +faststart lets PUP start playing before reading it all.
encode() {
   local name="$1" start="$2" length="$3"
   local file="${OUT}/${name}.mp4"
   if [ -f "${file}" ]; then
      kept=$((kept + 1))
      return
   fi
   local audio_args=(-an)
   if [ "${AUDIO}" -eq 1 ]; then
      audio_args=(-c:a aac -b:a 128k -ac 2)
   fi
   printf '%-24s from %8.1fs, %4.1fs\n' "${name}" "${start}" "${length}"
   if ! ffmpeg -hide_banner -loglevel error -nostdin -y \
         -ss "${start}" -t "${length}" -i "${INPUT}" \
         -vf "${CROP_FILTER}scale=${W}:${H}:force_original_aspect_ratio=increase:flags=lanczos,crop=${W}:${H},fps=${FPS},format=yuv420p" \
         -c:v libx264 -preset slow -crf "${CRF}" -profile:v high -level:v 4.0 \
         -g "${FPS}" -keyint_min "${FPS}" -sc_threshold 0 \
         "${audio_args[@]}" -movflags +faststart \
         "${file}.part.mp4"; then
      rm -f "${file}.part.mp4"
      die "ffmpeg failed on ${name}"
   fi
   mv "${file}.part.mp4" "${file}"
   made=$((made + 1))
}

if [ -n "${LIST}" ]; then
   [ -f "${LIST}" ] || die "no such list: ${LIST}"
   line_no=0
   while IFS= read -r line || [ -n "${line}" ]; do
      line_no=$((line_no + 1))
      line="${line%%#*}"
      set -- ${line}
      [ $# -eq 0 ] && continue
      [ $# -eq 3 ] || die "${LIST} line ${line_no}: expected 'name start length'"
      case "$1" in
         *[!A-Za-z0-9_-]*) die "${LIST} line ${line_no}: '$1' - use letters, digits, _ and - in names" ;;
      esac
      start="$(to_seconds "$2")"
      length="$(to_seconds "$3")"
      awk -v s="${start}" -v l="${length}" -v d="${DURATION}" 'BEGIN { exit !(l > 0 && s >= 0 && s + l <= d + 0.01) }' ||
         die "${LIST} line ${line_no}: ${2} + ${3} is outside the video (${DURATION%.*} s)"
      encode "$1" "${start}" "${length}"
   done <"${LIST}"
fi

if [ "${RANDOM_COUNT}" -gt 0 ] 2>/dev/null; then
   # Random starts on a grid of clip-sized slots, so no two clips overlap and
   # the same seed always gives the same clips.
   picks="$(awk -v n="${RANDOM_COUNT}" -v len="${LENGTH}" -v a="${SKIP_START}" -v d="${DURATION}" \
                -v b="${SKIP_END}" -v seed="${SEED}" 'BEGIN {
      first = a; last = d - b - len
      if (last < first) { first = 0; last = d - len }
      slots = int((last - first) / len)
      if (slots < 1) exit 1
      if (n > slots) n = slots
      srand(seed)
      for (i = 0; i < slots; i++) order[i] = i
      for (i = slots - 1; i > 0; i--) { j = int(rand() * (i + 1)); t = order[i]; order[i] = order[j]; order[j] = t }
      for (i = 0; i < n; i++) chosen[i] = order[i]
      # In film order, so scene01 comes before scene02 in the story.
      for (i = 0; i < n; i++) for (j = i + 1; j < n; j++) if (chosen[j] < chosen[i]) { t = chosen[i]; chosen[i] = chosen[j]; chosen[j] = t }
      for (i = 0; i < n; i++) printf "%.3f\n", first + chosen[i] * len
   }')" || die "the video is too short for clips of ${LENGTH} s"
   i=0
   for start in ${picks}; do
      i=$((i + 1))
      encode "$(printf '%s%02d' "${PREFIX}" "${i}")" "${start}" "${LENGTH}"
   done
fi

echo "make-scenes: ${made} made, ${kept} already there, in ${OUT} (${W}x${H}, ${FPS} fps$([ "${AUDIO}" -eq 1 ] && echo ", with sound"))"
