# board.yml: load and normalise one board's DMA declaration (docs/design/board-dma.md, ADR-0033).
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""Load a board file and normalise it: hex strings and integers become integers, defaults are filled, names are
validated, unknown keys are reported. Pure functions apart from `load`; the same dictionary shape goes to
`kb.dmacheck.check` and, as JSON, to the configurator's checker."""

import re

from . import yamlio

# One name style for regions, buffers, streams and buses: it becomes a C++ type or variable name, so it is
# lower-case words joined by single underscores.
NAME = re.compile(r"^[a-z][a-z0-9]*(_[a-z0-9]+)*$")
NAME_MAX = 32

FAMILIES = ("stm32", "esp32")
CACHES = ("none", "maintained", "coherent")
PRIORITIES = ("low", "medium", "high", "very_high")
NOTIFY = ("none", "task")
IRQ = ("all", "errors_only")
MODES = ("normal", "circular")
FALLBACKS = ("oneshot", "polled", "irq")
BUS_KINDS = ("spi", "uart", "i2c")
ESP_DRIVERS = ("spi_master", "adc_continuous", "uhci", "rmt", "i2s")

_MCU_KEYS = {"family", "device", "target", "core"}
_RTOS_KEYS = {"name", "max_syscall_priority", "priority_bits"}
_REGION_KEYS = {"base", "size", "cache", "line", "align", "mpu_region", "masters"}
_BUFFER_KEYS = {"region", "bytes"}
_STREAM_KEYS = {"engine", "stream", "request", "priority", "irq_priority", "notify", "irq", "mode", "buffer", "frames",
                "element_bytes", "elements", "frames_per_pass", "trigger", "rate_hz", "stall_periods", "fallback",
                "core", "dmamux_sync", "dmamux_request_generator", "driver", "host", "frame_bytes", "iram_safe",
                "half_duplex_both_phases"}
_BUS_KEYS = {"kind", "model", "dma", "staging_bytes", "devices"}
_DEVICE_KEYS = {"driver", "max_frame_bytes"}
_HANDLER_KEYS = {"stream", "frame_bytes", "frames", "conversions"}
_FALLBACK_KEYS = {"kind", "cpu_percent"}
_LINKER_KEYS = {"fixed_sections", "noload_list"}
_SECTION_KEYS = {"name", "base", "size"}
_TOP_KEYS = {"board", "mcu", "rtos", "memory", "dma", "buses", "handlers", "budget", "linker", "errata"}


def camel(name):
    """`d2_dma` -> `D2Dma`: the C++ type name of a region, stream or bus."""
    return "".join(part[:1].upper() + part[1:] for part in name.split("_"))


def to_int(value):
    """An integer from an int, a decimal string or a hex string; None when it is none of them (bool is refused)."""
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value
    if isinstance(value, str):
        text = value.strip().replace("_", "")
        try:
            return int(text, 16) if text.lower().startswith("0x") else int(text, 10)
        except ValueError:
            return None
    return None


class _Issues:
    def __init__(self):
        self.items = []

    def add(self, subject, message):
        self.items.append((subject, message))


def _mapping(value, subject, issues, allowed=None):
    if not isinstance(value, dict):
        issues.add(subject, f"{subject} must be a mapping")
        return {}
    if allowed is not None:
        for key in sorted(value, key=str):
            if key not in allowed:
                issues.add(subject, f"{subject}: unknown key {key!r} (allowed: {', '.join(sorted(allowed))})")
    return value


def _names(mapping, subject, issues):
    for name in mapping:
        if not isinstance(name, str) or not NAME.match(name) or len(name) > NAME_MAX:
            issues.add(f"{subject}.{name}", f"{subject}: name {name!r} must be lower-case words joined by single "
                       f"underscores, at most {NAME_MAX} characters (it becomes a C++ identifier)")


def _int(value, subject, issues, *, minimum=None, maximum=None, required=False, what="an integer"):
    if value is None:
        if required:
            issues.add(subject, f"{subject} is required")
        return None
    number = to_int(value)
    if number is None:
        issues.add(subject, f"{subject} must be {what} (a number, or a hex string such as \"0x30000000\"; "
                   "expressions are not accepted)")
        return None
    if minimum is not None and number < minimum:
        issues.add(subject, f"{subject} must be at least {minimum}")
    if maximum is not None and number > maximum:
        issues.add(subject, f"{subject} must be at most {maximum}")
    return number


