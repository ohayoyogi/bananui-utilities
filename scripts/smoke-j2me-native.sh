#!/bin/sh
# SPDX-License-Identifier: MIT
# Real phoneME SizeMIDlet integration; all installed data is temporary.
set -eu
if [ "${1:-}" != --inside ]; then exec dbus-run-session -- sh "$0" --inside; fi
source_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${BU_BUILD_DIR:-$source_dir/build}
prefix=${BU_TEST_PREFIX:-$HOME/.local}
native=${BU_MICROMIDP_RUNTIME:?set directory containing micromidp-linuxdev and lib}
evidence=$build_dir/j2me-real-gui-evidence
mkdir -p "$evidence"
task_runtime=$(mktemp -d /tmp/bananui-real-gui.XXXXXX)
manager_pid= compositor_pid=
cleanup() {
  [ -z "$manager_pid" ] || kill "$manager_pid" 2>/dev/null || true
  if [ -n "$compositor_pid" ]; then kill "$compositor_pid" 2>/dev/null || true; wait "$compositor_pid" 2>/dev/null || true; fi
  case "$task_runtime" in /tmp/bananui-real-gui.*) rm -rf -- "$task_runtime" ;; esac
}
trap cleanup EXIT HUP INT TERM
chmod 700 "$task_runtime"
export XDG_RUNTIME_DIR="$task_runtime" XDG_CONFIG_HOME="$task_runtime/config" XDG_DATA_HOME="$task_runtime/data" XDG_CACHE_HOME="$task_runtime/cache"
export XDG_DATA_DIRS="$task_runtime/data:$prefix/share:/usr/share" LD_LIBRARY_PATH="$prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PATH="$build_dir:$prefix/bin:$PATH" GSETTINGS_BACKEND=memory LANG=C.UTF-8
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1
mkdir -p "$XDG_CONFIG_HOME/bananui" "$XDG_CONFIG_HOME/bananui-utils" "$XDG_DATA_HOME/applications" "$XDG_DATA_HOME/bananui/scripts/themes" "$XDG_DATA_HOME/bananui-utils/files"
cp "$source_dir/data/utilities.ipt" "$XDG_DATA_HOME/bananui/scripts/themes/"
cp "${BU_MICROMIDP_SIZE_JAR:?set the real SizeMIDlet sample JAR}" "$XDG_DATA_HOME/bananui-utils/files/size.jar"
cp "${BU_MICROMIDP_KEYMAP:?set the runtime PC keyboard keymap}" "$task_runtime/pc.keymap"
cat > "$XDG_CONFIG_HOME/bananui-utils/runtime.ini" <<EOF
[runtime]
executable=$native/micromidp-linuxdev
lib-dir=$native/lib
keymap=$task_runtime/pc.keymap
EOF
cat > "$XDG_CONFIG_HOME/bananui/home.ini" <<'EOF'
[home]
wallpaper-animation=none
left-label=J2MEアプリ
left-app=org.bananui.J2ME.desktop
EOF
cp "$source_dir/data/org.bananui.J2ME.desktop" "$XDG_DATA_HOME/applications/"
cat > "$task_runtime/phoc.ini" <<'EOF'
[core]
xwayland=false
[output:HEADLESS-1]
mode=480x854
scale=2
EOF
phoc -C "$task_runtime/phoc.ini" -E "$prefix/libexec/bananui-shell" > "$evidence/phoc.log" 2>&1 &
compositor_pid=$!
export WAYLAND_DISPLAY=wayland-0
timeout 20 gdbus wait --session org.freedesktop.Notifications
pid_for() { gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus --method org.freedesktop.DBus.GetConnectionUnixProcessID "$1" | sed -n 's/.*uint32 \([0-9]*\).*/\1/p'; }
shot() { grim -s 1 -o HEADLESS-1 "$evidence/$1.png"; }
wtype -k F1
timeout 15 gdbus wait --session org.bananui.J2ME
manager_pid=$(pid_for org.bananui.J2ME)
sleep .7
shot empty
wtype -k F1
sleep .8
wtype -k Return
sleep .3
wtype -k Return
sleep .7
shot import-confirm
wtype -k F1
# Wait for the installer and following list refresh, then independently inspect the store.
sleep 4
"$native/micromidp-linuxdev" --lib "$native/lib" --data "$XDG_DATA_HOME/bananui-utils/j2me" --cache "$XDG_CACHE_HOME/bananui-utils/j2me" --list-suites > "$evidence/installed-list.txt" 2>&1
rg -q 'midlet [0-9]+\s+1\s+Size\s+SizeMIDlet' "$evidence/installed-list.txt"
shot installed
wtype -k Return -k F1
sleep 2
rg -q '\[SIZE\] mode=0 Canvas=' "$evidence/phoc.log"
shot running
# Real physical-key path through Wayland, with the PC keymap mapping Escape to END.
wtype -k 1 -k Up -k Return
sleep .3
wtype -k Escape
sleep 2
shot returned
kill -0 "$manager_pid"
wtype -k Return -k F2
sleep .3
shot remove-confirm
wtype -k F1
sleep 1
"$native/micromidp-linuxdev" --lib "$native/lib" --data "$XDG_DATA_HOME/bananui-utils/j2me" --cache "$XDG_CACHE_HOME/bananui-utils/j2me" --list-suites > "$evidence/removed-list.txt" 2>&1
rg -q '^\[ldev\] suites 0$' "$evidence/removed-list.txt"
shot removed
printf '%s\n' 'PASS: actual phoneME runtime manager GUI: picker import, native suite list, SizeMIDlet Wayland rendering, key-driven exit/manager return, stable-ID removal.' > "$evidence/success.txt"
cat "$evidence/success.txt"
