# The board vectors (tests/resolver/vectors/board/): each is a board with, for a fail vector, one seeded conflict, and
# the findings the checker must give. `kb image check --selftest` runs them; the unit tests run them too, and a future
# configurator/boardcheck.mjs must agree with them byte for byte, as the resolver vectors work today.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""Load and run the board vectors."""

import json
import os
import re
import sys

from . import dmacheck, dmagen, repolayout

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
NAME = re.compile(r"^R(\d{2})-(pass|fail)(-[a-z0-9]+(-[a-z0-9]+)*)?\.json$")
RULES = tuple(f"R{n}" for n in range(1, 16))
KEYS = ("rule", "code", "severity", "subject")


def vector_dir(root=None):
    return os.path.join(root or ROOT, repolayout.path("tests"), "resolver", "vectors", "board")


def load_vectors(directory=None):
    """[(file name, vector)] in name order."""
    directory = directory or vector_dir()
    out = []
    for name in sorted(os.listdir(directory)):
        if name.endswith(".json"):
            with open(os.path.join(directory, name), encoding="utf-8") as fh:
                out.append((name, json.load(fh)))
    return out


def exit_code(result, strict=False):
    """The exit code `kb image check` gives: 1 on an error finding, and on a warning too with --strict."""
    if not result["ok"]:
        return 1
    return 1 if strict and result["findings"] else 0


def run_vector(name, vec, data):
    """The problems of one vector (empty when it holds)."""
    problems = []
    match = NAME.match(name)
    if not match:
        return [f"{name}: the name is R<nn>-<pass|fail>[-<case>].json"]
    for key in ("description", "board", "expect"):
        if key not in vec:
            problems.append(f"{name}: missing {key}")
    if problems:
        return problems
    result = dmacheck.check(vec["board"], data, sdkconfig=vec.get("sdkconfig"), drivers=vec.get("drivers"))
    expect = vec["expect"]
    actual = [{k: f[k] for k in KEYS} for f in result["findings"]]
    if result["ok"] != expect.get("ok"):
        problems.append(f"{name}: ok is {result['ok']}, expected {expect.get('ok')}")
    if actual != expect.get("findings"):
        problems.append(f"{name}: findings differ\n  got      {json.dumps(actual)}\n  expected {json.dumps(expect.get('findings'))}")
    code = exit_code(result, bool(vec.get("strict")))
    if code != expect.get("exit"):
        problems.append(f"{name}: exit code {code}, expected {expect.get('exit')}")
    rule, kind = f"R{int(match.group(1))}", match.group(2)
    if kind == "fail" and code != 1:
        problems.append(f"{name}: a fail vector must make `kb image check` exit 1")
    if kind == "fail" and rule not in {f["rule"] for f in result["findings"]}:
        problems.append(f"{name}: a fail vector of {rule} must give a {rule} finding")
    if kind == "pass" and code != 0:
        problems.append(f"{name}: a pass vector must make `kb image check` exit 0")
    if kind == "pass" and any(f["rule"] == rule and f["severity"] == "error" for f in result["findings"]):
        problems.append(f"{name}: a pass vector of {rule} must not give a {rule} error")
    if result["ok"]:
        first = dmagen.generate(vec["board"], result)
        if first != dmagen.generate(vec["board"], dmacheck.check(vec["board"], data, sdkconfig=vec.get("sdkconfig"),
                                                                    drivers=vec.get("drivers"))):
            problems.append(f"{name}: the generated files are not byte-stable")
    for f in result["findings"]:
        if f["code"] not in dmacheck.CODES:
            problems.append(f"{name}: unknown code {f['code']}")
    return problems


def coverage(vectors):
    """Problems with the set: every rule R1 to R15 has at least one pass and one fail vector."""
    problems = []
    have = {}
    for name, _ in vectors:
        m = NAME.match(name)
        if m:
            have.setdefault((f"R{int(m.group(1))}", m.group(2)), []).append(name)
    for rule in RULES:
        for kind in ("pass", "fail"):
            if (rule, kind) not in have:
                problems.append(f"no {kind} vector for {rule}")
    return problems


def selftest(data, directory=None, out=None):
    """Run every vector; print the problems and a summary; 0 only when all hold."""
    out = out or sys.stdout
    vectors = load_vectors(directory)
    problems = coverage(vectors)
    for name, vec in vectors:
        problems += run_vector(name, vec, data)
    for p in problems:
        print(f"error: {p}", file=sys.stderr)
    print(f"kb: board selftest: {len(vectors)} vector(s), {len(problems)} problem(s)", file=out)
    return 1 if problems else 0
