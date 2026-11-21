"""The components of kb-core and what they depend on.

Reads: Three bands read from the folders of the repository, so the figure cannot drift. components/lib holds the parts:
kb::utils at the bottom, kb::rtos on it, kb::interfaces on that, and kb::handlers on top. components/ports holds the MCU
ports, which satisfy the interface contracts (the host port is for tests). components/drivers holds the pinned drivers,
each a submodule at a release tag; kb::handlers binds them and nothing else may use them.
"""

import kbfig
from kokeb_mkdocs_theme.figkit import Fig
from kokeb_mkdocs_theme.paths import repo_root

ROOT = repo_root()
LIB_ROLE = {"utils": "core", "rtos": "rtos", "interfaces": "contract", "handlers": "handler"}
LIB_ORDER = ["handlers", "interfaces", "rtos", "utils"]
LIB_NOTE = {
    "utils": "Status/Result, units, containers, CRC, seqlock",
    "rtos": "mutexes, queues, threads; none, FreeRTOS, ThreadX backends",
    "interfaces": "contracts as C++20 concepts, host test port",
    "handlers": "bindings, handler core, start-up pipeline",
}


def data():
    lib = sorted(p.name for p in (ROOT / "components/lib").iterdir() if p.is_dir())
    ports = sorted(p.name for p in (ROOT / "components/ports").iterdir() if p.is_dir())
    drivers = sorted(p.name.removeprefix("driver-") for p in (ROOT / "components/drivers").glob("driver-*"))
    return {"lib": lib, "ports": ports, "drivers": drivers,
            "sources": ["components/lib", "components/ports", "components/drivers"]}


def build(d):
    f = Fig(
        "components-map",
        "Parts, ports and drivers",
        alt=kbfig.title_case_alt(
            "Map of the repository: the lib parts utils, rtos, interfaces and handlers, each depending on the next down;",
            "MCU ports that satisfy the interface contracts; and the pinned drivers that handlers bind."),
        kicker="Architecture",
        subtitle="Read from components/: a new part, port or driver appears here without anyone editing the figure.",
        legend=kbfig.legend("handler", "contract", "rtos", "core", "port", "driver"),
    )
    W = f.body_w
    parts = [p for p in LIB_ORDER if p in d["lib"]] + [p for p in d["lib"] if p not in LIB_ORDER]
    cards = [dict(title=f"kb::{p}", lines=[LIB_NOTE.get(p, "")], role=LIB_ROLE.get(p, "core"), accent=True,
                  sub=f"components/lib/{p}") for p in parts]
    band, bx = kbfig.lane(f, 0, 0, W, "components/lib: the parts", cards, role="core", gap=16)
    for a, b in zip(bx, bx[1:]):
        f.edge([(a.r + 3, a.cy), (b.x - 3, b.cy)], tone="rail")
    y = band.b + 70
    dw = (W - 40) * 0.58
    inner = f.region_inner_top(y)
    h = f.chips(16, inner, dw - 32, d["drivers"], tone="green")
    f.region(0, y, dw, inner - y + h + 18, "components/drivers: pinned releases", tone="green", sub=f"{len(d['drivers'])} drivers")
    gx = dw + 40
    pband, pb = kbfig.lane(f, gx, y, W - gx, "components/ports: the MCU seam",
                           [dict(title=p, lines=["MCU port" if p != "host" else "tests only"], role="port", accent=True)
                            for p in d["ports"]], role="port", gap=16)
    hand = next((b for b, p in zip(bx, parts) if p == "handlers"), bx[0])
    ifc = next((b for b, p in zip(bx, parts) if p == "interfaces"), bx[0])
    f.connect(pband, ifc, "top", "bottom", at=0.92, bt=0.5, tone="teal", label="satisfy")
    f.edge([(hand.cx, hand.b + 3), (hand.cx, y - 3)], tone="violet", label="bind")
    return f
