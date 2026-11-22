# The DMA checker: rules R1 to R15 of ADR-0033 over a board.yml and the routing data. A pure function, so the
# configurator site can run the same rules (tests/resolver/vectors/board/ is the shared contract).
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""`check(board, data, sdkconfig=None, drivers=None)`: the findings and the resolved board.

No file is read and no global is used: the same arguments always give the same dictionary, findings sorted by rule
number then subject. See docs/design/board-dma.md for the board file, the codes and the result."""

import re

from . import board as boardmod

# code -> (rule, severity, what it says). The table is the contract: the vectors pin the codes.
CODES = {
    "dma.board_invalid": ("R0", "error", "The board file is malformed or lacks a field the rules need"),
    "dma.device_unknown": ("R0", "error", "No routing data for the device or target"),
    "dma.reference_unknown": ("R0", "error", "A name refers to a region, buffer, stream or bus that is not declared"),
    "dma.master_unknown": ("R0", "error", "A region lists a master the device does not have"),
    "dma.stream_used_twice": ("R1", "error", "Two streams use one stream or channel of one engine"),
    "dma.mux_channel_used_twice": ("R1", "error", "Two streams use one DMAMUX channel"),
    "dma.request_used_twice": ("R1", "error", "Two streams carry one request"),
    "dma.engine_unknown": ("R1", "error", "The engine is not one of the device's"),
    "dma.stream_out_of_range": ("R1", "error", "The stream or channel number is outside the engine's"),
    "dma.buffer_shared": ("R1", "error", "Two streams of different transfers share one buffer"),
    "dma.stream_in_two_buses": ("R1", "error", "One stream is listed by two buses"),
    "dma.request_unknown": ("R2", "error", "The request is in no routing table of the device"),
    "dma.request_not_routable": ("R2", "error", "The request cannot be routed to the chosen engine or stream"),
    "dma.region_unknown_memory": ("R3", "error", "The region is not inside a memory of the device"),
    "dma.region_unreachable": ("R3", "error", "The stream's engine cannot reach the memory of its buffer"),
    "dma.region_masters_missing": ("R3", "error", "The region does not list the stream's engine as a master"),
    "dma.region_masters_overclaim": ("R3", "error", "The region lists a master that cannot reach its memory"),
    "dma.bdma_buffer_outside_sram4": ("R4", "error", "A BDMA buffer is outside the D3 SRAM and backup memories"),
    "dma.region_line_invalid": ("R5", "error", "The region's line or alignment is not a power of two, or align < line"),
    "dma.region_line_below_cache_line": ("R5", "error", "A maintained region's line is below the core's cache line"),
    "dma.region_align_below_dma_unit": ("R5", "error", "The region's alignment is below what the DMA needs"),
    "dma.region_base_unaligned": ("R5", "error", "The region does not start on its alignment"),
    "dma.buffer_too_small": ("R5", "error", "A stream's ring does not fit its buffer"),
    "dma.buffer_not_element_multiple": ("R5", "error", "A buffer is not a whole number of the stream's elements"),
    "dma.spi_rx_length_multiple": ("R5", "error", "The SPI staging size is not a multiple of the receive rounding"),
    "dma.adc_frame_not_multiple": ("R5", "error", "The ADC frame is not a multiple of the conversion size"),
    "dma.buffer_in_cacheable_sdram": ("R6", "error", "A DMA buffer is in cacheable SDRAM"),
    "dma.noncacheable_without_mpu": ("R6", "error", "A cache: none region on a cached core has no MPU region"),
    "dma.region_cache_policy": ("R6", "error", "The region's cache policy does not suit the target's memory"),
    "dma.notify_above_rtos_limit": ("R7", "error", "A stream that notifies a task is more urgent than the RTOS allows"),
    "dma.priority_out_of_range": ("R7", "error", "An interrupt priority is outside the implemented bits"),
    "dma.peripheral_priority_mismatch": ("R7", "error", "The streams of one peripheral run at different priorities"),
    "dma.rtos_missing": ("R7", "error", "A stream notifies a task but the board declares no rtos"),
    "dma.rtos_priority_bits": ("R7", "error", "rtos.priority_bits differs from the device's implemented bits"),
    "dma.gdma_pairs_exceeded": ("R8", "error", "More GDMA pairs are needed than the target has"),
    "dma.dedicated_dma_exceeded": ("R8", "error", "More users of a dedicated DMA channel than the target has"),
    "dma.driver_without_dma": ("R8", "error", "The driver has no DMA on this target"),
    "dma.bus_without_stream": ("R8", "error", "A bus has DMA but no stream declares it, so its pair is not counted"),
    "dma.driver_frame_exceeds_bus": ("R9", "error", "A driver's largest frame exceeds what its bus carries"),
    "dma.staging_missing": ("R9", "error", "A bus with DMA has no staging size"),
    "dma.staging_exceeds_buffer": ("R9", "error", "The staging size is larger than a stream's buffer"),
    "dma.staging_too_large": ("R9", "error", "The staging size is above the target's largest DMA frame"),
    "dma.staging_too_small": ("R9", "error", "The staging size is below the target's smallest DMA frame"),
    "dma.handler_frame_mismatch": ("R10", "error", "A handler's frame size differs from the stream's"),
    "dma.handler_frames_mismatch": ("R10", "error", "A handler's frame count differs from the stream's"),
    "dma.handler_conversions_mismatch": ("R10", "error", "A handler's conversion count differs from the stream's"),
    "dma.stream_shape_invalid": ("R10", "error", "A stream's frames, element size or elements are inconsistent"),
    "dma.errata_f4_dma2_ahb_apb2": ("R11", "error", "DMA2 serves an AHB and an APB2 peripheral at once (F4)"),
    "dma.errata_dmamux_sync": ("R11", "error", "DMAMUX synchronisation or request generators are not supported"),
    "dma.errata_esp32_half_duplex_dma": ("R11", "error", "Half-duplex SPI DMA with both phases on the ESP32"),
    "dma.errata_stream_both_cores": ("R11", "error", "One stream is used from both cores"),
    "dma.stream_wrong_core": ("R12", "error", "A stream belongs to the other core than this image's"),
    "dma.cm4_buffer_in_sdram": ("R12", "error", "A CM4 DMA buffer is in external SDRAM (ES0445)"),
    "dma.buffer_core_cannot_reach": ("R12", "error", "The image's core cannot reach the buffer's memory"),
    "dma.iram_safe_option_missing": ("R13", "error", "iram_safe is declared without the matching sdkconfig option"),
    "dma.sdkconfig_not_checked": ("R13", "warning", "iram_safe is declared but no sdkconfig was given"),
    "dma.region_overlap": ("R14", "error", "Two regions overlap"),
    "dma.region_too_small": ("R14", "error", "The buffers of a region do not fit in it"),
    "dma.region_overlaps_fixed_section": ("R14", "error", "A region overlaps a fixed linker section"),
    "dma.mpu_size_not_power_of_two": ("R14", "error", "An MPU region's size is not a power of two"),
    "dma.mpu_base_not_aligned": ("R14", "error", "An MPU region does not start on its size"),
    "dma.mpu_size_too_small": ("R14", "error", "An MPU region is smaller than the MPU's minimum"),
    "dma.mpu_region_out_of_range": ("R14", "error", "The MPU region number is not one of the device's"),
    "dma.mpu_region_used_twice": ("R14", "error", "Two regions use one MPU region number"),
    "dma.fallback_cpu_over_budget": ("R15", "warning", "The declared fallbacks cost more CPU than the budget"),
}

# Master numbering of the ESP32 port (port-esp32 `kb/port/esp32/dma.hpp`); the STM32 numbering is in the device files.
ESP_MASTER_BITS = {"gdma": 0, "spi_dma": 1, "i2s_dma": 2, "axi_gdma": 3}
# The word every STM32 DMA stream needs a buffer aligned to (the widest item it moves).
STM32_DMA_UNIT = 4
PRIORITY_VALUES = {"low": 0, "medium": 1, "high": 2, "very_high": 3}
NOTIFY_VALUES = {"none": 0, "task": 1}
IRQ_VALUES = {"all": 0, "errors_only": 1}
MODE_VALUES = {"normal": 0, "circular": 1}
_SERIAL = re.compile(r"^([A-Z]+[0-9]+)_(RX|TX)$")


def _rule_key(rule):
    return int(rule[1:])


def _is_pow2(n):
    return isinstance(n, int) and n > 0 and (n & (n - 1)) == 0


def _num(value):
    return boardmod.to_int(value) if value is not None else None


def _hex(n):
    return f"0x{n:08X}" if n is not None else None


def parse_sdkconfig(text):
    """The options of an sdkconfig file as {name: value}: `CONFIG_X=y` gives "y", `# CONFIG_X is not set` gives "n"."""
    options = {}
    for raw in text.splitlines():
        line = raw.strip()
        if line.startswith("# CONFIG_") and line.endswith(" is not set"):
            options[line[2:-len(" is not set")]] = "n"
        elif line.startswith("CONFIG_") and "=" in line:
            name, _, value = line.partition("=")
            options[name] = value.strip().strip('"')
    return options


class _Ctx:
    def __init__(self, board, sdkconfig, drivers):
        self.b = board
        self.sdkconfig = sdkconfig
        self.drivers = drivers or {}
        self.findings = []
        self.masters = {}

    def add(self, code, subject, message):
        rule, severity, _ = CODES[code]
        self.findings.append({"rule": rule, "code": code, "severity": severity, "subject": subject,
                              "message": message})


def _driver_limit(ctx, device):
    """A device's largest frame: the board's number, else the driver catalogue entry (--drivers), else None."""
    if device.get("max_frame_bytes") is not None:
        return device["max_frame_bytes"]
    entry = ctx.drivers.get(device["driver"])
    return _num(entry.get("max_frame_bytes")) if isinstance(entry, dict) else None


def _devices(ctx, bus):
    return [{"driver": d["driver"], "max_frame_bytes": _driver_limit(ctx, d)} for d in bus["devices"]]


def _finish(ctx, resolved):
    findings = sorted(ctx.findings, key=lambda f: (_rule_key(f["rule"]), f["subject"], f["code"], f["message"]))
    return {"ok": not any(f["severity"] == "error" for f in findings), "findings": findings, "resolved": resolved}


def _empty_resolved():
    return {"regions": [], "buffers": [], "streams": [], "buses": [], "gdma_pairs": {"used": 0, "budget": 0},
            "mpu": [], "masters": {}, "cached_core": False}


def check(board, data, *, sdkconfig=None, drivers=None):
    """Check one board against the routing data.

    board     the board document (as loaded from YAML or JSON; it is normalised here)
    data      the routing data (`kb.dmadata.load_dir()` or the site's dma-data.json)
    sdkconfig {option: value} of the image's sdkconfig, or None (R13 then only warns)
    drivers   {driver name: {"max_frame_bytes": n}} for devices whose board entry gives no number, or None

    Returns {"ok", "findings", "resolved"}; a finding is {rule, code, severity, subject, message}."""
    b, issues = boardmod.normalise(board)
    ctx = _Ctx(b, sdkconfig, drivers)
    if issues:
        for subject, message in issues:
            ctx.add("dma.board_invalid", subject, message)
        return _finish(ctx, _empty_resolved())
    if b["mcu"]["family"] == "stm32":
        resolved = _stm32(ctx, data)
    else:
        resolved = _esp32(ctx, data)
    return _finish(ctx, resolved)


# ----------------------------------------------------------------------------------------------------------------
# Shared pieces

def _peripheral(request):
    match = _SERIAL.match(request) if isinstance(request, str) else None
    return match.group(1) if match else None


def _check_references(ctx):
    """Every name that points at another declaration must find it (R0). Returns False when one does not."""
    b = ctx.b
    ok = True
    for name, buf in b["buffers"].items():
        if buf["region"] not in b["regions"]:
            ctx.add("dma.reference_unknown", f"buffer:{name}", f"buffer {name} is in region {buf['region']!r}, which "
                    "memory.regions does not declare")
            ok = False
    for name, st in b["streams"].items():
        if st["buffer"] is not None and st["buffer"] not in b["buffers"]:
            ctx.add("dma.reference_unknown", f"stream:{name}", f"stream {name} uses buffer {st['buffer']!r}, which "
                    "dma.buffers does not declare")
            ok = False
    for name, bus in b["buses"].items():
        if isinstance(bus["dma"], list):
            for s in bus["dma"]:
                if s not in b["streams"]:
                    ctx.add("dma.reference_unknown", f"bus:{name}", f"bus {name} lists stream {s!r}, which dma.streams "
                            "does not declare")
                    ok = False
    for name, h in b["handlers"].items():
        if h["stream"] not in b["streams"]:
            ctx.add("dma.reference_unknown", f"handler:{name}", f"handler {name} reads stream {h['stream']!r}, which "
                    "dma.streams does not declare")
            ok = False
    return ok


def _shape(ctx, name, st):
    """(frames, element_bytes, elements) of a stream, or None when it declares none; R10 on an inconsistent one."""
    parts = (st["frames"], st["element_bytes"], st["elements"])
    if all(p is None for p in parts):
        return None
    if any(p is None for p in parts):
        ctx.add("dma.stream_shape_invalid", f"stream:{name}", f"stream {name}: frames, element_bytes and elements go "
                "together (give all three or none)")
        return None
    if st["mode"] == "circular" and st["frames"] < 2:
        ctx.add("dma.stream_shape_invalid", f"stream:{name}", f"stream {name}: a circular stream needs at least 2 "
                "frames (half and complete interrupts); a ring of one frame cannot be drained safely")
    if st["frames_per_pass"] is not None and st["frames_per_pass"] > st["frames"]:
        ctx.add("dma.stream_shape_invalid", f"stream:{name}", f"stream {name}: frames_per_pass "
                f"{st['frames_per_pass']} is above frames {st['frames']}")
    return parts


def _check_handlers(ctx, shapes, esp_conv_bytes=None):
    for name, h in ctx.b["handlers"].items():
        shape = shapes.get(h["stream"])
        if shape is None:
            continue
        frames, element_bytes, elements = shape
        if h["frame_bytes"] is not None and h["frame_bytes"] != element_bytes * elements:
            ctx.add("dma.handler_frame_mismatch", f"handler:{name}", f"handler {name} expects frames of "
                    f"{h['frame_bytes']} bytes but stream {h['stream']} carries {element_bytes * elements} "
                    f"({elements} x {element_bytes}); change one of them")
        if h["frames"] is not None and h["frames"] != frames:
            ctx.add("dma.handler_frames_mismatch", f"handler:{name}", f"handler {name} expects a ring of "
                    f"{h['frames']} frames but stream {h['stream']} has {frames}; change one of them")
        if h["conversions"] is not None and h["conversions"] != elements:
            ctx.add("dma.handler_conversions_mismatch", f"handler:{name}", f"handler {name} converts "
                    f"{h['conversions']} channels per frame but stream {h['stream']} carries {elements} elements; "
                    "the circular buffer would wrap on the wrong boundary")


def _check_fallback_budget(ctx):
    budget = ctx.b["budget"]["fallback_cpu_percent"]
    if budget is None:
        return
    total = 0.0
    for st in ctx.b["streams"].values():
        fb = st["fallback"]
        if fb and fb["cpu_percent"] is not None:
            total += fb["cpu_percent"]
    if total > budget:
        ctx.add("dma.fallback_cpu_over_budget", "budget", f"the declared fallbacks cost {total:g} percent of the CPU, "
                f"above budget.fallback_cpu_percent {budget:g}; raise the budget or give the stream DMA")


def _check_shared_buffers(ctx):
    users = {}
    for name, st in ctx.b["streams"].items():
        if st["buffer"] is not None:
            users.setdefault(st["buffer"], []).append(name)
    for buf in sorted(users):
        if len(users[buf]) > 1:
            ctx.add("dma.buffer_shared", f"buffer:{buf}", f"buffer {buf} is used by streams "
                    f"{', '.join(sorted(users[buf]))}: one staging buffer per stream, or two transfers race on it")


def _check_bus_streams(ctx):
    seen = {}
    for name, bus in ctx.b["buses"].items():
        if isinstance(bus["dma"], list):
            for s in bus["dma"]:
                if s in seen and seen[s] != name:
                    ctx.add("dma.stream_in_two_buses", f"stream:{s}", f"stream {s} is listed by buses {seen[s]} and "
                            f"{name}")
                seen.setdefault(s, name)


# ----------------------------------------------------------------------------------------------------------------
# STM32

def _memories(dev):
    reach = dev.get("cpu_reach") or {}
    out = {}
    for name, m in (dev.get("memories") or {}).items():
        cores = m.get("cores")
        if cores is None and reach:
            cores = [c for c in sorted(reach) if name in (reach[c] or [])]
        out[name] = {"base": _num(m.get("base")), "size": _num(m.get("size")),
                     "masters": sorted(m.get("masters") or []), "cached": bool(m.get("cached", False)),
                     "external": bool(m.get("external", False)), "cores": cores}
    return out


def _memory_of(mems, base, size):
    """(name, memory) of the device memory holding [base, base+size), or (None, None)."""
    if base is None or size is None:
        return None, None
    for name in sorted(mems):
        m = mems[name]
        if m["base"] is not None and m["size"] is not None and m["base"] <= base and base + size <= m["base"] + m["size"]:
            return name, m
    return None, None


def _cores(dev):
    """(cores, cores with a data cache, data cache line of each core in bytes)."""
    cores = [str(c).lower() for c in (dev.get("cores") or ["cm"])]
    by_core = {str(k).lower(): _num(v) or 0 for k, v in (dev.get("dcache_line_by_core") or {}).items()}
    if not by_core:
        by_core = {cores[0]: _num(dev.get("dcache_line")) or 0}
    return cores, [c for c in cores if by_core.get(c, 0) > 0], by_core


def _erratum(dev, rule, core=None):
    """The data's erratum entry for `rule` (that applies to `core`, when given), or None."""
    for e in dev.get("errata") or []:
        if e.get("rule") == rule and (core is None or core in [str(c).lower() for c in (e.get("applies_to") or [core])]):
            return e
    return None


def _stm32_regions(ctx, dev, mems, engines, cached_core, dline):
    """Resolve each region: policy, line, align, masters, physical memory. Rules R3 (masters), R5, R6 (region part)."""
    b = ctx.b
    out = {}
    master_names = {e["master_bit"]: n for n, e in engines.items()}
    for name, r in b["regions"].items():
        mem_name, mem = _memory_of(mems, r["base"], r["size"])
        cache = r["cache"]
        if cache == "maintained":
            policy = "Maintained"
        elif cache == "coherent":
            policy = "Coherent"
        else:
            policy = "NonCacheable" if cached_core else "Coherent"
        line = r["line"] if r["line"] is not None else ((dline or 32) if policy == "Maintained" else 1)
        unit = dline if (cached_core and dline) else STM32_DMA_UNIT
        align = r["align"] if r["align"] is not None else max(line, unit)
        declared = r["masters"]
        physical = mem["masters"] if mem else []
        if declared is None:
            masters = sorted(physical)
        else:
            masters = declared
            for m in declared:
                if m not in engines:
                    ctx.add("dma.master_unknown", f"region:{name}", f"region {name} lists master {m!r}; this device "
                            f"has {', '.join(sorted(engines))}")
            if mem:
                for m in declared:
                    if m in engines and m not in physical:
                        ctx.add("dma.region_masters_overclaim", f"region:{name}", f"region {name} lists {m} as a "
                                f"master, but the memory {mem_name} cannot be reached by it (the device's bus "
                                f"matrix allows {', '.join(physical) or 'no DMA master'})")
        if r["base"] is not None and mem is None:
            ctx.add("dma.region_unknown_memory", f"region:{name}", f"region {name} ({_hex(r['base'])}, "
                    f"{r['size']} bytes) is not inside one memory of the device; declare it inside "
                    f"{', '.join(sorted(mems))}")
        out[name] = {"name": name, "base": r["base"], "size": r["size"], "cache": cache, "policy": policy,
                     "line": line, "align": align, "mpu_region": r["mpu_region"], "masters": sorted(masters),
                     "master_bits": sum(1 << engines[m]["master_bit"] for m in masters if m in engines),
                     "memory": mem_name, "cached_memory": bool(mem and mem["cached"]),
                     "external": bool(mem and mem["external"]), "_mem": mem}
        # R5: line and alignment
        if not _is_pow2(line) or not _is_pow2(align) or align < line:
            ctx.add("dma.region_line_invalid", f"region:{name}", f"region {name}: line {line} and align {align} must "
                    "be powers of two with align at least line")
        elif policy == "Maintained" and cached_core and dline and line < dline:
            ctx.add("dma.region_line_below_cache_line", f"region:{name}", f"region {name}: the cache line is "
                    f"{line} bytes but the core's data cache line is {dline}; an invalidate would reach the "
                    f"neighbours (set line: {dline})")
        if _is_pow2(align) and align < STM32_DMA_UNIT:
            ctx.add("dma.region_align_below_dma_unit", f"region:{name}", f"region {name}: align {align} is below the "
                    f"{STM32_DMA_UNIT} bytes the DMA needs for word transfers")
        if r["base"] is not None and _is_pow2(align) and r["base"] % align:
            ctx.add("dma.region_base_unaligned", f"region:{name}", f"region {name} starts at {_hex(r['base'])}, not "
                    f"on its alignment of {align} bytes")
    return out


def _mux_channel(engines, ename, unit):
    e = engines[ename]
    group = e.get("mux")
    first = _num(e.get("first")) or 0
    if _num(e.get("mux_channel_base")) is not None:
        return _num(e["mux_channel_base"]) + (unit - first)
    offset = 0
    for other in sorted(engines, key=lambda n: _num(engines[n].get("id")) or 0):
        if other == ename:
            break
        if engines[other].get("mux") == group and group not in (None, "none"):
            offset += _num(engines[other].get("count")) or 0
    return offset + (unit - first)


def _stm32_streams(ctx, dev, engines):
    """R1 (engine, range, duplicates, mux channels) and R2 (routing); returns the resolved stream entries."""
    b = ctx.b
    routing = dev.get("routing", "mux")
    out = {}
    used = {}
    requests = {}
    channels = {}
    for name, st in b["streams"].items():
        s = f"stream:{name}"
        rs = {"name": name, "engine": st["engine"], "engine_id": 0, "unit": st["stream"], "request": None,
              "request_line": None, "master_bit": None, "mux": None, "mux_channel": None, "peripheral": None,
              "routable": False}
        for key in ("priority", "irq_priority", "notify", "irq", "mode", "buffer", "trigger", "rate_hz",
                    "stall_periods", "frames_per_pass", "fallback"):
            rs[key] = st[key]
        out[name] = rs
        eng = engines.get(st["engine"])
        if eng is None:
            ctx.add("dma.engine_unknown", s, f"stream {name}: this device has no engine {st['engine']!r} "
                    f"(it has {', '.join(sorted(engines))})")
            continue
        rs["engine_id"] = _num(eng.get("id"))
        rs["master_bit"] = _num(eng.get("master_bit"))
        first, count = _num(eng.get("first")) or 0, _num(eng.get("count")) or 0
        unit = st["stream"]
        if unit < first or unit >= first + count:
            noun = "channel" if eng.get("units") == "channels" else "stream"
            ctx.add("dma.stream_out_of_range", s, f"stream {name}: {st['engine']} has {noun}s {first} to "
                    f"{first + count - 1}, not {unit}")
            continue
        key = (st["engine"], unit)
        if key in used:
            ctx.add("dma.stream_used_twice", f"{st['engine']}/{unit}", f"streams {used[key]} and {name} both use "
                    f"{st['engine']} {unit}; give one of them another stream")
        used.setdefault(key, name)
        group = eng.get("mux")
        if group not in (None, "none"):
            rs["mux"] = group
            ch = _mux_channel(engines, st["engine"], unit)
            rs["mux_channel"] = ch
            if (group, ch) in channels and channels[(group, ch)][0] != key:
                ctx.add("dma.mux_channel_used_twice", f"{group}/{ch}", f"streams {channels[(group, ch)][1]} and "
                        f"{name} map to {group} channel {ch}")
            channels.setdefault((group, ch), (key, name))
        # R2: routing
        req = st["request"]
        if req is None:
            ctx.add("dma.board_invalid", s, f"stream {name}: request is required")
            continue
        _route(ctx, dev, routing, eng, name, st, rs, req)
        if rs["request"] is not None:
            rs["peripheral"] = _peripheral(rs["request"])
            if rs["request"] in requests:
                ctx.add("dma.request_used_twice", f"request:{rs['request']}", f"streams {requests[rs['request']]} and "
                        f"{name} both carry {rs['request']}")
            requests.setdefault(rs["request"], name)
    return out


def _route(ctx, dev, routing, eng, name, st, rs, req):
    ename, unit = st["engine"], st["stream"]
    if routing == "fixed":
        table = dev.get("fixed") or {}
        if not isinstance(req, str):
            ctx.add("dma.request_unknown", f"stream:{name}", f"stream {name}: this family selects the request by "
                    "channel table, name it (for example SPI1_RX)")
            return
        entries = table.get(req)
        if entries is None:
            ctx.add("dma.request_unknown", f"stream:{name}", f"stream {name}: {req} is in no request table of this "
                    "device")
            return
        rs["request"] = req
        match = [e for e in entries if e.get("engine") == ename and _num(e.get("unit")) == unit]
        if not match:
            options = ", ".join(f"{e['engine']} {_num(e['unit'])}" for e in entries)
            ctx.add("dma.request_not_routable", f"stream:{name}", f"stream {name}: {req} cannot be routed to {ename} "
                    f"{unit}; the reference manual's table offers {options}")
            return
        rs["request_line"] = _num(match[0].get("select"))
        rs["routable"] = True
        return
    group = eng.get("mux")
    tables = dev.get("mux") or {}
    if group in (None, "none"):
        ctx.add("dma.request_not_routable", f"stream:{name}", f"stream {name}: {ename} carries no peripheral "
                "requests in the routing data")
        return
    here = tables.get(group) or {}
    if isinstance(req, int):
        names = [n for n, line in here.items() if _num(line) == req]
        if not names:
            ctx.add("dma.request_unknown", f"stream:{name}", f"stream {name}: request line {req} is not in {group}")
            return
        req = sorted(names)[0]
    if req in here:
        rs["request"] = req
        rs["request_line"] = _num(here[req])
        rs["routable"] = True
        return
    others = sorted(g for g in tables if g != group and req in (tables[g] or {}))
    if others:
        ctx.add("dma.request_not_routable", f"stream:{name}", f"stream {name}: {req} is a {others[0]} request and "
                f"{ename} is served by {group}; use an engine of {others[0]}")
        return
    ctx.add("dma.request_unknown", f"stream:{name}", f"stream {name}: {req} is in no request table of this device")


def _stm32(ctx, data):
    from . import dmadata

    b = ctx.b
    dev = dmadata.find_device(data, b["mcu"]["device"])
    if dev is None:
        ctx.add("dma.device_unknown", "mcu", f"no routing data for the device {b['mcu']['device']}; the data folder "
                "has " + (", ".join(sorted(data.get("stm32", {}))) or "no STM32 device"))
        return _empty_resolved()
    cores, dcache_cores, dlines = _cores(dev)
    core = b["mcu"]["core"] or cores[0]
    if core not in cores:
        ctx.add("dma.board_invalid", "mcu.core", f"mcu.core {core!r}: this device has the cores {', '.join(cores)}")
        return _empty_resolved()
    cached_core = core in dcache_cores
    dline = dlines.get(core, 0)
    engines = dev.get("engines") or {}
    ctx.masters = {n: _num(e.get("master_bit")) for n, e in engines.items()}
    mems = _memories(dev)
    bits = _num(dev.get("nvic_priority_bits")) or 4
    refs_ok = _check_references(ctx)
    for name, st in b["streams"].items():
        for key in ("engine", "stream", "buffer", "irq_priority"):
            if st[key] is None:
                ctx.add("dma.board_invalid", f"stream:{name}", f"stream {name}: {key} is required on an stm32 board")
                refs_ok = False
    regions = _stm32_regions(ctx, dev, mems, engines, cached_core, dline)
    if not refs_ok:
        return _resolve_partial(ctx, regions, {})
    streams = _stm32_streams(ctx, dev, engines)
    _check_shared_buffers(ctx)
    _check_bus_streams(ctx)

    # R3 / R4 / R6 / R12 per stream buffer
    seen_region_checks = set()
    for name, st in b["streams"].items():
        rs = streams[name]
        buf = b["buffers"][st["buffer"]]
        reg = regions[buf["region"]]
        eng = engines.get(st["engine"])
        if eng is None:
            continue
        mem = reg["_mem"]
        s = f"stream:{name}"
        if st["engine"] == "bdma":
            if mem is not None and "bdma" not in mem["masters"]:
                ctx.add("dma.bdma_buffer_outside_sram4", s, f"stream {name} is on bdma but its buffer "
                        f"{st['buffer']} is in {reg['memory']}; the BDMA reaches only SRAM4 and the backup memory")
            elif mem is not None and "bdma" not in reg["masters"]:
                ctx.add("dma.region_masters_missing", s, f"region {buf['region']} does not list bdma in masters")
        elif mem is not None and st["engine"] not in mem["masters"]:
            ctx.add("dma.region_unreachable", s, f"stream {name} is on {st['engine']}, which cannot reach "
                    f"{reg['memory']} (buffer {st['buffer']}, region {buf['region']}); the bus matrix allows "
                    f"{', '.join(mem['masters']) or 'no DMA master'}")
        elif mem is not None and st["engine"] not in reg["masters"]:
            ctx.add("dma.region_masters_missing", s, f"stream {name} is on {st['engine']} but region "
                    f"{buf['region']} does not list it in masters ({', '.join(reg['masters']) or 'none'})")
        # R12: the image's core and the stream's core
        stream_cores = st["core"] or [core]
        unknown = [c for c in stream_cores if c not in cores]
        if unknown:
            ctx.add("dma.stream_wrong_core", s, f"stream {name} names core {unknown[0]}, which this device does not "
                    f"have (it has {', '.join(cores)})")
        elif len(stream_cores) > 1:
            ctx.add("dma.errata_stream_both_cores", s, f"stream {name} is declared for cores "
                    f"{', '.join(stream_cores)}: one stream belongs to one core (its interrupt and its flags)")
        elif stream_cores[0] != core:
            ctx.add("dma.stream_wrong_core", s, f"stream {name} belongs to core {stream_cores[0]} but this image "
                    f"runs on {core}; declare it in the other core's board")
        if mem is not None and mem["external"] and _erratum(dev, "cm4_sdram_byte_access", core):
            ctx.add("dma.cm4_buffer_in_sdram", s, f"stream {name}: buffer {st['buffer']} is in {reg['memory']}, "
                    "external SDRAM, which the CM4 reads wrongly for bytes and half-words (ES0445); use D2 SRAM")
        elif mem is not None and mem["cores"] and core not in [str(c).lower() for c in mem["cores"]]:
            ctx.add("dma.buffer_core_cannot_reach", s, f"stream {name}: core {core} cannot reach {reg['memory']} "
                    f"(buffer {st['buffer']})")
        # R5: buffer against the ring
        shape = _shape(ctx, name, st)
        if shape:
            frames, ebytes, elements = shape
            if buf["bytes"] < frames * ebytes * elements:
                ctx.add("dma.buffer_too_small", s, f"stream {name}: {frames} frames of {elements} x {ebytes} bytes "
                        f"need {frames * ebytes * elements} bytes but buffer {st['buffer']} has {buf['bytes']}")
            if buf["bytes"] % ebytes:
                ctx.add("dma.buffer_not_element_multiple", s, f"stream {name}: buffer {st['buffer']} "
                        f"({buf['bytes']} bytes) is not a whole number of {ebytes}-byte elements")
        # R6 once per region that holds a DMA buffer
        if buf["region"] not in seen_region_checks:
            seen_region_checks.add(buf["region"])
            _stm32_region_cache(ctx, reg, buf["region"], cached_core)

    # R7 interrupt priorities
    _stm32_priorities(ctx, streams, bits)
    # R11 errata
    _stm32_errata(ctx, dev, streams)
    # R14 regions, MPU
    _check_regions_layout(ctx, regions, dev, cached_core)
    shapes = {}
    for name, st in b["streams"].items():
        parts = (st["frames"], st["element_bytes"], st["elements"])
        shapes[name] = parts if all(p is not None for p in parts) else None
    _check_handlers(ctx, shapes)
    _check_fallback_budget(ctx)
    buses = _stm32_buses(ctx, streams, regions)
    return _resolve_final(ctx, dev, regions, streams, buses, cached_core, {"used": 0, "budget": 0})


def _stm32_region_cache(ctx, reg, name, cached_core):
    if not cached_core:
        return  # no data cache on this core: nothing to hold the lines
    if reg["external"] and reg["cached_memory"] and (reg["policy"] == "Maintained" or reg["mpu_region"] is None):
        ctx.add("dma.buffer_in_cacheable_sdram", f"region:{name}", f"region {name} holds a DMA buffer but sits in "
                f"cacheable external memory ({reg['memory']}); cover it with a non-cacheable MPU region "
                "(cache: none and mpu_region) or move the buffer to internal SRAM")
    elif reg["cache"] == "none" and reg["cached_memory"] and reg["mpu_region"] is None:
        ctx.add("dma.noncacheable_without_mpu", f"region:{name}", f"region {name} is cache: none on a core with a "
                "data cache but names no mpu_region, so the cache would hold its lines; give it the MPU region "
                "that makes it non-cacheable, or use cache: maintained")


def _stm32_priorities(ctx, streams, bits):
    b = ctx.b
    rtos = b["rtos"]
    levels = 1 << bits
    if rtos and rtos["priority_bits"] != bits:
        ctx.add("dma.rtos_priority_bits", "rtos", f"rtos.priority_bits is {rtos['priority_bits']} but the device "
                f"implements {bits}")
    groups = {}
    for name in sorted(b["streams"]):
        st = b["streams"][name]
        s = f"stream:{name}"
        p = st["irq_priority"]
        if p >= levels:
            ctx.add("dma.priority_out_of_range", s, f"stream {name}: irq_priority {p} is outside the {bits} "
                    f"implemented bits (0 to {levels - 1})")
        if st["notify"] == "task":
            if rtos is None:
                ctx.add("dma.rtos_missing", s, f"stream {name} notifies a task but the board declares no rtos limit")
            elif p < rtos["max_syscall_priority"]:
                ctx.add("dma.notify_above_rtos_limit", s, f"stream {name} notifies a task at irq_priority {p}, "
                        f"more urgent than the RTOS limit {rtos['max_syscall_priority']} (lower is more urgent); "
                        "use notify: none, or priority "
                        f"{rtos['max_syscall_priority']} or above")
        per = streams[name].get("peripheral")
        if per:
            groups.setdefault(per, []).append(name)
    for per in sorted(groups):
        prios = {n: b["streams"][n]["irq_priority"] for n in groups[per]}
        if len(set(prios.values())) > 1:
            detail = ", ".join(f"{n} {p}" for n, p in sorted(prios.items()))
            ctx.add("dma.peripheral_priority_mismatch", f"peripheral:{per}", f"the streams of {per} run at "
                    f"different interrupt priorities ({detail}); give them one value so error and completion cannot "
                    "preempt each other")


def _stm32_errata(ctx, dev, streams):
    b = ctx.b
    workarounds = set(b["errata"]["workarounds"])
    for entry in dev.get("errata") or []:
        rule = entry.get("rule")
        if rule == "f4_dma2_ahb_apb2" and rule not in workarounds:
            ahb = {str(p).upper() for p in (entry.get("ahb_requests") or entry.get("ahb") or [])}
            apb = {str(p).upper() for p in (entry.get("apb2_requests") or entry.get("apb2") or [])}
            on_ahb = sorted({rs["request"] for rs in streams.values() if rs["engine"] == "dma2" and _in_group(rs, ahb)})
            on_apb = sorted({rs["request"] for rs in streams.values() if rs["engine"] == "dma2" and _in_group(rs, apb)})
            if on_ahb and on_apb:
                ctx.add("dma.errata_f4_dma2_ahb_apb2", "dma2", f"dma2 serves the AHB request(s) "
                        f"{', '.join(on_ahb)} and the APB2 request(s) {', '.join(on_apb)} ({entry.get('id')}: "
                        "data corruption when both run concurrently); move one group to dma1, or list "
                        "f4_dma2_ahb_apb2 under errata.workarounds once the workaround is in place")
    if any(e.get("rule") == "no_dmamux_sync" for e in dev.get("errata") or []):
        for name in sorted(b["streams"]):
            st = b["streams"][name]
            generator = isinstance(st["request"], str) and st["request"].startswith("GENERATOR")
            if st["dmamux_sync"] or st["dmamux_request_generator"] or generator:
                ctx.add("dma.errata_dmamux_sync", f"stream:{name}", f"stream {name} uses DMAMUX synchronisation or "
                        "a request generator, whose flags the errata sheet lists as unreliable and the port does "
                        "not handle")


def _in_group(rs, group):
    """True when the stream's request is one of `group` (exact request names, or a peripheral name as prefix)."""
    name = (rs["request"] or "").upper()
    return any(name == p or name.startswith(p + "_") for p in group)


def _check_regions_layout(ctx, regions, dev, cached_core):
    """R14: overlaps, fixed sections, MPU rules."""
    b = ctx.b
    placed = [(n, r) for n, r in sorted(regions.items()) if r["base"] is not None and r["size"] is not None]
    for i, (n1, r1) in enumerate(placed):
        for n2, r2 in placed[i + 1:]:
            if r1["base"] < r2["base"] + r2["size"] and r2["base"] < r1["base"] + r1["size"]:
                ctx.add("dma.region_overlap", f"region:{n1}", f"regions {n1} and {n2} overlap "
                        f"({_hex(r1['base'])} + {r1['size']} and {_hex(r2['base'])} + {r2['size']})")
    for n, r in placed:
        for sec in b["linker"]["fixed_sections"]:
            if r["base"] < sec["base"] + sec["size"] and sec["base"] < r["base"] + r["size"]:
                ctx.add("dma.region_overlaps_fixed_section", f"region:{n}", f"region {n} overlaps the fixed "
                        f"linker section {sec['name']} ({_hex(sec['base'])} + {sec['size']})")
    for n, r in placed:
        total = 0
        for buf in b["buffers"].values():
            if buf["region"] == n and _is_pow2(r["align"]):
                total += ((buf["bytes"] + r["align"] - 1) // r["align"]) * r["align"]
        if total > r["size"]:
            ctx.add("dma.region_too_small", f"region:{n}", f"the buffers of region {n} need {total} bytes (padded to "
                    f"{r['align']}) but the region has {r['size']}")
    mpu = dev.get("mpu") or {}
    count = _num(mpu.get("regions"))
    min_size = _num(mpu.get("min_size")) or 32
    numbers = {}
    for n, r in sorted(regions.items()):
        num = r["mpu_region"]
        if num is None:
            continue
        s = f"region:{n}"
        if count is not None and num >= count:
            ctx.add("dma.mpu_region_out_of_range", s, f"region {n}: mpu_region {num} but the device has {count} MPU "
                    f"regions (0 to {count - 1})")
        if num in numbers:
            ctx.add("dma.mpu_region_used_twice", s, f"regions {numbers[num]} and {n} both use MPU region {num}")
        numbers.setdefault(num, n)
        size, base = r["size"], r["base"]
        if size is None or base is None:
            ctx.add("dma.board_invalid", s, f"region {n}: an MPU region needs base and size")
            continue
        if not _is_pow2(size):
            ctx.add("dma.mpu_size_not_power_of_two", s, f"region {n}: size {size} is not a power of two, so no one "
                    "MPU region can cover it (AN4838); round it to a power of two")
        else:
            if size < min_size:
                ctx.add("dma.mpu_size_too_small", s, f"region {n}: size {size} is below the MPU minimum of {min_size}")
            if base % size:
                ctx.add("dma.mpu_base_not_aligned", s, f"region {n}: base {_hex(base)} is not a multiple of its size "
                        f"{size}; an MPU region starts on its size")


def _stm32_buses(ctx, streams, regions):
    b = ctx.b
    out = []
    for name in sorted(b["buses"]):
        bus = b["buses"][name]
        listed = bus["dma"] if isinstance(bus["dma"], list) else []
        mode = "dma" if (bus["dma"] is True or listed) else "irq"
        staging = bus["staging_bytes"]
        if mode == "dma":
            cap = [b["buffers"][b["streams"][s]["buffer"]]["bytes"] for s in listed]
            if staging is None and cap:
                staging = min(cap)
            if staging is None:
                ctx.add("dma.staging_missing", f"bus:{name}", f"bus {name} has DMA but no staging_bytes and no "
                        "stream list to take the size from")
            elif cap and staging > min(cap):
                ctx.add("dma.staging_exceeds_buffer", f"bus:{name}", f"bus {name}: staging_bytes {staging} is above "
                        f"the smallest stream buffer ({min(cap)} bytes)")
        aligns = [regions[b["buffers"][b["streams"][s]["buffer"]]["region"]]["align"] for s in listed]
        limits = {"dma": mode == "dma", "max_frame_dma": staging if mode == "dma" and staging else 0,
                  "max_frame_no_dma": None, "rx_length_multiple": 1, "align_bytes": max(aligns) if aligns else
                  STM32_DMA_UNIT}
        for dev in bus["devices"]:
            limit = _driver_limit(ctx, dev)
            if mode == "dma" and staging and limit is not None and limit > staging:
                ctx.add("dma.driver_frame_exceeds_bus", f"bus:{name}/{dev['driver']}", f"driver {dev['driver']} on "
                        f"bus {name} sends frames of {limit} bytes but the DMA staging carries {staging}; raise "
                        "the staging buffer or let the bus use interrupts")
        out.append({"name": name, "kind": bus["kind"], "model": bus["model"], "mode": mode, "streams": listed,
                    "limits": limits, "devices": _devices(ctx, bus), "staging_bytes": staging})
    return out


def _resolve_partial(ctx, regions, streams):
    return _resolve_final(ctx, None, regions, streams, [], False, {"used": 0, "budget": 0})


def _resolve_final(ctx, dev, regions, streams, buses, cached_core, gdma):
    b = ctx.b
    reg_list = []
    for name in sorted(regions):
        r = dict(regions[name])
        r.pop("_mem", None)
        r["base_hex"] = _hex(r["base"])
        reg_list.append(r)
    buf_list = []
    for name in sorted(b["buffers"]):
        buf = b["buffers"][name]
        reg = regions.get(buf["region"])
        if reg is None:
            continue
        align = reg["align"] if _is_pow2(reg["align"]) else 1
        capacity = ((buf["bytes"] + align - 1) // align) * align
        buf_list.append({"name": name, "region": buf["region"], "bytes": buf["bytes"], "capacity": capacity,
                         "align": reg["align"], "section": f".kb_dma.{buf['region']}"})
    stream_list = []
    for name in sorted(streams):
        rs = dict(streams[name])
        st = b["streams"][name]
        if all(st[k] is not None for k in ("frames", "element_bytes", "elements")):
            rs["shape"] = {"frames": st["frames"], "element_bytes": st["element_bytes"], "elements": st["elements"]}
        stream_list.append(rs)
    mpu = []
    if dev is not None and cached_core and dev.get("mpu"):
        for name in sorted(regions):
            r = regions[name]
            if r["mpu_region"] is None or r["base"] is None:
                continue
            if r["policy"] == "NonCacheable":
                attrs = {"tex": 1, "c": 0, "b": 0, "s": 1, "xn": 1}
            elif r["policy"] == "Maintained":
                attrs = {"tex": 1, "c": 1, "b": 1, "s": 0, "xn": 1}
            else:
                attrs = {"tex": 0, "c": 1, "b": 1, "s": 1, "xn": 1}
            mpu.append({"region": name, "number": r["mpu_region"], "base": r["base"], "size": r["size"], **attrs})
        mpu.sort(key=lambda m: m["number"])
    return {"regions": reg_list, "buffers": buf_list, "streams": stream_list, "buses": buses,
            "gdma_pairs": gdma, "mpu": mpu, "masters": dict(sorted(ctx.masters.items())),
            "cached_core": bool(cached_core)}


# ----------------------------------------------------------------------------------------------------------------
# ESP32

def _esp_target(t):
    """The facts the rules read from one target of esp32/targets.yml (tools/kb/data/dma/README.md): GDMA groups, the
    drivers that take a pair or a dedicated channel, receive rounding, alignment, cache line, frame limits,
    sdkconfig options and errata."""
    gdma = t.get("gdma") or {}
    groups = {}
    for i, g in enumerate(gdma.get("groups") or []):
        groups[str(g.get("kind", i))] = _num(g.get("pairs")) or 0
    total = _num(gdma.get("pairs_total"))
    dedicated = t.get("dedicated_dma") or {}
    spi_hosts = [str(h).upper() for h in (t.get("spi_hosts_with_dma") or [])]
    capacity = {}
    if "spi_dma" in dedicated:
        capacity["spi_master"] = len((dedicated["spi_dma"] or {}).get("hosts") or [])
    if "i2s_dma" in dedicated:
        capacity["adc_continuous"] = 1
    kinds = t.get("pair_consumer_kinds")
    if kinds is None:
        kinds = {str(d): "gdma" for d in (t.get("pair_consumers") or [])}
    options = {}
    for key, name in (t.get("sdkconfig") or {}).items():
        for driver in ("spi_master", "adc_continuous", "uhci", "rmt", "i2s"):
            if key.startswith(driver + "_"):
                options.setdefault(driver, []).append(name)
    line = _num(t.get("data_cache_line_bytes")) or 0
    erratum = any(e.get("rule") == "esp32_half_duplex_dma_both_phases" for e in (t.get("errata") or []))
    bits = {str(k): _num(v) for k, v in (t.get("master_bits") or {}).items()}
    return {
        "groups": groups,
        "pairs": total if total is not None else sum(groups.values()),
        "capacity": capacity,
        "kinds": {str(k): v for k, v in kinds.items()},
        "spi_hosts": spi_hosts,
        "rx_multiple": _num(t.get("spi_rx_length_multiple")) or 1,
        "align": _num(t.get("buffer_align_bytes")) or 4,
        "line": line,
        "psram_line": _num(t.get("psram_cache_line_default")) or 0,
        "internal_cached": line > 0,
        "max_dma_frame": _num(t.get("max_dma_frame_bytes")) or 4092,
        "min_dma_frame": 4,
        "max_no_dma": _num(t.get("max_frame_no_dma_bytes")) or 64,
        "adc_conv_bytes": _num(t.get("adc_data_bytes_per_conv")) or 4,
        "half_duplex_erratum": erratum,
        "sdkconfig": options,
        "master_bits": bits,
    }


def _esp32(ctx, data):
    from . import dmadata

    b = ctx.b
    raw = dmadata.find_target(data, b["mcu"]["target"])
    if raw is None:
        ctx.add("dma.device_unknown", "mcu", f"no routing data for the target {b['mcu']['target']}; the data file has "
                + (", ".join(sorted(data.get("esp32", {}))) or "no ESP32 target"))
        return _empty_resolved()
    t = _esp_target(raw)
    refs_ok = _check_references(ctx)
    for name, st in b["streams"].items():
        if st["driver"] is None:
            ctx.add("dma.board_invalid", f"stream:{name}", f"stream {name}: driver is required on an esp32 board "
                    "(spi_master, adc_continuous, uhci, rmt or i2s)")
            refs_ok = False
        if st["driver"] == "spi_master" and not st["host"]:
            ctx.add("dma.board_invalid", f"stream:{name}", f"stream {name}: an spi_master stream names its host")
            refs_ok = False
    if not refs_ok:
        return _resolve_partial(ctx, {}, {})
    bits = dict(ESP_MASTER_BITS)
    bits.update({k: v for k, v in t["master_bits"].items() if v is not None})
    masters_all = sorted(t["master_bits"]) or sorted(ESP_MASTER_BITS)
    ctx.masters = {n: bits[n] for n in masters_all if n in bits}
    regions = {}
    for name, r in b["regions"].items():
        cache = r["cache"]
        policy = {"maintained": "Maintained", "coherent": "Coherent", "none": "Coherent"}[cache]
        line = r["line"] if r["line"] is not None else ((t["line"] or t["psram_line"] or 32) if policy == "Maintained"
                                                          else 1)
        align = r["align"] if r["align"] is not None else max(line, t["align"])
        declared = r["masters"]
        masters = sorted(declared if declared is not None else masters_all)
        for m in masters:
            if m not in bits:
                ctx.add("dma.master_unknown", f"region:{name}", f"region {name} lists master {m!r}; the ESP32 port "
                        f"knows {', '.join(sorted(bits))}")
        regions[name] = {"name": name, "base": r["base"], "size": r["size"], "cache": cache, "policy": policy,
                         "line": line, "align": align, "mpu_region": r["mpu_region"], "masters": masters,
                         "master_bits": sum(1 << bits[m] for m in masters if m in bits),
                         "memory": None, "cached_memory": t["internal_cached"], "external": False}
        if not _is_pow2(line) or not _is_pow2(align) or align < line:
            ctx.add("dma.region_line_invalid", f"region:{name}", f"region {name}: line {line} and align {align} "
                    "must be powers of two with align at least line")
        elif align < t["align"]:
            ctx.add("dma.region_align_below_dma_unit", f"region:{name}", f"region {name}: align {align} is below "
                    f"the {t['align']} bytes the DMA of {b['mcu']['target']} needs")
        if t["internal_cached"] and cache != "maintained":
            ctx.add("dma.region_cache_policy", f"region:{name}", f"region {name}: on {b['mcu']['target']} internal RAM "
                    "is reached through the data cache, so the region must be cache: maintained")
        elif cache == "maintained" and t["line"] and _is_pow2(line) and line < t["line"]:
            ctx.add("dma.region_line_below_cache_line", f"region:{name}", f"region {name}: line {line} is below the "
                    f"data cache line of {t['line']} bytes")
    _check_shared_buffers(ctx)
    _check_bus_streams(ctx)
    streams = {}
    shapes = {}
    for name in sorted(b["streams"]):
        st = b["streams"][name]
        s = f"stream:{name}"
        rs = {"name": name, "driver": st["driver"], "host": st["host"], "frame_bytes": st["frame_bytes"],
              "frames": st["frames"], "iram_safe": st["iram_safe"], "buffer": st["buffer"]}
        streams[name] = rs
        if st["driver"] == "adc_continuous":
            fb = st["frame_bytes"]
            if fb is None or st["frames"] is None:
                ctx.add("dma.board_invalid", s, f"stream {name}: an adc_continuous stream needs frame_bytes and "
                        "frames")
            else:
                multiple = max(t["adc_conv_bytes"], t["line"]) if t["internal_cached"] else t["adc_conv_bytes"]
                if fb % t["adc_conv_bytes"] or (multiple and fb % multiple):
                    ctx.add("dma.adc_frame_not_multiple", s, f"stream {name}: frame_bytes {fb} must be a multiple "
                            f"of {multiple} on {b['mcu']['target']} (the conversion result size"
                            + (", and the cache line" if multiple > t["adc_conv_bytes"] else "") + ")")
                rs["shape"] = {"frames": st["frames"], "element_bytes": t["adc_conv_bytes"],
                               "elements": fb // t["adc_conv_bytes"]}
                shapes[name] = (st["frames"], t["adc_conv_bytes"], fb // t["adc_conv_bytes"])
        if st["half_duplex_both_phases"] and t["half_duplex_erratum"] and st["driver"] == "spi_master":
            ctx.add("dma.errata_esp32_half_duplex_dma", s, f"stream {name}: half-duplex SPI DMA with a write and a "
                    f"read phase is not supported on {b['mcu']['target']} (ESP-IDF spi_master known issue); use full "
                    "duplex or no DMA on this bus")
        if st["iram_safe"]:
            _esp_iram(ctx, t, name, st)
    pairs = _esp_budget(ctx, t, streams)
    buses = _esp_buses(ctx, t, streams)
    _check_handlers(ctx, shapes)
    _check_fallback_budget(ctx)
    ctx_regions = regions
    return _resolve_final(ctx, None, ctx_regions, streams, buses, False, pairs)


def _esp_iram(ctx, t, name, st):
    options = t["sdkconfig"].get(st["driver"]) or []
    s = f"stream:{name}"
    if not options:
        return
    if ctx.sdkconfig is None:
        ctx.add("dma.sdkconfig_not_checked", s, f"stream {name} declares iram_safe; pass the image's sdkconfig "
                f"(--sdkconfig) to check {', '.join(sorted(options))}")
        return
    missing = [o for o in sorted(options) if ctx.sdkconfig.get(o) not in ("y", True, "1", 1)]
    if missing:
        ctx.add("dma.iram_safe_option_missing", s, f"stream {name} declares iram_safe but the sdkconfig does not set "
                f"{', '.join(o + '=y' for o in missing)}; the callbacks would run from flash while the cache is off")


def _esp_budget(ctx, t, streams):
    b = ctx.b
    target = b["mcu"]["target"]
    used = 0
    per_driver = {}
    for name in sorted(streams):
        rs = streams[name]
        drv = rs["driver"]
        per_driver[drv] = per_driver.get(drv, 0) + 1
        kind = t["kinds"].get(drv)
        if not kind:
            ctx.add("dma.driver_without_dma", f"stream:{name}", f"stream {name}: {drv} has no DMA on {target}")
            continue
        if drv == "spi_master" and rs["host"] and t["spi_hosts"] and rs["host"].upper() not in t["spi_hosts"]:
            ctx.add("dma.driver_without_dma", f"stream:{name}", f"stream {name}: {rs['host']} has no DMA on {target} "
                    f"(hosts with DMA: {', '.join(t['spi_hosts'])})")
            continue
        if kind == "gdma":
            used += 1
    for drv in sorted(per_driver):
        cap = t["capacity"].get(drv)
        if t["kinds"].get(drv) == "dedicated" and cap is not None and per_driver[drv] > cap:
            ctx.add("dma.dedicated_dma_exceeded", f"driver:{drv}", f"{per_driver[drv]} {drv} streams but {target} "
                    f"has {cap} dedicated DMA channel(s) for it")
    if used > t["pairs"]:
        groups = ", ".join(f"{k} {v}" for k, v in sorted(t["groups"].items()))
        ctx.add("dma.gdma_pairs_exceeded", "gdma", f"the board needs {used} GDMA pair(s) but {target} has "
                f"{t['pairs']}" + (f" ({groups})" if groups else "") + "; each SPI host, ADC, UHCI, RMT-with-DMA "
                "and I2S user takes one pair")
    return {"used": used, "budget": t["pairs"]}


def _esp_buses(ctx, t, streams):
    b = ctx.b
    out = []
    by_host = {}
    for name, rs in streams.items():
        if rs["driver"] == "spi_master" and rs["host"]:
            by_host[rs["host"].lower()] = name
    for name in sorted(b["buses"]):
        bus = b["buses"][name]
        listed = bus["dma"] if isinstance(bus["dma"], list) else []
        dma = bool(listed) or bus["dma"] is True
        staging = bus["staging_bytes"]
        if dma and bus["kind"] == "spi" and not listed and name not in by_host:
            ctx.add("dma.bus_without_stream", f"bus:{name}", f"bus {name} has DMA but no spi_master stream names "
                    f"host {name.upper()}, so its GDMA pair is not counted")
        if dma and bus["kind"] == "spi":
            if staging is None:
                ctx.add("dma.staging_missing", f"bus:{name}", f"bus {name} has DMA but no staging_bytes")
            else:
                if staging > t["max_dma_frame"]:
                    ctx.add("dma.staging_too_large", f"bus:{name}", f"bus {name}: staging_bytes {staging} is above the "
                            f"largest DMA frame of {t['max_dma_frame']} bytes")
                elif staging < t["min_dma_frame"]:
                    ctx.add("dma.staging_too_small", f"bus:{name}", f"bus {name}: staging_bytes {staging} is below "
                            f"the smallest DMA buffer of {t['min_dma_frame']} bytes")
                if staging % t["rx_multiple"]:
                    ctx.add("dma.spi_rx_length_multiple", f"bus:{name}", f"bus {name}: staging_bytes {staging} is not "
                            f"a multiple of {t['rx_multiple']}; the SPI receive on {b['mcu']['target']} rounds up "
                            "to that and would write past the buffer")
        limits = {"dma": dma, "max_frame_dma": staging if dma and staging else 0, "max_frame_no_dma": t["max_no_dma"],
                  "rx_length_multiple": t["rx_multiple"], "align_bytes": t["align"]}
        carry = limits["max_frame_dma"] if dma else t["max_no_dma"]
        for dev in bus["devices"]:
            limit = _driver_limit(ctx, dev)
            if limit is not None and limit > carry and not (dma and staging is None):
                ctx.add("dma.driver_frame_exceeds_bus", f"bus:{name}/{dev['driver']}", f"driver {dev['driver']} on "
                        f"bus {name} sends frames of {limit} bytes but the bus carries {carry}"
                        + ("" if dma else " without DMA (the SPI FIFO); give the bus DMA with a staging buffer")
                        + (" in its staging buffer" if dma else ""))
        mode = "dma" if dma else "polled"
        out.append({"name": name, "kind": bus["kind"], "model": bus["model"], "mode": mode, "streams": listed,
                    "limits": limits, "devices": _devices(ctx, bus), "staging_bytes": staging})
    return out
