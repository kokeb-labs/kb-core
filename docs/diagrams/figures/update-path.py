"""The update path.

Reads: Left to right. kb outdated, or the configurator's Open an existing project view, plans from the current lock, the
catalogue and the image: kb-core moves first, then an extracted port, then each driver in name order. Each candidate must
pass the driver gates in order (MCU family, kb_core_min, owner-task rule, bus kind in use); a newer version that fails is
held back with the code of its first failing gate. The plan lists one row per component with the SemVer change, whether it
breaks, and known vulnerabilities fixed and remaining. kb upgrade writes a new image and lock, the CMake files, the fetch
script and kb-upgrade.md; the user reviews the diff and runs the fetch script, builds and tests.
"""

import kbfig
from kokeb_mkdocs_theme.figkit import Fig


def build(_d):
    f = Fig(
        "update-path",
        "The update path",
        alt=kbfig.title_case_alt(
            "Five steps: read the lock, catalogue and image; order kb-core, port and drivers; apply the driver gates and hold",
            "back what fails; write one row per component; write the new image, lock, CMake files, fetch script and report,",
            "then review, fetch, build and test."),
        kicker="Updates",
        subtitle="One pure function plans an upgrade. The kb tool and the configurator give the same bytes.",
        legend=[("blue", "inputs"), ("violet", "plan"), ("rose", "held back"), ("green", "written")],
    )
    W = f.body_w
    steps = kbfig.flow(f, 0, 0, W, [
        dict(title="1  Read", lines=["lock, catalogue and image: the current versions come from the lock"], role="contract", accent=True),
        dict(title="2  Order", lines=["kb-core, then an extracted port, then each driver by name"], role="handler", accent=True),
        dict(title="3  Gate", lines=["MCU family, kb_core_min, owner-task rule, bus kind in use"], role="handler", accent=True),
        dict(title="4  Rows", lines=["one per component: change, breaking, vulnerabilities fixed"], role="handler", accent=True),
        dict(title="5  Write", lines=["image, lock, CMake files, fetch script, kb-upgrade.md"], role="ok", accent=True)], gap=30)
    held = f.card(steps[2].x, steps[2].b + 64, steps[2].w, "Held back", ["a newer version that fails a gate is listed with the code of its first failing gate"], tone="rose", accent=True)
    f.edge([(steps[2].cx, steps[2].b + 3), (steps[2].cx, held.y - 3)], tone="rose", dashed=True, label="fails")
    rev = f.card(steps[4].x - 120, steps[4].b + 64, steps[4].w + 120, "Then, by hand", ["review the diff, sh kb-fetch.sh, build and test"], tone="brown", accent=True)
    f.edge([(steps[4].cx, steps[4].b + 3), (steps[4].cx, rev.y - 3)], tone="green")
    f.card(0, steps[0].b + 64, steps[1].r, "Entry points", ["kb outdated: exit 3 when a component can move", "kb upgrade: writes the files", "the configurator's Open an existing project view"], tone="brown", accent=True)
    return f