def _choice(value, choices, subject, issues, default=None):
    if value is None:
        return default
    text = str(value).strip().lower().replace("-", "_")
    if text == "veryhigh":
        text = "very_high"
    if text not in choices:
        issues.add(subject, f"{subject} must be one of {', '.join(choices)} (got {value!r})")
        return default
    return text


def _string(value, subject, issues, required=False):
    if value is None:
        if required:
            issues.add(subject, f"{subject} is required")
        return None
    if not isinstance(value, str) or not value:
        issues.add(subject, f"{subject} must be a non-empty string")
        return None
    return value


def _bool(value, subject, issues, default=False):
    if value is None:
        return default
    if not isinstance(value, bool):
        issues.add(subject, f"{subject} must be true or false")
        return default
    return value


def _region(name, raw, issues):
    s = f"memory.regions.{name}"
    raw = _mapping(raw, s, issues, _REGION_KEYS)
    masters = raw.get("masters")
    if masters is not None and (not isinstance(masters, list) or not all(isinstance(m, str) for m in masters)):
        issues.add(s, f"{s}.masters must be a list of master names")
        masters = None
    return {
        "base": _int(raw.get("base"), f"{s}.base", issues, minimum=0),
        "size": _int(raw.get("size"), f"{s}.size", issues, minimum=1),
        "cache": _choice(raw.get("cache"), CACHES, f"{s}.cache", issues, "none"),
        "line": _int(raw.get("line"), f"{s}.line", issues, minimum=1),
        "align": _int(raw.get("align"), f"{s}.align", issues, minimum=1),
        "mpu_region": _int(raw.get("mpu_region"), f"{s}.mpu_region", issues, minimum=0),
        "masters": None if masters is None else sorted(set(masters)),  # None: every master the memory allows
    }


def _fallback(raw, subject, issues):
    if raw is None:
        return None
    if isinstance(raw, str):
        raw = {"kind": raw}
    raw = _mapping(raw, subject, issues, _FALLBACK_KEYS)
    percent = raw.get("cpu_percent")
    if percent is not None and (isinstance(percent, bool) or not isinstance(percent, (int, float)) or percent < 0):
        issues.add(subject, f"{subject}.cpu_percent must be a number of at least 0")
        percent = None
    return {"kind": _choice(raw.get("kind"), FALLBACKS, f"{subject}.kind", issues), "cpu_percent": percent}


def _core(raw, subject, issues):
    if raw is None:
        return None
    if isinstance(raw, str):
        return [raw.lower()]
    if isinstance(raw, list) and raw and all(isinstance(c, str) for c in raw):
        return sorted({c.lower() for c in raw})
    issues.add(subject, f"{subject} must be a core name or a list of core names")
    return None


def _stream(name, raw, issues):
    s = f"dma.streams.{name}"
    raw = _mapping(raw, s, issues, _STREAM_KEYS)
    request = raw.get("request")
    if request is not None and (not isinstance(request, (str, int)) or isinstance(request, bool)):
        issues.add(s, f"{s}.request must be a request name or a request number")
        request = None
    out = {
        "engine": _string(raw.get("engine"), f"{s}.engine", issues),
        "stream": _int(raw.get("stream"), f"{s}.stream", issues, minimum=0),
        "request": request if not isinstance(request, str) else request.strip(),
        "priority": _choice(raw.get("priority"), PRIORITIES, f"{s}.priority", issues, "medium"),
        "irq_priority": _int(raw.get("irq_priority"), f"{s}.irq_priority", issues, minimum=0),
        "notify": _choice(raw.get("notify"), NOTIFY, f"{s}.notify", issues, "task"),
        "irq": _choice(raw.get("irq"), IRQ, f"{s}.irq", issues, "all"),
        "mode": _choice(raw.get("mode"), MODES, f"{s}.mode", issues, "normal"),
        "buffer": _string(raw.get("buffer"), f"{s}.buffer", issues),
        "frames": _int(raw.get("frames"), f"{s}.frames", issues, minimum=1),
        "element_bytes": _int(raw.get("element_bytes"), f"{s}.element_bytes", issues, minimum=1, maximum=8),
        "elements": _int(raw.get("elements"), f"{s}.elements", issues, minimum=1),
        "frames_per_pass": _int(raw.get("frames_per_pass"), f"{s}.frames_per_pass", issues, minimum=1),
        "trigger": _string(raw.get("trigger"), f"{s}.trigger", issues),
        "rate_hz": _int(raw.get("rate_hz"), f"{s}.rate_hz", issues, minimum=1),
        "stall_periods": _int(raw.get("stall_periods"), f"{s}.stall_periods", issues, minimum=1),
        "fallback": _fallback(raw.get("fallback"), f"{s}.fallback", issues),
        "core": _core(raw.get("core"), f"{s}.core", issues),
        "dmamux_sync": _bool(raw.get("dmamux_sync"), f"{s}.dmamux_sync", issues),
        "dmamux_request_generator": _bool(raw.get("dmamux_request_generator"), f"{s}.dmamux_request_generator", issues),
        "driver": _choice(raw.get("driver"), ESP_DRIVERS, f"{s}.driver", issues),
        "host": _string(raw.get("host"), f"{s}.host", issues),
        "frame_bytes": _int(raw.get("frame_bytes"), f"{s}.frame_bytes", issues, minimum=1),
        "iram_safe": _bool(raw.get("iram_safe"), f"{s}.iram_safe", issues),
        "half_duplex_both_phases": _bool(raw.get("half_duplex_both_phases"), f"{s}.half_duplex_both_phases", issues),
    }
    return out


