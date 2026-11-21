"""Generators and the hosted configurator.

Reads: Left to right. Inputs are single sources published with each kb-core release: each pinned driver's driver.yml, the
release lock with its tags and SHAs, the evidence (SBOM, CVE scan, licence report), the tested version sets and, later,
board.yml. One resolver specification has two front ends, the kb command line and the static configurator site; it
resolves drivers to handlers to interfaces to parts, applies the MCU gates, kb_core_min and the thread model per bus. It
produces byte-for-byte repeatable downloads: kb-image.yml, git commands, a CMake presets fragment with kb_image.cmake,
the evidence for the selection and, later, a project archive.
"""

import kbfig
from kokeb_mkdocs_theme.figkit import Fig


def build(_d):
    f = Fig(
        "generator-flow",
        "Generators and the configurator",
        alt=kbfig.title_case_alt(
            "Three columns. Inputs: driver.yml files, the release lock, evidence, tested version sets and later board.yml.",
            "Middle: the kb command line and the configurator site, both using one resolver. Outputs: kb-image.yml, git",
            "commands, CMake presets, evidence for the selection and later a project archive."),
        kicker="Configurator",
        subtitle="One resolver specification, two front ends, repeatable downloads. Dashed cards come later (M6, M7).",
        legend=[("blue", "inputs"), ("brown", "tool"), ("green", "outputs")],
    )
    W = f.body_w
    cw = (W - 2 * 70) / 3
    X = [0, cw + 70, 2 * (cw + 70)]
    def col(i, label, role, items, y=0):
        inner = f.region_inner_top(y)
        h = [kbfig.need_h(f, cw - 32, t, ls) for t, ls, _d in items]
        yy = inner
        boxes = []
        for (t, ls, dashed), hh in zip(items, h):
            boxes.append(f.card(X[i] + 16, yy, cw - 32, t, ls, tone=kbfig.tone(role), accent=True, dashed=dashed, h=hh))
            yy += hh + 14
        f.region(X[i], y, cw, yy - y + 2, label, tone=kbfig.tone(role))
        return boxes, yy
    ins, ye = col(0, "Inputs", "contract", [
        ("driver.yml of each pinned driver", ["bus, mcus, peripheral, kb_core_min, ports, binding"], False),
        ("Release lock", ["kb-core tag and SHA, every pin with tag and SHA"], False),
        ("Evidence", ["SBOM (SPDX, CycloneDX), CVE scan, licence report"], False),
        ("Tested version sets", ["kb YYYY.N"], False),
        ("board.yml", ["buses, pins, chips (later)"], True)])
    mid, _ = col(1, "One resolver spec", "tool", [
        ("kb command line", ["Python standard library only, tools/kb"], False),
        ("Configurator site", ["static, GitHub Pages: pick MCU, RTOS, drivers, handlers"], False),
        ("Resolve", ["drivers, then handlers, interfaces and parts; MCU gates; kb_core_min; thread model per bus"], False)])
    out, _ = col(2, "Downloads, byte for byte repeatable", "ok", [
        ("kb-image.yml", ["the selection"], False),
        ("Git commands", ["clone at the tag, check the SHA, init only the enabled submodules"], False),
        ("CMake presets and kb_image.cmake", ["a presets fragment and the image include"], False),
        ("Evidence for the selection", ["SBOM, CVEs, licences, compatibility rows, tested set"], False),
        ("Project archive", ["later"], True)])
    my = mid[1].cy
    f.edge([(X[0] + cw + 3, my), (X[1] - 3, my)], tone="rail", label="read")
    f.edge([(X[1] + cw + 3, my), (X[2] - 3, my)], tone="rail", label="writes")
    return f
