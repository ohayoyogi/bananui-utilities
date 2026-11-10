#!/bin/sh
# SPDX-License-Identifier: MIT
# UI integration with an explicitly synthetic runtime. Real VM tests are separate.
set -eu
if [ "${1:-}" != --inside ]; then exec dbus-run-session -- sh "$0" --inside; fi
source_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${BU_BUILD_DIR:-$source_dir/build}
prefix=${BU_TEST_PREFIX:-$HOME/.local}
shell=${BU_TEST_SHELL:-$prefix/libexec/bananui-shell}
evidence=${BU_TEST_EVIDENCE:-$build_dir/j2me-gui-evidence}
mkdir -p "$evidence"
task_runtime=$(mktemp -d /tmp/bananui-j2me-gui.XXXXXX)
manager_pid= files_pid= compositor_pid=
cleanup() {
  [ -z "$manager_pid" ] || kill "$manager_pid" 2>/dev/null || true
  [ -z "$files_pid" ] || kill "$files_pid" 2>/dev/null || true
  if [ -n "$compositor_pid" ]; then kill "$compositor_pid" 2>/dev/null || true; wait "$compositor_pid" 2>/dev/null || true; fi
  case "$task_runtime" in /tmp/bananui-j2me-gui.*) rm -rf -- "$task_runtime" ;; esac
}
trap cleanup EXIT HUP INT TERM
chmod 700 "$task_runtime"
export XDG_RUNTIME_DIR="$task_runtime" XDG_CONFIG_HOME="$task_runtime/config" XDG_DATA_HOME="$task_runtime/data" XDG_CACHE_HOME="$task_runtime/cache"
export XDG_DATA_DIRS="$task_runtime/data:$prefix/share:/usr/share" LD_LIBRARY_PATH="$prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PATH="$build_dir:$prefix/bin:$PATH" GSETTINGS_BACKEND=memory LANG=C.UTF-8
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1
export BU_TEST_RUNTIME_STATE="$task_runtime/state.json" BU_TEST_RUNTIME_LOG="$task_runtime/argv.jsonl" BU_TEST_RUNTIME_WINDOW="$build_dir/test-runtime-window"
mkdir -p "$XDG_CONFIG_HOME/bananui" "$XDG_CONFIG_HOME/bananui-utils" "$XDG_DATA_HOME/applications" "$XDG_DATA_HOME/bananui/scripts/themes" "$XDG_DATA_HOME/bananui-utils/files" "$task_runtime/lib"
cp "$source_dir/data/utilities.ipt" "$XDG_DATA_HOME/bananui/scripts/themes/"
cp "$source_dir/tests/fixtures/j2me-runtime.py" "$task_runtime/runtime"
chmod +x "$task_runtime/runtime"
printf '%s\n' synthetic > "$XDG_DATA_HOME/bananui-utils/files/test app.jar"
cat > "$XDG_CONFIG_HOME/bananui-utils/runtime.ini" <<EOF
[runtime]
executable=$task_runtime/runtime
lib-dir=$task_runtime/lib
EOF
cat > "$XDG_CONFIG_HOME/bananui/home.ini" <<'EOF'
[home]
wallpaper-animation=none
left-label=J2MEアプリ
left-app=org.bananui.J2ME.desktop
right-label=データフォルダ
right-app=org.bananui.Files.desktop
EOF
for name in J2ME Files Calculator; do
  case "$name" in J2ME) binary=bananui-j2me ;; Files) binary=bananui-files ;; Calculator) binary=bananui-calculator ;; esac
  cat > "$XDG_DATA_HOME/applications/org.bananui.$name.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=$name
Exec="$build_dir/$binary" %U
StartupWMClass=org.bananui.$name
EOF
done
cat > "$task_runtime/phoc.ini" <<'EOF'
[core]
xwayland=false
[output:HEADLESS-1]
mode=480x854
scale=2
EOF
phoc -C "$task_runtime/phoc.ini" -E "$shell" > "$evidence/phoc.log" 2>&1 &
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
test ! -f "$BU_TEST_RUNTIME_STATE"
# Cancelling must discard this path so F1 opens a new picker next time.
wtype -k F2 -k F1
sleep .6
wtype -k Return
sleep .3
wtype -k Return
sleep .6
wtype -k F1
sleep .8
python3 - "$BU_TEST_RUNTIME_STATE" <<'PY'
import json, sys
assert json.load(open(sys.argv[1]))['installed']
PY
shot installed
wtype -k Return -k Down -k F1
timeout 15 gdbus wait --session org.bananui.J2METestRuntime
sleep .4
wtype -k 1 -k 2 -k Up -k 3 -k Return
sleep .2
shot running
wtype -k F2
sleep .9
shot returned
python3 - "$BU_TEST_RUNTIME_LOG" <<'PY'
import json, sys
commands=[json.loads(line) for line in open(sys.argv[1])]
run=next(args for args in commands if '--suite' in args)
assert run[run.index('--suite')+1]=='42'
assert run[run.index('--midlet')+1]=='2'
PY
wtype -k Return -k F2
sleep .3
shot remove-confirm
wtype -k F2
python3 - "$BU_TEST_RUNTIME_STATE" <<'PY'
import json, sys
assert json.load(open(sys.argv[1]))['installed']
PY
wtype -k F2 -k F1
sleep .8
python3 - "$BU_TEST_RUNTIME_STATE" <<'PY'
import json, sys
assert not json.load(open(sys.argv[1]))['installed']
PY
shot removed
# A second open request must make an existing manager visible above normal Files.
bananui-files > "$evidence/files.log" 2>&1 &
files_pid=$!
timeout 10 gdbus wait --session org.bananui.Files
sleep .6
wtype -k Return
sleep .3
wtype -k Return
sleep .7
shot normal-files-handoff
wtype -k F1
sleep .7
python3 - "$BU_TEST_RUNTIME_STATE" <<'PY'
import json, sys
assert json.load(open(sys.argv[1]))['installed']
PY
kill -TERM "$manager_pid"
manager_pid=
kill -TERM "$files_pid" 2>/dev/null || true
files_pid=
cp "$BU_TEST_RUNTIME_LOG" "$evidence/argv.jsonl"
printf '%s\n' 'PASS: synthetic-runtime GUI: empty list, picker import/cancel/reopen, multi-MIDlet launch argv, foreground app return, remove/cancel, existing manager activation.' > "$evidence/success.txt"
cat "$evidence/success.txt"
