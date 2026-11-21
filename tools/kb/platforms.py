# The MCU families, RTOS backends and parts of kb-core that the catalogue lists next to the drivers. Mirrors
# cmake/kb_build_settings.cmake (_KB_SUPPORTED_MCUS, _KB_PLANNED_MCUS, _KB_SUPPORTED_RTOS, _KB_PLANNED_RTOS);
# tests/test_platforms.py checks that every value there is here with the same status.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""Static platform table of the catalogue."""

from . import repolayout

KB_CORE_REPO = "kokeb-labs/kb-core"
KB_CORE_LICENSE = "MIT"
KB_CORE_AUTHORS = ["The kb-core authors"]
PARTS = ["utils", "rtos", "dma", "interfaces", "handlers"]



def _in_tree(entry, *rest):
    """A path inside this repository: the layout entry (tools/layout/paths.yml) and the folders below it."""
    return "/".join((repolayout.path(entry),) + rest)


RTOS = [
    {"name": "freertos", "status": "supported"},
    {"name": "host", "status": "supported"},      # std::thread backend for host tests, outside the MISRA claim
    {"name": "none", "status": "supported"},
    {"name": "threadx", "status": "supported"},  # ADR-0015
]


def mcus(port_versions=None):
    """The MCU families; ``port_versions`` maps a family to the versions of its extracted port repository."""
    port_versions = port_versions or {}
    return [
        {"family": "esp32", "status": "supported",
         "targets": ["esp32", "esp32c3", "esp32c5", "esp32c6", "esp32h2", "esp32p4", "esp32s2", "esp32s3"],
         "rtos": ["freertos"], "default_rtos": "freertos",
         "port": {"name": "esp32", "repo": "kokeb-labs-drivers/port-esp32", "path": _in_tree("ports", "esp32"),
                  "versions": list(port_versions.get("esp32", []))}},
        {"family": "host", "status": "supported", "targets": ["host"], "rtos": ["host", "none"],
         "default_rtos": "none",
         "port": {"name": "host", "repo": KB_CORE_REPO, "path": _in_tree("lib", "interfaces", "host"), "versions": []}},
        {"family": "stm32", "status": "planned", "targets": ["stm32"], "rtos": ["freertos", "none", "threadx"],
         "default_rtos": "freertos", "port": None},
    ]
