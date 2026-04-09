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
  - It only removes the per-user GNOME/XDG autostart entry and leaves the build output untouched.
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

if [[ ! -e "$desktop_file" ]]; then
  echo "Autostart entry not found: $desktop_file"
  exit 0
fi

rm -f "$desktop_file"
echo "Removed GNOME autostart entry: $desktop_file"