def _bus(name, raw, issues):
    s = f"buses.{name}"
    raw = _mapping(raw, s, issues, _BUS_KEYS)
    dma = raw.get("dma", False)
    if isinstance(dma, list):
        if not all(isinstance(x, str) for x in dma):
            issues.add(s, f"{s}.dma must be true, false or a list of stream names")
            dma = []
        dma = sorted(set(dma))
    elif not isinstance(dma, bool):
        issues.add(s, f"{s}.dma must be true, false or a list of stream names")
        dma = False
    kind = raw.get("kind")
    if kind is None:
        match = re.match(r"^(spi|uart|i2c)", name)
        kind = match.group(1) if match else None
        if kind is None:
            issues.add(s, f"{s}.kind is required (spi, uart or i2c): it cannot be read from the name")
    else:
        kind = _choice(kind, BUS_KINDS, f"{s}.kind", issues)
    devices = []
    raw_devices = raw.get("devices", [])
    if not isinstance(raw_devices, list):
        issues.add(s, f"{s}.devices must be a list")
        raw_devices = []
    for i, dev in enumerate(raw_devices):
        if isinstance(dev, str):
            dev = {"driver": dev}
        dev = _mapping(dev, f"{s}.devices[{i}]", issues, _DEVICE_KEYS)
        driver = _string(dev.get("driver"), f"{s}.devices[{i}].driver", issues, required=True)
        devices.append({"driver": driver,
                        "max_frame_bytes": _int(dev.get("max_frame_bytes"), f"{s}.devices[{i}].max_frame_bytes",
                                                issues, minimum=1)})
    model = raw.get("model", "owner")
    if model not in ("owner", "guarded"):
        issues.add(s, f"{s}.model must be owner or guarded (ADR-0009)")
        model = "owner"
    return {"kind": kind, "model": model, "dma": dma,
            "staging_bytes": _int(raw.get("staging_bytes"), f"{s}.staging_bytes", issues, minimum=1),
            "devices": devices}


