"""The components of kb-core and what they depend on.

Reads: Read from the folders of the repository, so the figure cannot drift. The folder components/lib holds the parts:
kb::utils at the bottom, kb::rtos on it, kb::interfaces on that, and kb::handlers on top; kb::dma, the chip-neutral DMA
vocabulary, also rests on kb::utils. The folder components/ports holds the MCU ports, which satisfy the interface
contracts (the host port is for tests only). The folder components/drivers holds the pinned drivers, each a submodule at
a release tag, listed by name; kb::handlers binds them and nothing else may use them.
"""

from kokeb_mkdocs_theme.figkit.kinds import from_model
from kokeb_mkdocs_theme.paths import repo_root

ROOT = repo_root()
#: Part -> (tone, the line under its name, the part it rests on). A part not listed here is drawn without an edge.
LIB = {
    "handlers": ("violet", "bindings, core, start-up", "interfaces"),
    "interfaces": ("blue", "C++20 concepts, host port", "rtos"),
    "rtos": ("amber", "none, FreeRTOS, ThreadX", "utils"),
    "utils": ("brand", "Result, units, CRC, seqlock", ""),
    "dma": ("brand", "buffers, leases, streams", "utils"),
}
ORDER = ["handlers", "interfaces", "rtos", "utils", "dma"]


def data():
    lib = sorted(p.name for p in (ROOT / "components/lib").iterdir() if p.is_dir())
    ports = sorted(p.name for p in (ROOT / "components/ports").iterdir() if p.is_dir())
    drivers = sorted(p.name.removeprefix("driver-") for p in (ROOT / "components/drivers").glob("driver-*"))
    return {"lib": lib, "ports": ports, "drivers": drivers,
            "sources": ["components/lib", "components/ports", "components/drivers"]}


def model(d: dict) -> dict:
    """The graph model of the figure, from the folder listing in ``d``."""
    parts = [p for p in ORDER if p in d["lib"]] + [p for p in d["lib"] if p not in ORDER]
    nodes = []
    edges = []
    for p in parts:
        tone, line, rests_on = LIB.get(p, ("brand", f"components/lib/{p}", ""))
        nodes.append({"id": p, "label": f"kb::{p}", "tone": tone, "lines": [line], "accent": True})
        if rests_on in parts:
            edges.append({"from": p, "to": rests_on})
    nodes.append({"id": "ports", "label": "components/ports", "tone": "teal", "accent": True,
                  "lines": [f"{p}: {'tests only' if p == 'host' else 'MCU port'}" for p in d["ports"]]})
    nodes.append({"id": "drivers", "label": f"{len(d['drivers'])} pinned drivers", "tone": "green", "accent": True,
                  "lines": [", ".join(d["drivers"])]})
    if "interfaces" in parts:
        edges.append({"from": "ports", "to": "interfaces", "label": "satisfy", "tone": "teal"})
    if "handlers" in parts:
        edges.append({"from": "handlers", "to": "drivers", "label": "bind", "tone": "violet"})
    return {
        "kind": "graph",
        "id": "components-map",
        "title": "Parts, ports and drivers",
        "kicker": "Architecture",
        "subtitle": "Read from components/: a new part, port or driver appears here without anyone editing the figure.",
        "alt": ("Map of the repository in which the lib parts handlers, interfaces, rtos and utils each rest on the next "
                "one down, the MCU ports satisfy the interface contracts, and handlers bind the pinned drivers."),
        "direction": "TB",
        "print": "portrait",
        "legend": [["violet", "handlers"], ["blue", "interfaces"], ["amber", "kb::rtos"], ["brand", "kb::utils"],
                   ["teal", "MCU ports"], ["green", "drivers"]],
        "nodes": nodes,
        "edges": edges,
    }


def build(d):
    return from_model(model(d))
