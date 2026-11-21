#!/usr/bin/env python3
"""Find direct and indirect recursion (MISRA C++:2023 Rule 8.2.10) in a driver tree, before CodeQL does.

usage: python3 -I recursion_check.py [--root DIR] [--paths inc src] [FILE...]
exit 1 when a cycle is found, 0 otherwise.

A text call graph, standard library only. Comments and string/char literals are blanked first. A function definition
is `name(...) <qualifiers> {` at file, namespace or class level (not inside another function body); its body is the
matching braces, lambdas included. A call is `name(` or `name<...>(` inside a body, where `name` is a function defined
in the scanned files; calls through `.` or `->` on another object (`port_.Transfer()`) are not edges, `this->f()` is.
A function is told apart by name and number of parameters (defaults give a range); a call goes to every definition
whose name and range fit its argument count. A cycle made of calls with one fitting definition each is reported as a
cycle (exit 1). A cycle that needs a call with several fitting definitions (overloads of the same arity, or
same-named members of different classes) is listed as "possible" for a human to check, and does not fail. Cycles are
the strongly connected components of two or more functions, and functions that call themselves.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

KEYWORDS = {
    "if", "for", "while", "switch", "catch", "return", "sizeof", "alignof", "decltype", "static_assert", "noexcept",
    "requires", "constexpr", "consteval", "else", "do", "case", "default", "new", "delete", "throw", "typeid",
    "alignas", "operator", "template", "typename", "using", "namespace", "class", "struct", "union", "enum",
    "static_cast", "reinterpret_cast", "const_cast", "dynamic_cast", "co_await", "co_return", "co_yield", "assert",
}
QUALIFIER = re.compile(r"^\s*((const|noexcept(\s*\([^()]*\))?|override|final|volatile|&&|&|mutable)\s*)*"
                       r"(->\s*[\w:<>,\s*&]+)?\s*$")
CALL = re.compile(r"(\bthis\s*->\s*|[.>]\s*|::\s*|\b)(~?\w+)\s*(?:<[^;(){}]*>)?\s*\(")


def blank(text: str) -> str:
    """Comments and literals become spaces, line breaks kept (so offsets still give line numbers)."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        elif c in "\"'":
            # a digit separator (1'000) is not a char literal
            if c == "'" and i > 0 and text[i - 1].isalnum() and i + 1 < n and text[i + 1].isalnum():
                out.append(" ")
                i += 1
                continue
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(c + re.sub(r"[^\n]", " ", text[i + 1:j - 1]) + c)
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def match_back(text: str, close: int) -> int:
    """Index of the '(' that matches the ')' at `close`, or -1."""
    depth = 0
    for k in range(close, -1, -1):
        if text[k] == ")":
            depth += 1
        elif text[k] == "(":
            depth -= 1
            if depth == 0:
                return k
    return -1


def split_top(args: str) -> list[str]:
    """Top-level comma-separated parts (nested (), <>, [], {} kept together)."""
    parts, depth, cur = [], 0, []
    for ch in args:
        if ch in "([{<":
            depth += 1
        elif ch in ")]}>":
            depth = max(depth - 1, 0)
        if ch == "," and depth == 0:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    parts.append("".join(cur))
    return [x for x in (y.strip() for y in parts) if x and x != "void"]


def arity_range(params: str) -> tuple[int, int]:
    parts = split_top(params)
    if any("..." in x for x in parts):
        return (len([x for x in parts if "..." not in x and "=" not in x]), 99)
    return (len([x for x in parts if "=" not in x]), len(parts))


def definition_name(text: str, brace: int):
    """The function name when the '{' at `brace` opens a function body, else None."""
    start = max(text.rfind(";", 0, brace), text.rfind("}", 0, brace), text.rfind("{", 0, brace)) + 1
    head = text[start:brace]
    close = head.rfind(")")
    if close < 0 or not QUALIFIER.match(head[close + 1:]):
        # a constructor's member-initialiser list: ") : a_(x), b_(y) {" -> look at the last ')' before ':'
        m = re.search(r"\)\s*:\s*[\w:]+\s*[({]", head)
        if not m:
            return None
        close = m.start()
    opened = match_back(head, close)
    if opened < 0:
        return None
    m = re.search(r"(~?\w+)\s*(?:<[^;(){}]*>)?\s*$", head[:opened])
    if not m or m.group(1) in KEYWORDS:
        return None
    before = head[:m.start()].rstrip()
    if before.endswith(("=", "(", ",", "return", "[", "]")):
        return None  # a call or a lambda, not a definition
    if "[" in head[:opened] and "]" in head[:opened] and re.search(r"\]\s*$", head[:opened].rstrip()):
        return None
    return m.group(1), head[opened + 1:close]


DECL = re.compile(r"(~?\w+)\s*\(([^;{}()]*(?:\([^;{}()]*\)[^;{}()]*)*)\)\s*"
                  r"(?:const|noexcept|override|final|&&|&|\s)*(?:->\s*[\w:<>,\s*&]+)?;")


