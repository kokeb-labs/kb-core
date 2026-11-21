# Throw-away git repositories for the catalogue tests: a driver with driver.yml, LICENSE and tags.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""Create small local driver repositories with tags."""

import os
import subprocess

MIT = "MIT License\n\nCopyright (c) 2026 The driver-x authors\n\nPermission is hereby granted, free of charge, to any person\n"
ENV = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@example.invalid", GIT_COMMITTER_NAME="t",
           GIT_COMMITTER_EMAIL="t@example.invalid", GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_SYSTEM=os.devnull)


def git(repo, *args):
    return subprocess.run(["git", "-C", repo, "-c", "commit.gpgsign=false", "-c", "tag.gpgsign=false", *args],
                          check=True, capture_output=True, text=True, env=ENV).stdout.strip()


def driver_repo(path, name, releases, origin=None, license_text=MIT, extra_yml=""):
    """``releases``: list of (tag, version in driver.yml, bus list text)."""
    os.makedirs(path)
    git(path, "init", "-q", "-b", "main")
    if origin:
        git(path, "remote", "add", "origin", origin)
    for tag, version, bus in releases:
        with open(os.path.join(path, "driver.yml"), "w", encoding="utf-8") as fh:
            fh.write(f"name: {name}\nversion: {version}\nbus: {bus}\nmcus: [any]\nperipheral: null\n"
                     f"kb_core_min: null\nauthors: [\"The driver-{name} authors\"]\n{extra_yml}")
        if license_text is not None:
            with open(os.path.join(path, "LICENSE"), "w", encoding="utf-8") as fh:
                fh.write(license_text)
        git(path, "add", "-A")
        git(path, "commit", "-q", "-m", f"release {tag}")
        if tag:
            git(path, "tag", "-a", tag, "-m", tag)
    return path
