#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""The port SDK: what a port needs from kb-core, as one release asset (docs/port-sdk.md).

  port_sdk.py build  --out DIR [--tag vX.Y.Z[-rc.N] | --version X.Y.Z[-rc.N]] [--root DIR]
      writes DIR/kb-port-sdk-<version>.tar.gz and DIR/kb-port-sdk-<version>.manifest.json
  port_sdk.py verify ARCHIVE [--manifest FILE]
      checks an archive: safe member names, only regular files, every file equal to the manifest inside it, and
      (with --manifest) the archive's own SHA-256 equal to the one the release manifest states

The file list is `vendor` in tools/layout/paths.yml (the same list a port copies today, and that `kb_layout_unit`
proves configures), plus a slice of catalog/compat.yml (the version and the ports, without the driver list). The
archive is reproducible: sorted members, owner 0, time 0, gzip without a name or time.

Exit code 0: done or verified; 1: a check failed; 2: usage.
"""
from __future__ import annotations

import argparse
import gzip
import hashlib
import io
import json
import os
import re
import subprocess
import sys
import tarfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "layout"))
import layout as _layout  # noqa: E402  (tools/layout/layout.py: the one reader of paths.yml)

NAME = "kb-port-sdk"
SCHEMA = "kb-port-sdk/1"
INNER_MANIFEST = "SDK-MANIFEST.json"
COMPAT_SLICE = "catalog/compat.yml"
VERSION_RE = re.compile(r"^\d+\.\d+\.\d+(-rc\.\d+)?$")
# The suites and models a port runs, named by the layout entry they live under (path below the entry).
SUITES = {
    "dma_conformance": "{lib}/dma/host/inc/kb/dma/host/conformance.hpp",
    "dma_host_model": "{lib}/dma/host/inc/kb/dma/host",
    "interfaces_host_fakes": "{lib}/interfaces/host/inc/kb/interfaces/host",
    "host_port": "{host_port}",
    "test_harness": "{tests}/harness",
}


class SdkError(Exception):
    pass


def sha256_of(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def git(root: str, *args: str) -> str:
    return subprocess.run(["git", "-C", root, *args], check=True, capture_output=True, text=True).stdout.strip()


def version_from_cmake(root: str) -> str:
    text = open(os.path.join(root, "cmake", "kb_version.cmake"), encoding="utf-8").read()
    parts = [re.search(rf"set\(KB_VERSION_{p} (\d+)\)", text) for p in ("MAJOR", "MINOR", "PATCH")]
    if not all(parts):
        raise SdkError("cmake/kb_version.cmake: cannot read KB_VERSION_MAJOR/MINOR/PATCH")
    return ".".join(p.group(1) for p in parts)


def resolve_version(root: str, tag: str | None, version: str | None) -> str:
    if tag and version:
        raise SdkError("give --tag or --version, not both")
    v = (tag or "").removeprefix("v") or version or ""
    if not VERSION_RE.match(v):
        raise SdkError(f"version {v!r} is not X.Y.Z or X.Y.Z-rc.N")
    # A tag is cut from the commit whose cmake/kb_version.cmake carries the same X.Y.Z: the version a port reads in
    # find_package(kb) and the asset name must not disagree.
    if tag:
        core = version_from_cmake(root)
        if v.split("-")[0] != core:
            raise SdkError(f"tag {tag} but cmake/kb_version.cmake says {core}; bump it in the release pull request")
    return v


def expand(root: str, data: dict) -> list[str]:
    """Every file under the vendor list: tracked files only (a build folder never gets in)."""
    entries = _layout.vendor_paths(data)
    files = set()
    all_files = _layout.tracked(root, untracked=True)
    for entry in entries:
        entry = entry.rstrip("/")
        hit = [p for p in all_files if p == entry or p.startswith(entry + "/")]
        if not hit:
            raise SdkError(f"vendor entry {entry} matches no file")
        files.update(hit)
    return sorted(files)


def compat_slice(root: str) -> bytes:
    """catalog/compat.yml without the driver list: the SDK's reader needs the version and the ports."""
    text = open(os.path.join(root, COMPAT_SLICE), encoding="utf-8").read()
    kept = ["# Slice of catalog/compat.yml for ports (kb-port-sdk): the kb-core version and the ports, no drivers."]
    skip = False
    for line in text.splitlines():
        if line.startswith("#"):
            continue
        m = re.match(r"^([A-Za-z_]+):", line)
        if m:
            skip = m.group(1) == "drivers"
        if not skip and line.strip():
            kept.append(line)
    return ("\n".join(kept) + "\n").encode()


def source_files(root: str, data: dict) -> dict[str, tuple[bytes, int]]:
    """path -> (bytes, mode) of everything the archive holds, except the inner manifest."""
    out: dict[str, tuple[bytes, int]] = {}
    for rel in expand(root, data):
        full = os.path.join(root, rel)
        if os.path.islink(full) or not os.path.isfile(full):
            raise SdkError(f"{rel} is not a regular file")
        mode = 0o755 if os.access(full, os.X_OK) else 0o644
        out[rel] = (open(full, "rb").read(), mode)
    out[COMPAT_SLICE] = (compat_slice(root), 0o644)
    return out


