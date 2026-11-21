"""The assembler: from tags to a project.

Reads: Left to right. kb catalog build reads each driver's driver.yml at its tags and the kb-core tags with their
platform table and pins, and writes catalog.json. The resolver, one specification with a Python and a JavaScript
implementation, reads kb-image.yml and the catalogue and writes the lock kb-image.lock.yml and three project files:
CMakePresets.json, kb_image.cmake and kb-fetch.sh. kb sbom turns the lock into CycloneDX 1.6 and SPDX 2.3 documents.
"""

import kbfig
from kokeb_mkdocs_theme.figkit import Fig


def build(_d):
    f = Fig(
        "assembler-flow",
        "From tags to a project",
        alt=kbfig.title_case_alt(
            "Driver and kb-core tags feed kb catalog build, which writes catalog.json; the resolver reads it and kb-image.yml",
            "and writes the lock and the project files; kb sbom turns the lock into SBOMs."),
        kicker="Configurator",
        subtitle="One resolver specification, implemented in Python and JavaScript, held to the same vectors.",
        legend=[("blue", "inputs"), ("brown", "tool"), ("green", "outputs")],
    )
    W = f.body_w
    cw, rh = 252, 92
    X = [0, 294, 588, 884]
    ys = [i * 118 for i in range(6)] + []
    def c(col, r, t, ls, tone, **kw):
        return f.card(X[col], r * 118, cw, t, ls, tone=tone, h=rh, accent=True, **kw)
    d = c(0, 0, "driver-<chip> tags", ["driver.yml at each tag"], "blue")
    k = c(0, 1, "kb-core tags", ["platform table, pins"], "blue")
    build = c(1, 0.5, "kb catalog build", ["reads the tags"], "brown")
    cat = c(2, 0.5, "catalog.json", ["every part at every version"], "green")
    img = c(0, 3, "kb-image.yml", ["what the user selected"], "blue")
    res = c(2, 3, "Resolver", ["Python or JavaScript, one spec"], "brown")
    out = c(3, 2, "Project files", ["CMakePresets.json, kb_image.cmake, kb-fetch.sh"], "green")
    lock = c(3, 3, "kb-image.lock.yml", ["every version, tag and SHA"], "green")
    sbom = c(3, 4, "kb sbom", ["CycloneDX 1.6 and SPDX 2.3"], "brown")
    f.edge([(d.r + 3, d.cy), (build.x - 20, d.cy), (build.x - 20, build.cy - 20), (build.x - 3, build.cy - 20)])
    f.edge([(k.r + 3, k.cy), (build.x - 20, k.cy), (build.x - 20, build.cy + 20), (build.x - 3, build.cy + 20)])
    f.edge([(build.r + 3, build.cy), (cat.x - 3, cat.cy)])
    f.edge([(cat.cx, cat.b + 3), (res.cx, res.y - 3)], label="read")
    f.edge([(img.r + 3, img.cy), (res.x - 3, res.cy)], label="read")
    f.edge([(res.r + 3, res.cy), (lock.x - 3, lock.cy)], tone="green")
    f.edge([(res.r + 3, res.cy - 20), (res.r + 24, res.cy - 20), (res.r + 24, out.cy), (out.x - 3, out.cy)], tone="green")
    f.edge([(lock.cx, lock.b + 3), (sbom.cx, sbom.y - 3)], tone="brown")
    return f
