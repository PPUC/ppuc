#!/bin/bash

set -euo pipefail

usage() {
  cat <<'USAGE'
Usage:
  platforms/linux/uninstall-gnome-autostart.sh [--autostart-name NAME]

Examples:
  platforms/linux/uninstall-gnome-autostart.sh
  platforms/linux/uninstall-gnome-autostart.sh --autostart-name ppuc-pinmame

Notes:
  - The script removes ~/.config/autostart/<name>.desktop.
  - It also removes the generated launcher script under ~/.local/share/ppuc/autostart/.
  - It only removes per-user autostart artifacts and leaves the build output untouched.
USAGE
}

autostart_name="ppuc-pinmame"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --autostart-name)
      shift
      [[ $# -gt 0 ]] || { echo "Missing value for --autostart-name" >&2; exit 1; }
      autostart_name="$1"
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
  shift
done

autostart_dir="${XDG_CONFIG_HOME:-$HOME/.config}/autostart"
desktop_file="$autostart_dir/$autostart_name.desktop"
data_home="${XDG_DATA_HOME:-$HOME/.local/share}"
launcher_script="$data_home/ppuc/autostart/$autostart_name.sh"

removed_any=0

if [[ -e "$desktop_file" ]]; then
  rm -f "$desktop_file"
  echo "Removed GNOME autostart entry: $desktop_file"
  removed_any=1
fi

if [[ -e "$launcher_script" ]]; then
  rm -f "$launcher_script"
  echo "Removed launcher script: $launcher_script"
  removed_any=1
fi

if [[ "$removed_any" -eq 0 ]]; then
  echo "Autostart entry not found for name: $autostart_name"
fi
