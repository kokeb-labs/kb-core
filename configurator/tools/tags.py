# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""Search tags and category of a driver, from its GitHub topics and the rules in configurator/data/tags.json.

A topic is dropped (universal topics and bus names), aliased to an allowed tag, an allowed tag, or ignored: chip names,
vendor names and unknown topics are not tags, they stay searchable through the card's own text. A driver whose topics
leave no tag falls back to driver.yml (category, description keywords) and its bus; its category is never empty.
Pure functions, no I/O except load_rules.
"""

from __future__ import annotations

import json
import re
from pathlib import Path

HERE = Path(__file__).resolve().parent
RULES_PATH = HERE.parent / "data" / "tags.json"
OTHER = "other"


class RulesError(Exception):
    pass


def load_rules(path: Path = RULES_PATH) -> dict:
    """tags.json, checked against its schema and for cross references (an alias names an allowed tag, ...)."""
    from jsonschema_lite import validate  # noqa: PLC0415  (sibling module, found when tools/ is on sys.path)
    rules = json.loads(Path(path).read_text())
    schema = json.loads((HERE.parent / "schemas-draft" / "tags.schema.json").read_text())
    problems = validate(rules, schema)
    allow, cats = set(rules.get("allow", [])), set(rules.get("categories", []))
    if not problems:
        for topic, target in rules["alias"].items():
            if target not in allow:
                problems.append(f"alias {topic} -> {target}: {target} is not in allow")
            if topic in allow:
                problems.append(f"alias {topic} is also in allow")
            if topic in rules["drop"]:
                problems.append(f"alias {topic} is also in drop")
        for tag in rules["allow"]:
            if tag in rules["drop"]:
                problems.append(f"{tag} is in both allow and drop")
            if tag not in rules["category_of"]:
                problems.append(f"allowed tag {tag} has no category_of entry")
        for tag, cat in rules["category_of"].items():
            if tag not in allow:
                problems.append(f"category_of {tag} is not an allowed tag")
            if cat not in cats:
                problems.append(f"category_of {tag}: {cat} is not one of categories")
        if OTHER not in cats:
            problems.append(f"categories must include {OTHER}")
        for word, tag in rules["fallback"]["keywords"].items():
            if tag not in allow:
                problems.append(f"fallback keyword {word} -> {tag}: {tag} is not in allow")
        for bus, cat in rules["fallback"]["bus_category"].items():
            if cat not in cats:
                problems.append(f"fallback bus {bus}: {cat} is not one of categories")
    if problems:
        raise RulesError("tags.json: " + "; ".join(problems))
    return rules


def tags_from_topics(topics: list[str], rules: dict) -> list[str]:
    """The allowed tags of `topics` (aliases resolved, dropped and unknown topics left out), sorted, no duplicates."""
    out = set()
    for raw in topics:
        topic = raw.strip().lower()
        if topic in rules["drop"]:
            continue
        topic = rules["alias"].get(topic, topic)
        if topic in rules["allow"]:
            out.add(topic)
    return sorted(out)


def category_of_tags(tags: list[str], rules: dict) -> str | None:
    """The category with most tags; a tie goes to the one listed first in `categories`. None for no tags."""
    counts: dict[str, int] = {}
    for tag in tags:
        cat = rules["category_of"][tag]
        counts[cat] = counts.get(cat, 0) + 1
    if not counts:
        return None
    order = rules["categories"]
    return sorted(counts, key=lambda c: (-counts[c], order.index(c)))[0]


def fallback(meta: dict, summary: str, buses: list[str], rules: dict) -> tuple[list[str], str]:
    """(tags, category) for a driver without an allowed topic, from driver.yml `category` and `description`, the
    README summary and the bus. The category is never empty."""
    cats = rules["categories"]
    declared = meta.get("category")
    text = f"{meta.get('description') or ''} {summary or ''}".lower()
    tags = set()
    for word, tag in rules["fallback"]["keywords"].items():
        if re.search(rf"\b{re.escape(word)}", text):
            tags.add(tag)
    tags = sorted(tags)
    if isinstance(declared, str) and declared in cats:
        return tags, declared
    cat = category_of_tags(tags, rules)
    if cat is None:
        for bus in buses:
            if bus in rules["fallback"]["bus_category"]:
                return tags, rules["fallback"]["bus_category"][bus]
        cat = OTHER
    return tags, cat


def driver_tags(topics: list[str] | None, meta: dict, summary: str, buses: list[str], rules: dict) -> dict:
    """{"tags": [...], "category": ...} of one driver. `topics` None means they were not fetched (offline build):
    only the fallback applies."""
    tags = tags_from_topics(topics or [], rules)
    if tags:
        return {"tags": tags, "category": category_of_tags(tags, rules)}
    fb_tags, cat = fallback(meta, summary, buses, rules)
    return {"tags": fb_tags, "category": cat}


def tag_index(drivers: list[dict], rules: dict) -> dict:
    """Counts for the chips: drivers per allowed tag and per category (zero counts included, so a chip can be
    shown disabled), keyed in sorted order."""
    tags = {t: 0 for t in sorted(rules["allow"])}
    cats = {c: 0 for c in rules["categories"]}
    for drv in drivers:
        for tag in drv["tags"]:
            tags[tag] += 1
        cats[drv["category"]] += 1
    return {"tags": tags, "categories": cats}