def scan(path: Path, defs: dict, bodies: list, decls: dict) -> None:
    raw = path.read_text(errors="replace")
    text = blank(raw)
    for m in DECL.finditer(text):
        if m.group(1) not in KEYWORDS and "=" in m.group(2):
            decls.setdefault(m.group(1), []).append(arity_range(m.group(2)))
    depth = 0
    fn_depth = -1
    fn_name = ""
    fn_start = 0
    for i, c in enumerate(text):
        if c == "{":
            if fn_depth < 0:
                found = definition_name(text, i)
                if found:
                    name, params = found
                    lo, hi = arity_range(params)
                    line = text.count("\n", 0, i) + 1
                    node = f"{name}/{hi}@{path.name}:{line}"
                    fn_depth, fn_name, fn_start = depth, node, i
                    defs.setdefault(name, {})[node] = [lo, hi, str(path), line]
            depth += 1
        elif c == "}":
            depth -= 1
            if fn_depth >= 0 and depth == fn_depth:
                bodies.append((fn_name, path, text, fn_start, i))
                fn_depth = -1


def call_args(text: str, open_paren: int) -> str:
    depth = 0
    for k in range(open_paren, len(text)):
        if text[k] == "(":
            depth += 1
        elif text[k] == ")":
            depth -= 1
            if depth == 0:
                return text[open_paren + 1:k]
    return ""


def graph(files: list[Path]):
    defs: dict[str, dict] = {}
    bodies: list = []
    decls: dict[str, list] = {}
    for f in files:
        scan(f, defs, bodies, decls)
    # default arguments live on the declaration: widen a definition's range to it
    for name, nodes in defs.items():
        for entry in nodes.values():
            for lo, hi in decls.get(name, []):
                if hi == entry[1] and lo < entry[0]:
                    entry[0] = lo
    sure: dict[str, dict[str, tuple]] = {}
    maybe: dict[str, dict[str, tuple]] = {}
    for node, path, text, a, b in bodies:
        for m in CALL.finditer(text, a + 1, b):
            lead, callee = m.group(1), m.group(2)
            if callee not in defs or callee in KEYWORDS:
                continue
            if lead.strip().startswith((".", ">")) and "this" not in lead:
                continue  # another object's member
            n = len(split_top(call_args(text, m.end() - 1)))
            fits = [k for k, (lo, hi, _p, _l) in defs[callee].items() if lo <= n <= hi]
            line = text.count("\n", 0, m.start(2)) + 1
            for k in fits:
                (sure if len(fits) == 1 else maybe).setdefault(node, {}).setdefault(k, (str(path), line))
    return defs, sure, maybe


def cycles(edges: dict) -> list[list[str]]:
    index, low, stack, on, out, counter = {}, {}, [], set(), [], [0]
    sys.setrecursionlimit(10000)

    def visit(v):
        index[v] = low[v] = counter[0]
        counter[0] += 1
        stack.append(v)
        on.add(v)
        for w in edges.get(v, {}):
            if w not in index:
                visit(w)
                low[v] = min(low[v], low[w])
            elif w in on:
                low[v] = min(low[v], index[w])
        if low[v] == index[v]:
            comp = []
            while True:
                w = stack.pop()
                on.discard(w)
                comp.append(w)
                if w == v:
                    break
            if len(comp) > 1 or v in edges.get(v, {}):
                out.append(sorted(comp))

    for v in list(edges):
        if v not in index:
            visit(v)
    return out


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", default=".")
    ap.add_argument("--paths", nargs="*", default=["inc", "src"])
    ap.add_argument("files", nargs="*")
    a = ap.parse_args(argv)
    root = Path(a.root)
    files = [Path(f) for f in a.files] or sorted(
        p for d in a.paths for p in (root / d).rglob("*") if p.suffix in {".hpp", ".ipp", ".h", ".cpp"})
    defs, sure, maybe = graph(files)
    both: dict[str, dict[str, tuple]] = {k: dict(v) for k, v in sure.items()}
    for k, v in maybe.items():
        both.setdefault(k, {}).update(v)
    found = cycles(sure)
    known = {frozenset(c) for c in found}
    possible = [c for c in cycles(both) if frozenset(c) not in known]

    def show(kind: str, comps: list, edges: dict) -> None:
        for comp in comps:
            print(f"{kind}: {' -> '.join(comp)}")
            members = set(comp)
            for f in comp:
                for callee, (p, line) in sorted(edges.get(f, {}).items()):
                    if callee in members:
                        print(f"  {p}:{line}: {f} calls {callee}")

    show("cycle", found, sure)
    show("possible", possible, both)
    print(f"recursion_check: {len(files)} file(s), {len(defs)} function name(s), {len(found)} cycle(s), "
          f"{len(possible)} possible (overloads to check)")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
