#!/bin/bash

set -euo pipefail

usage() {
  cat <<'USAGE'
Usage:
  platforms/linux/install-gnome-autostart.sh [--repo-root PATH] [--autostart-name NAME] [--terminal CMD] -- <ppuc-pinmame args...>

Examples:
  platforms/linux/install-gnome-autostart.sh -- -c examples/t2.yml -n -i
  platforms/linux/install-gnome-autostart.sh --repo-root /opt/ppuc -- -c /opt/ppuc/examples/t2.yml -n -i
  platforms/linux/install-gnome-autostart.sh --terminal kgx -- -c examples/t2.yml -n -i

Notes:
  - The script writes ~/.config/autostart/<name>.desktop for GNOME/XDG autostart.
  - It also writes a small launcher script under ~/.local/share/ppuc/autostart/.
  - The repo build output is expected at <repo-root>/ppuc/ppuc-pinmame.
  - Any relative paths after -- are resolved from <repo-root> before being written.
USAGE
}

repo_root="$(pwd)"
autostart_name="ppuc-pinmame"
terminal_cmd=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --repo-root)
      shift
      [[ $# -gt 0 ]] || { echo "Missing value for --repo-root" >&2; exit 1; }
      repo_root="$1"
      ;;
    --autostart-name)
      shift
      [[ $# -gt 0 ]] || { echo "Missing value for --autostart-name" >&2; exit 1; }
      autostart_name="$1"
      ;;
    --terminal)
      shift
      [[ $# -gt 0 ]] || { echo "Missing value for --terminal" >&2; exit 1; }
      terminal_cmd="$1"
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    --)
      shift
      break
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
  shift
done

if [[ $# -eq 0 ]]; then
  echo "No ppuc-pinmame arguments supplied." >&2
  usage >&2
  exit 1
fi

repo_root="$(realpath "$repo_root")"
workdir="$repo_root/ppuc"
binary="$workdir/ppuc-pinmame"
template="$repo_root/platforms/linux/autostart/ppuc-pinmame.desktop.in"
autostart_dir="${XDG_CONFIG_HOME:-$HOME/.config}/autostart"
desktop_file="$autostart_dir/$autostart_name.desktop"
data_home="${XDG_DATA_HOME:-$HOME/.local/share}"
launcher_dir="$data_home/ppuc/autostart"
launcher_script="$launcher_dir/$autostart_name.sh"

if [[ ! -x "$binary" ]]; then
  echo "Expected executable not found: $binary" >&2
  echo "Build the Linux target first, for example with platforms/linux/aarch64/build.sh" >&2
  exit 1
fi

if [[ ! -f "$template" ]]; then
  echo "Desktop template not found: $template" >&2
  exit 1
fi

resolved_args=()
for arg in "$@"; do
  case "$arg" in
    /*)
      resolved_args+=("$arg")
      ;;
    -*)
      resolved_args+=("$arg")
      ;;
    *)
      if [[ -e "$repo_root/$arg" ]]; then
        resolved_args+=("$(realpath "$repo_root/$arg")")
      else
        resolved_args+=("$arg")
      fi
      ;;
  esac
done

if [[ -z "$terminal_cmd" ]]; then
  if command -v gnome-terminal >/dev/null 2>&1; then
    terminal_cmd="gnome-terminal"
  elif command -v kgx >/dev/null 2>&1; then
    terminal_cmd="kgx"
  elif command -v x-terminal-emulator >/dev/null 2>&1; then
    terminal_cmd="x-terminal-emulator"
  else
    echo "No supported terminal emulator found. Use --terminal <command>." >&2
    exit 1
  fi
fi

mkdir -p "$autostart_dir"
mkdir -p "$launcher_dir"

{
  echo "#!/bin/bash"
  echo "set -euo pipefail"
  echo "cd $(printf '%q' "$workdir")"
  printf 'exec %q' "$binary"
  for arg in "${resolved_args[@]}"; do
    printf ' %q' "$arg"
  done
  printf '\n'
} > "$launcher_script"

chmod 755 "$launcher_script"

case "$terminal_cmd" in
  gnome-terminal)
    printf -v autostart_exec '%q --wait -- %q' "$terminal_cmd" "$launcher_script"
    ;;
  kgx)
    printf -v autostart_exec '%q -- %q' "$terminal_cmd" "$launcher_script"
    ;;
  x-terminal-emulator)
    printf -v autostart_exec '%q -e %q' "$terminal_cmd" "$launcher_script"
    ;;
  *)
    printf -v autostart_exec '%q -e %q' "$terminal_cmd" "$launcher_script"
    ;;
esac

sed \
  -e "s|@AUTOSTART_EXEC@|$autostart_exec|g" \
  "$template" > "$desktop_file"

chmod 644 "$desktop_file"

echo "Installed GNOME autostart entry: $desktop_file"
echo "Launcher: $launcher_script"
echo "Terminal: $terminal_cmd"
echo "Command: $binary ${resolved_args[*]}"
