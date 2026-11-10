#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Synthetic launcher protocol for GUI tests; does not execute Java bytecode."""
import json
import os
from pathlib import Path
import selectors
import signal
import subprocess
import sys

state_path = Path(os.environ["BU_TEST_RUNTIME_STATE"])
log_path = Path(os.environ["BU_TEST_RUNTIME_LOG"])
args = sys.argv[1:]
with log_path.open("a", encoding="utf-8") as stream:
    stream.write(json.dumps(args, ensure_ascii=False) + "\n")

def notify(text):
    print("[ldev] " + text, flush=True)

def value(option):
    return args[args.index(option) + 1]

state = json.loads(state_path.read_text()) if state_path.exists() else {"installed": False}
notify(f"hello protocol=1 pid={os.getpid()}")
if "--list-suites" in args:
    if state.get("bad_list"):
        notify("suite 42\tSynthetic\tTest\t1.0")
        # Deliberately omit the MIDlet and suite-count trailers.
    elif state["installed"]:
        notify("suite 42\tテストアプリ\tGUI fixture\t1.0")
        notify("midlet 42\t1\t一つ目\torg.example.First")
        notify("midlet 42\t2\t二つ目\torg.example.Second")
        notify("midlets 42 2")
        notify("suites 1")
    else:
        notify("suites 0")
elif "--install-only" in args:
    assert Path(args[-1]).is_file()
    state["installed"] = True
    state_path.write_text(json.dumps(state))
    notify("installed suite 42")
elif "--remove-suite" in args:
    assert value("--remove-suite") == "42"
    state["installed"] = False
    state_path.write_text(json.dumps(state))
    notify("removed suite 42")
elif "--suite" in args:
    assert state["installed"] and value("--suite") == "42"
    assert value("--midlet") == "2"
    assert value("--display") == "wayland"
    assert value("--input") == "cmd:-"
    assert value("--app-id") == "org.bananui.J2ME"
    child = subprocess.Popen([os.environ["BU_TEST_RUNTIME_WINDOW"], value("--app-id")])
    selector = selectors.DefaultSelector()
    selector.register(sys.stdin, selectors.EVENT_READ)
    while child.poll() is None:
        for key, _ in selector.select(timeout=0.05):
            line = key.fileobj.readline()
            if not line:
                selector.unregister(key.fileobj)
            elif line.strip() in {"exit", "quit"}:
                child.send_signal(signal.SIGTERM)
    selector.close()
    assert child.returncode == 0
else:
    notify("error unsupported test operation")
    sys.exit(2)
notify("bye status=0")
