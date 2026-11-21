"""The kb-core CI flow.

Reads: From the left. Every change first passes tools/preflight.sh on a workstation, then reaches CI as a pull request
or push. Always run: repository checks, layering, layout and examples, own parts, MISRA and pin hygiene. Run when the
paths match, and on main and tags: quality, integration, the ESP-IDF port job and the catalogue. Never re-run for
components/drivers: the drivers' own tests, which are trusted through pin hygiene. Everything ends in one required
gate; a tag adds the release assets, and a schedule re-scans releases for known vulnerabilities.
"""

import kbfig
from kokeb_mkdocs_theme.figkit import Fig


def build(_d):
    f = Fig(
        "ci-flow",
        "The CI flow",
        alt=kbfig.title_case_alt(
            "Flow: local preflight, then a pull request; always-run checks, path-gated checks and the drivers that are",
            "never re-run feed one required gate; a tag then produces the release assets and a schedule re-scans them."),
        kicker="Testing",
        subtitle="Every CI job has a local equivalent in tools/preflight.sh, run before any push.",
        legend=[("brown", "start"), ("green", "always"), ("blue", "path-gated"), ("slate", "never re-run"),
                ("brand", "gate and release")],
    )
    W = f.body_w
    pf, pr = kbfig.flow(f, 0, 0, W * 0.6, [
        dict(title="tools/preflight.sh", lines=["local, before every push"], tone="brown", accent=True),
        dict(title="Pull request or push", tone="brown", accent=True)], gap=60)
    y = pr.b + 44
    a, ac = kbfig.lane(f, 0, y, W, "Always, on every pull request", [
        dict(title="Repository checks", lines=["DCO, lint, handbook, drift"], role="ok"),
        dict(title="Layering", lines=["include checker, CMake DAG, poison build, symbol lint"], role="ok"),
        dict(title="Layout and examples", lines=["root allowlist, retired locations, example index"], role="ok"),
        dict(title="Own parts", lines=["host build and tests on GCC and Clang, clang-tidy, cppcheck"], role="ok"),
        dict(title="MISRA, level high", lines=["CodeQL pack plus kokeb-cpp20, host and Cortex-M"], role="ok"),
        dict(title="Pin hygiene", lines=["gitlink = lock = release or rc tag with a green gate; never a branch"], role="ok")],
        role="ok", gap=14)
    g, gc = kbfig.lane(f, 0, a.b + 40, W, "Path-gated, and on main and tags", [
        dict(title="Quality", lines=["coverage, sanitizers, fuzz, mutation kill-rate gate"], role="contract"),
        dict(title="Integration", lines=["binding x port x MCU and RTOS matrix from catalog/compat.yml"], role="contract"),
        dict(title="Port job", lines=["ESP-IDF v5.5 build of components/ports/esp32"], role="contract"),
        dict(title="Catalogue", lines=["regenerate compat.yml, fail on drift"], role="contract")],
        role="contract", sub="components/drivers, handlers, interfaces, ports and cmake", gap=14)
    n, nc = kbfig.lane(f, 0, g.b + 40, W, "Never re-run for components/drivers", [
        dict(title="Driver tests, fuzz, mutation, MISRA, tidy, docs", lines=["trusted through pin hygiene: each driver runs them in its own repository"], role="soup", dashed=True)],
        role="soup")
    f.edge([(pr.cx, pr.b + 3), (pr.cx, a.y - 3)], tone="rail", label="runs")
    gate, rel, vs = kbfig.row(f, 0, n.b + 56, W, [
        dict(title="ru-ci-gate", lines=["the single required check"], role="core", accent=True),
        dict(title="Tag vX.Y.Z", lines=["per-part packages, SHA256SUMS, compat.yml, release lock, SBOM, CVE scan, licence report, provenance"], role="core", accent=True),
        dict(title="Scheduled", lines=["CVE re-scan of releases, the tested version set kb YYYY.N"], role="core", accent=True)],
        gap=40, weights=[1, 2, 1.4])
    f.edge([(gate.cx, n.b + 3), (gate.cx, gate.y - 3)], tone="rail", label="all must pass")
    f.edge([(gate.r + 3, gate.cy), (rel.x - 3, rel.cy)], tone="brand")
    f.edge([(rel.r + 3, rel.cy), (vs.x - 3, vs.cy)], tone="brand")
    return f
