#!/bin/sh
# SPDX-License-Identifier: MIT
# Host-only GUI checks. All files are synthetic, under one temporary directory.
set -eu
if [ "${1:-}" != --inside ]; then exec dbus-run-session -- sh "$0" --inside; fi
source_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${BU_BUILD_DIR:-$source_dir/build}
prefix=${BU_TEST_PREFIX:-$HOME/.local}
shell=${BU_TEST_SHELL:-$prefix/libexec/bananui-shell}
evidence=${BU_TEST_EVIDENCE:-$build_dir/files-gui-evidence}
mkdir -p "$evidence"
task_runtime=$(mktemp -d /tmp/bananui-files-gui.XXXXXX)
app_pid= compositor_pid=
cleanup() {
  [ -z "$app_pid" ] || kill "$app_pid" 2>/dev/null || true
  if [ -n "$compositor_pid" ]; then kill "$compositor_pid" 2>/dev/null || true; wait "$compositor_pid" 2>/dev/null || true; fi
  case "$task_runtime" in /tmp/bananui-files-gui.*) rm -rf -- "$task_runtime" ;; esac
}
trap cleanup EXIT HUP INT TERM
chmod 700 "$task_runtime"
export XDG_RUNTIME_DIR="$task_runtime" XDG_CONFIG_HOME="$task_runtime/config" XDG_DATA_HOME="$task_runtime/data"
export XDG_DATA_DIRS="$task_runtime/data:$prefix/share:/usr/share"
export LD_LIBRARY_PATH="$prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export GSETTINGS_BACKEND=memory WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 LANG=C.UTF-8
export BU_HANDOFF_LOG="$task_runtime/handoff.bin"
export PATH="$task_runtime/bin:$build_dir:$prefix/bin:$PATH"
mkdir -p "$XDG_CONFIG_HOME/bananui" "$XDG_DATA_HOME/applications" "$XDG_DATA_HOME/bananui/scripts/themes" "$task_runtime/bin" "$task_runtime/files/保存先" "$task_runtime/sd"
cp "$source_dir/data/utilities.ipt" "$XDG_DATA_HOME/bananui/scripts/themes/"
cat > "$XDG_CONFIG_HOME/bananui/home.ini" <<'EOF'
[home]
wallpaper-animation=none
left-label=ファイル
left-app=org.bananui.Files.desktop
right-label=アプリ
EOF
cat > "$XDG_DATA_HOME/applications/org.bananui.Files.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=データフォルダ
Exec="$build_dir/bananui-files" --root "$task_runtime/files" --sd-root "$task_runtime/sd"
Icon=system-file-manager
StartupWMClass=org.bananui.Files
EOF
cat > "$task_runtime/phoc.ini" <<'EOF'
[core]
xwayland=false
[output:HEADLESS-1]
mode=480x854
scale=2
EOF
# This is only an argv handoff assertion, not a replacement runtime test.
cat > "$task_runtime/bin/bananui-j2me" <<'EOF'
#!/bin/sh
printf '%s\0' "$@" > "$BU_HANDOFF_LOG"
EOF
chmod +x "$task_runtime/bin/bananui-j2me"
python3 - "$task_runtime/files" <<'PY'
from pathlib import Path
import struct, sys, zlib
root = Path(sys.argv[1])
(root / '01-日本語.txt').write_text('日本語のファイル\n' + '\n'.join(f'{i}: 文章を上下に読めます。' for i in range(40)), encoding='utf-8')
def chunk(name, data):
    return struct.pack('>I', len(data)) + name + data + struct.pack('>I', zlib.crc32(name + data))
width, height = 160, 100
raw = b''.join(b'\0' + bytes(v for x in range(width) for v in (x, y * 2, 160)) for y in range(height))
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')
(root / '02-picture.png').write_bytes(png)
(root / '03-demo ; $(not-a-command).jar').write_bytes(b'synthetic handoff fixture')
PY
phoc -C "$task_runtime/phoc.ini" -E "$shell" > "$evidence/phoc.log" 2>&1 &
compositor_pid=$!
export WAYLAND_DISPLAY=wayland-0
timeout 20 gdbus wait --session org.freedesktop.Notifications
wtype -k F1
timeout 15 gdbus wait --session org.bananui.Files
app_pid=$(gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus --method org.freedesktop.DBus.GetConnectionUnixProcessID org.bananui.Files | sed -n 's/.*uint32 \([0-9]*\).*/\1/p')
sleep 1
shot() { grim -s 1 -o HEADLESS-1 "$evidence/$1.png"; }
repeat_key() { key=$1; count=$2; set --; while [ "$count" -gt 0 ]; do set -- "$@" -k "$key"; count=$((count - 1)); done; wtype "$@"; }
shot roots
wtype -k Return
sleep .4
shot files
wtype -k Down -k Return
sleep .4
shot text
repeat_key Down 5
sleep .2
shot text-scrolled
wtype -k F2 -k F1 -k Down -k Return
sleep .5
repeat_key Right 40
repeat_key BackSpace 40
# The first editor focus starts in Ab; two switches select numeric input.
wtype -k XF86NumericPound -k XF86NumericPound -k 1 -k 2 -k 3 -k F1
sleep .6
test -f "$task_runtime/files/123"
test ! -e "$task_runtime/files/01-日本語.txt"
shot renamed
wtype -k Return
sleep .4
shot image
wtype -k F2 -k Down -k Return
sleep .5
python3 - "$BU_HANDOFF_LOG" "$task_runtime/files/03-demo ; \$(not-a-command).jar" <<'PY'
from pathlib import Path
import sys
assert Path(sys.argv[1]).read_bytes() == sys.argv[2].encode() + b'\0'
PY
shot jar-handoff
wtype -k Down -k F1 -k Down -k Down -k Return
repeat_key Up 3
wtype -k Return -k F1
repeat_key Down 4
wtype -k Return
sleep .4
test -f "$task_runtime/files/保存先/123"
test -f "$task_runtime/files/123"
shot copied
wtype -k F2
repeat_key Down 3
wtype -k F1
repeat_key Down 6
wtype -k Return
sleep .3
shot delete-confirm
wtype -k F2
test -f "$task_runtime/files/123"
wtype -k F1
repeat_key Down 6
wtype -k Return -k Return
sleep .4
test ! -e "$task_runtime/files/123"
test "$(find "$task_runtime/files/.bananui-trash" -type f -name 123 | wc -l)" -eq 1
shot trashed
wtype -k F2 -k Down -k Return
sleep .3
shot sd
rm -- "$BU_HANDOFF_LOG"
"$build_dir/bananui-files" --pick-j2me --root "$task_runtime/files" > "$evidence/picker.log" 2>&1 &
picker_pid=$!
sleep .7
wtype -k Return -k Down -k Down -k Return
wait "$picker_pid"
test -f "$BU_HANDOFF_LOG"
kill -0 "$app_pid"
sleep .3
shot picker-returned
kill -TERM "$app_pid"
app_pid=
printf '%s\n' 'PASS: roots, UTF-8 text scroll, IME rename, image, argv handoff, copy, trash confirmation, SD root, independent J2ME picker.' > "$evidence/success.txt"
cat "$evidence/success.txt"