def normalise(raw):
    """(board, issues): the board with every default filled and every number an int, and the (subject, message)
    pairs of whatever is malformed. A board with issues must not be checked further: some fields are None."""
    issues = _Issues()
    top = _mapping(raw, "board", issues, _TOP_KEYS)
    out = {"board": _string(top.get("board"), "board", issues, required=True)}

    mcu = _mapping(top.get("mcu", {}), "mcu", issues, _MCU_KEYS)
    family = _choice(mcu.get("family"), FAMILIES, "mcu.family", issues)
    if family is None and mcu.get("family") is None:
        issues.add("mcu.family", "mcu.family is required (stm32 or esp32)")
    core = mcu.get("core")
    out["mcu"] = {"family": family, "device": _string(mcu.get("device"), "mcu.device", issues),
                  "target": _string(mcu.get("target"), "mcu.target", issues),
                  "core": core.lower() if isinstance(core, str) else None}
    if family == "stm32" and not out["mcu"]["device"]:
        issues.add("mcu.device", "mcu.device is required for an stm32 board")
    if family == "esp32" and not out["mcu"]["target"]:
        issues.add("mcu.target", "mcu.target is required for an esp32 board")

    rtos = top.get("rtos")
    if rtos is None:
        out["rtos"] = None
    else:
        rtos = _mapping(rtos, "rtos", issues, _RTOS_KEYS)
        out["rtos"] = {
            "name": _string(rtos.get("name"), "rtos.name", issues),
            "max_syscall_priority": _int(rtos.get("max_syscall_priority"), "rtos.max_syscall_priority", issues,
                                         minimum=0, required=True),
            "priority_bits": _int(rtos.get("priority_bits", 4), "rtos.priority_bits", issues, minimum=1, maximum=8),
        }

    memory = _mapping(top.get("memory", {}), "memory", issues, {"regions"})
    regions = _mapping(memory.get("regions", {}), "memory.regions", issues)
    _names(regions, "memory.regions", issues)
    out["regions"] = {n: _region(n, regions[n], issues) for n in sorted(regions, key=str) if isinstance(n, str)}

    dma = _mapping(top.get("dma", {}), "dma", issues, {"buffers", "streams"})
    buffers = _mapping(dma.get("buffers", {}), "dma.buffers", issues)
    _names(buffers, "dma.buffers", issues)
    out["buffers"] = {}
    for n in sorted(buffers, key=str):
        if not isinstance(n, str):
            continue
        b = _mapping(buffers[n], f"dma.buffers.{n}", issues, _BUFFER_KEYS)
        out["buffers"][n] = {"region": _string(b.get("region"), f"dma.buffers.{n}.region", issues, required=True),
                             "bytes": _int(b.get("bytes"), f"dma.buffers.{n}.bytes", issues, minimum=1, required=True,
                                           what="a whole number of bytes")}
    streams = _mapping(dma.get("streams", {}), "dma.streams", issues)
    _names(streams, "dma.streams", issues)
    out["streams"] = {n: _stream(n, streams[n], issues) for n in sorted(streams, key=str) if isinstance(n, str)}

    buses = _mapping(top.get("buses", {}), "buses", issues)
    _names(buses, "buses", issues)
    out["buses"] = {n: _bus(n, buses[n], issues) for n in sorted(buses, key=str) if isinstance(n, str)}

    handlers = _mapping(top.get("handlers", {}), "handlers", issues)
    _names(handlers, "handlers", issues)
    out["handlers"] = {}
    for n in sorted(handlers, key=str):
        if not isinstance(n, str):
            continue
        h = _mapping(handlers[n], f"handlers.{n}", issues, _HANDLER_KEYS)
        out["handlers"][n] = {
            "stream": _string(h.get("stream"), f"handlers.{n}.stream", issues, required=True),
            "frame_bytes": _int(h.get("frame_bytes"), f"handlers.{n}.frame_bytes", issues, minimum=1),
            "frames": _int(h.get("frames"), f"handlers.{n}.frames", issues, minimum=1),
            "conversions": _int(h.get("conversions"), f"handlers.{n}.conversions", issues, minimum=1),
        }

    budget = _mapping(top.get("budget", {}), "budget", issues, {"fallback_cpu_percent"})
    percent = budget.get("fallback_cpu_percent")
    if percent is not None and (isinstance(percent, bool) or not isinstance(percent, (int, float)) or percent < 0):
        issues.add("budget.fallback_cpu_percent", "budget.fallback_cpu_percent must be a number of at least 0")
        percent = None
    out["budget"] = {"fallback_cpu_percent": percent}

    linker = _mapping(top.get("linker", {}), "linker", issues, _LINKER_KEYS)
    sections = linker.get("fixed_sections", [])
    if not isinstance(sections, list):
        issues.add("linker.fixed_sections", "linker.fixed_sections must be a list")
        sections = []
    fixed = []
    for i, sec in enumerate(sections):
        sec = _mapping(sec, f"linker.fixed_sections[{i}]", issues, _SECTION_KEYS)
        fixed.append({"name": _string(sec.get("name"), f"linker.fixed_sections[{i}].name", issues, required=True),
                      "base": _int(sec.get("base"), f"linker.fixed_sections[{i}].base", issues, minimum=0,
                                   required=True),
                      "size": _int(sec.get("size"), f"linker.fixed_sections[{i}].size", issues, minimum=1,
                                   required=True)})
    prefix = linker.get("noload_list", "__kb_dma")
    if not isinstance(prefix, str) or not re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", prefix):
        issues.add("linker.noload_list", "linker.noload_list must be a symbol prefix (letters, digits, underscore)")
        prefix = "__kb_dma"
    out["linker"] = {"fixed_sections": fixed, "noload_list": prefix}

    errata = _mapping(top.get("errata", {}), "errata", issues, {"workarounds"})
    workarounds = errata.get("workarounds", [])
    if not isinstance(workarounds, list) or not all(isinstance(w, str) for w in workarounds):
        issues.add("errata.workarounds", "errata.workarounds must be a list of rule names")
        workarounds = []
    out["errata"] = {"workarounds": sorted(set(workarounds))}
    return out, issues.items


def load(path):
    """The raw board document of a YAML or JSON file."""
    return yamlio.load(path)