def manifest_for(root: str, data: dict, version: str, files: dict[str, tuple[bytes, int]]) -> dict:
    lay = data["layout"]
    parts = sorted(p for p in ("utils", "rtos", "dma", "interfaces") if any(
        f.startswith(f"{lay['lib']}/{p}/") for f in files))
    try:
        commit = git(root, "rev-parse", "HEAD")
    except (subprocess.CalledProcessError, FileNotFoundError):
        commit = None
    return {
        "schema": SCHEMA,
        "name": NAME,
        "version": version,
        "kb_core_version": version_from_cmake(root),
        "commit": commit,
        "license": "MIT",
        "parts": parts,
        "layout": dict(sorted(lay.items())),
        "suites": {k: re.sub(r"\{([A-Za-z_]+)\}", lambda m: lay[m.group(1)], v) for k, v in SUITES.items()},
        "files": [{"path": p, "sha256": sha256_of(b), "size": len(b)} for p, (b, _m) in sorted(files.items())],
    }


def write_archive(path: str, prefix: str, files: dict[str, tuple[bytes, int]]) -> None:
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode="w", format=tarfile.PAX_FORMAT) as tar:
        for rel in sorted(files):
            body, mode = files[rel]
            info = tarfile.TarInfo(prefix + rel)
            info.size, info.mode, info.mtime = len(body), mode, 0
            info.uid = info.gid = 0
            info.uname = info.gname = ""
            tar.addfile(info, io.BytesIO(body))
    with open(path, "wb") as f, gzip.GzipFile(filename="", mode="wb", fileobj=f, mtime=0) as gz:
        gz.write(raw.getvalue())


def build(root: str, out: str, tag: str | None, version: str | None) -> tuple[str, str]:
    data = _layout.load(os.path.join(root, "tools", "layout", "paths.yml"))
    v = resolve_version(root, tag, version)
    files = source_files(root, data)
    manifest = manifest_for(root, data, v, files)
    files[INNER_MANIFEST] = ((json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode(), 0o644)
    os.makedirs(out, exist_ok=True)
    archive = os.path.join(out, f"{NAME}-{v}.tar.gz")
    write_archive(archive, f"{NAME}-{v}/", files)
    blob = open(archive, "rb").read()
    release = dict(manifest, archive={"name": os.path.basename(archive), "sha256": sha256_of(blob), "size": len(blob),
                                      "prefix": f"{NAME}-{v}/"})
    mpath = os.path.join(out, f"{NAME}-{v}.manifest.json")
    with open(mpath, "w", encoding="utf-8") as f:
        f.write(json.dumps(release, indent=2, sort_keys=True) + "\n")
    return archive, mpath


def read_archive(archive: str) -> tuple[str, dict[str, bytes]]:
    """(prefix, relative path -> bytes), refusing anything that is not a plain file below one folder."""
    members: dict[str, bytes] = {}
    prefix = None
    with tarfile.open(archive, "r:gz") as tar:
        for m in tar.getmembers():
            parts = m.name.split("/")
            if m.name.startswith("/") or ".." in parts or "" in parts[:-1] or len(parts) < 2:
                raise SdkError(f"unsafe member name {m.name!r}")
            if not m.isreg():
                raise SdkError(f"{m.name} is not a regular file")
            if prefix is None:
                prefix = parts[0] + "/"
            elif parts[0] + "/" != prefix:
                raise SdkError(f"{m.name} is outside {prefix}")
            members["/".join(parts[1:])] = tar.extractfile(m).read()  # type: ignore[union-attr]
    if prefix is None:
        raise SdkError("the archive is empty")
    return prefix, members


def verify(archive: str, manifest_file: str | None) -> dict:
    prefix, members = read_archive(archive)
    if INNER_MANIFEST not in members:
        raise SdkError(f"{INNER_MANIFEST} is missing")
    inner = json.loads(members.pop(INNER_MANIFEST))
    if inner.get("schema") != SCHEMA:
        raise SdkError(f"unknown schema {inner.get('schema')!r}")
    if prefix != f"{NAME}-{inner['version']}/":
        raise SdkError(f"folder {prefix} does not match version {inner['version']}")
    listed = {f["path"]: f for f in inner["files"]}
    if set(listed) != set(members):
        raise SdkError("files differ from the manifest: " + ", ".join(sorted(set(listed) ^ set(members))[:5]))
    for path, body in members.items():
        if sha256_of(body) != listed[path]["sha256"] or len(body) != listed[path]["size"]:
            raise SdkError(f"{path} differs from the manifest")
    if manifest_file:
        release = json.load(open(manifest_file, encoding="utf-8"))
        if release["archive"]["sha256"] != sha256_of(open(archive, "rb").read()):
            raise SdkError("the archive's SHA-256 differs from the release manifest")
        if {k: v for k, v in release.items() if k != "archive"} != inner:
            raise SdkError("the release manifest and the manifest inside the archive differ")
    return inner


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("--out", required=True)
    b.add_argument("--tag")
    b.add_argument("--version")
    b.add_argument("--root", default=_layout.ROOT)
    v = sub.add_parser("verify")
    v.add_argument("archive")
    v.add_argument("--manifest")
    args = ap.parse_args(argv)
    try:
        if args.cmd == "build":
            archive, manifest = build(os.path.abspath(args.root), args.out, args.tag, args.version)
            print(f"{sha256_of(open(archive, 'rb').read())}  {os.path.basename(archive)}")
            print(f"{sha256_of(open(manifest, 'rb').read())}  {os.path.basename(manifest)}")
        else:
            inner = verify(args.archive, args.manifest)
            print(f"ok: {NAME} {inner['version']}, {len(inner['files'])} files")
    except (SdkError, OSError, tarfile.TarError, subprocess.CalledProcessError, KeyError, ValueError) as e:
        print(f"port_sdk: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
