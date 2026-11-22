#!/usr/bin/env python3
# The DMA routing data under tools/kb/data/dma/: load it, find a device or target, export one JSON bundle for the
# configurator site (dma-data.json). The files themselves are written by hand from the reference manuals; see
# tools/kb/data/dma/README.md for their sources.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""Read the DMA routing data (STM32 devices, ESP32 targets) and export it as one JSON bundle.

  python3 tools/kb/dmadata.py --export-json dma-data.json     the bundle the configurator fetches
  python3 tools/kb/dmadata.py --list                          the devices and targets found
"""

import argparse
import os
import sys

if __package__ in (None, ""):  # run as a script: make `kb` importable
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    from kb import yamlio  # noqa: E402
    from kb.emit import canonical_json  # noqa: E402
else:
    from . import yamlio
    from .emit import canonical_json

SCHEMA = 1
DATA_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data", "dma")


def _yml_files(folder):
    if not os.path.isdir(folder):
        return []
    return sorted(os.path.join(folder, n) for n in os.listdir(folder) if n.endswith((".yml", ".yaml")))


def load_dir(root=None):
    """The whole data set: {"schema": 1, "stm32": {device: facts}, "esp32": {target: facts}}.

    STM32: one file per device under `stm32/`, the key is the file's `device:` (upper case). ESP32: `esp32/targets.yml`
    maps each target to its facts (directly, or below a `targets:` key; a `common:` map beside it is merged into each
    target, the target's own value winning)."""
    root = root or DATA_DIR
    stm32 = {}
    for path in _yml_files(os.path.join(root, "stm32")):
        doc = yamlio.load(path)
        if not isinstance(doc, dict) or not isinstance(doc.get("device"), str):
            raise yamlio.InputError(f"{path}: a device file has a `device:` name")
        key = doc["device"].upper()
        if key in stm32:
            raise yamlio.InputError(f"{path}: device {key} is defined twice")
        stm32[key] = doc
    esp32 = {}
    for path in _yml_files(os.path.join(root, "esp32")):
        doc = yamlio.load(path)
        common = {}
        if isinstance(doc, dict) and isinstance(doc.get("targets"), dict):
            common = doc.get("common") or {}
            doc = doc["targets"]
        if not isinstance(doc, dict):
            raise yamlio.InputError(f"{path}: expected a mapping of targets")
        for name, facts in doc.items():
            if isinstance(facts, dict):
                esp32[str(name).lower()] = {**common, **facts}  # a target's own value wins over `common`
    return {"schema": SCHEMA, "stm32": stm32, "esp32": esp32}


def find_device(data, device):
    """The facts of the STM32 device named by `device` (`STM32H747XI` finds `STM32H747`: the longest data name that
    is a prefix of the part name), or None."""
    name = str(device).upper()
    best = None
    for key in data.get("stm32", {}):
        if name.startswith(key) and (best is None or len(key) > len(best)):
            best = key
    return data["stm32"][best] if best else None


def find_target(data, target):
    """The facts of the ESP32 target `target` (case-insensitive), or None."""
    return data.get("esp32", {}).get(str(target).lower())


def export_json(data):
    """The bundle as canonical JSON text (sorted keys, two-space indent, final newline)."""
    return canonical_json(data)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data-dir", default=None, help="the data folder (default: tools/kb/data/dma)")
    ap.add_argument("--export-json", metavar="OUT", help="write the whole data set as one JSON file")
    ap.add_argument("--list", action="store_true", help="list the devices and targets")
    args = ap.parse_args(argv)
    try:
        data = load_dir(args.data_dir)
    except yamlio.InputError as exc:
        print(f"kb: {exc}", file=sys.stderr)
        return 2
    if args.export_json:
        with open(args.export_json, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(export_json(data))
        print(f"dmadata: {len(data['stm32'])} STM32 device(s), {len(data['esp32'])} ESP32 target(s): "
              f"{args.export_json}")
    if args.list or not args.export_json:
        for name in sorted(data["stm32"]):
            print(f"stm32 {name}")
        for name in sorted(data["esp32"]):
            print(f"esp32 {name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
