"""A countdown timer for the Muse Charm.

Usage:
    python3 timer.py --preset 5m
    python3 timer.py --minutes 3 --pause-at 60
    python3 timer.py --seconds 30 --demo

The timer renders one JPEG frame per second into frames/ and prints
each state change as a line of JSON on stdout. A driver (Muse, through
the gadget's display.draw_file command) pushes each frame to the
device. When the timer reaches zero it prints {"state": "done"} and
keeps emitting flashing done frames until stopped; on firmware with
an audio command the driver plays the alarm sound at that point (see
the app README for the current state of sound on the gadget).

Controls are start, pause, resume and stop, driven by the driver:
send SIGINT once to pause or resume, and SIGTERM to stop. The CLI
flags exist so a run can be scripted end to end.
"""

import argparse
import json
import os
import signal
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import render as R

PRESET_SECONDS = {"2m": 120, "3m": 180, "5m": 300, "10m": 600, "15m": 900}

_state = {"paused": False}


def _toggle_pause(signum, frame):
    _state["paused"] = not _state["paused"]


def emit(**event):
    print(json.dumps(event), flush=True)


def main():
    ap = argparse.ArgumentParser(description="Muse Charm countdown timer")
    group = ap.add_mutually_exclusive_group(required=True)
    group.add_argument("--preset", choices=sorted(PRESET_SECONDS))
    group.add_argument("--minutes", type=float)
    group.add_argument("--seconds", type=float)
    ap.add_argument("--frames-dir", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "frames"))
    ap.add_argument("--step", type=float, default=1.0, help="seconds per frame")
    ap.add_argument("--done-frames", type=int, default=6, help="flashing frames to emit after zero")
    args = ap.parse_args()

    if args.preset:
        total = PRESET_SECONDS[args.preset]
    elif args.minutes is not None:
        total = args.minutes * 60
    else:
        total = args.seconds

    os.makedirs(args.frames_dir, exist_ok=True)
    signal.signal(signal.SIGINT, _toggle_pause)

    emit(state="ready", total=total, preset=args.preset)
    path = os.path.join(args.frames_dir, "frame-000000.jpg")
    R.save_frame(R.render(total, total, state="ready", active_preset=args.preset), path)
    emit(state="frame", path=path, remaining=total)

    emit(state="running", total=total)
    remaining = float(total)
    index = 0
    while remaining > 0:
        if _state["paused"]:
            index += 1
            path = os.path.join(args.frames_dir, f"frame-{index:06d}.jpg")
            R.save_frame(R.render(remaining, total, state="paused", active_preset=args.preset), path)
            emit(state="paused", path=path, remaining=remaining)
            while _state["paused"]:
                time.sleep(0.1)
            emit(state="running", total=total)
            continue
        time.sleep(args.step)
        remaining -= args.step
        index += 1
        path = os.path.join(args.frames_dir, f"frame-{index:06d}.jpg")
        R.save_frame(R.render(max(remaining, 0), total, active_preset=args.preset), path)
        emit(state="frame", path=path, remaining=max(remaining, 0))

    emit(state="done", total=total)
    for i in range(args.done_frames):
        path = os.path.join(args.frames_dir, f"frame-done-{i:02d}.jpg")
        R.save_frame(R.render(0, total, state="done", flash=(i % 2 == 1)), path)
        emit(state="done-frame", path=path, flash=(i % 2 == 1))
        time.sleep(0.6)


if __name__ == "__main__":
    main()
