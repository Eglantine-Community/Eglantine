#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Save or inspect the opt-in emulated-GPU terrain resource history."""

import argparse
import json
import os
import time
from collections import Counter
from pathlib import Path


def inspect(path):
    with path.open(encoding="utf-8") as source:
        rows = [json.loads(line) for line in source if line.strip()]
    if not rows or rows[0].get("kind") != "trace":
        raise ValueError(f"{path} is not a terrain resource trace")
    kinds = Counter(row.get("kind") for row in rows[1:])
    frames = [row["frame"] for row in rows[1:]]
    result = {
        "file": str(path),
        "header": rows[0],
        "frames": [min(frames), max(frames)] if frames else [],
        "kinds": dict(kinds),
        "image_events": dict(Counter(row["event"] for row in rows
                                     if row.get("kind") == "image")),
        "buffer_events": dict(Counter(row["event"] for row in rows
                                      if row.get("kind") == "buffer-event")),
    }
    print(json.dumps(result, indent=2))


def checkpoint(directory, state, timeout):
    if not directory.is_dir():
        raise ValueError(f"Enable terrain tracing first; directory does not exist: {directory}")
    output = directory / ("clean.jsonl" if state == "clean" else "history.jsonl")
    trigger = directory / ("clean.trigger" if state == "clean" else "dump.trigger")
    before = output.stat().st_mtime_ns if output.exists() else None
    trigger.touch()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if (not trigger.exists() and output.exists()
                and output.stat().st_mtime_ns != before):
            inspect(output)
            return
        time.sleep(0.1)
    raise TimeoutError("Recorder did not acknowledge the checkpoint; ensure tracing is enabled "
                       "and the emulated GPU is presenting frames. The trigger remains pending.")


def main():
    default = Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local/share")) / "eglantine/log/terrain-trace"
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    save = commands.add_parser("checkpoint")
    save.add_argument("state", choices=("clean", "bad"))
    save.add_argument("--directory", type=Path, default=default)
    save.add_argument("--timeout", type=float, default=15)
    view = commands.add_parser("inspect")
    view.add_argument("file", type=Path)
    args = parser.parse_args()
    try:
        if args.command == "inspect":
            inspect(args.file)
        else:
            checkpoint(args.directory, args.state, args.timeout)
    except (OSError, ValueError, TimeoutError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
