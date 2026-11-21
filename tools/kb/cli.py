# The kb command line: `kb catalog build`, `kb resolve`, `kb outdated`, `kb upgrade`, `kb sbom`.
# Usage: tools/kb/README.md.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""Command-line front end of the kb resolver."""

import argparse
import os
import sys

from . import __version__, catalog, emit, resolver, sbom, upgrade, yamlio

EXIT_OK = 0
EXIT_RESOLVE = 1   # the selection does not resolve, or the catalogue has problems
EXIT_USAGE = 2     # bad arguments or unreadable input
EXIT_OUTDATED = 3  # kb outdated: at least one component has a newer selectable version


def _write(path, text):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)


def _report(entries, stream, label):
    for e in entries:
        print(f"{label} {e['code']} {e['subject']}: {e['message']}", file=stream)


def cmd_catalog_build(args):
    kb_dir = args.kb_core_dir
    pinned = catalog.pins(kb_dir) if kb_dir else {}
    sources = []
    if args.online:
        names = args.repo or catalog.online_repos(args.org)
        for name in names:
            sources.append(catalog.online_source(args.org, name, pinned))
    for d in catalog.driver_dirs(args.drivers_dir or []):
        sources.append(catalog.local_source(d, args.org, pinned))
    kb_versions = catalog.kb_core_versions(kb_dir, args.include_head) if kb_dir else []
    cat, problems = catalog.build(sources, kb_versions)
    if args.platforms:
        facts = yamlio.load(args.platforms)
        catalog.apply_binding_facts(cat, (facts or {}).get("bindings") or {} if isinstance(facts, dict) else {})
    for p in problems:
        print(f"error: {p}", file=sys.stderr)
    text = emit.canonical_json(cat)
    if args.output == "-":
        sys.stdout.write(text)
    else:
        _write(args.output, text)
        print(f"kb: catalogue with {len(cat['drivers'])} driver(s), {len(kb_versions)} kb-core version(s): "
              f"{args.output}")
    return EXIT_RESOLVE if problems else EXIT_OK


def cmd_resolve(args):
    image = yamlio.load(args.image)
    cat = catalog.load_any(yamlio.load(args.catalog), args.catalog)
    result = resolver.resolve(cat, image)
    _report(result["warnings"], sys.stderr, "warning")
    _report(result["errors"], sys.stderr, "error")
    if not result["ok"]:
        print(f"kb: {args.image} does not resolve ({len(result['errors'])} error(s))", file=sys.stderr)
        return EXIT_RESOLVE
    files = emit.files(result["lock"])
    if args.output_dir is None:
        sys.stdout.write(files[emit.LOCK_FILE])
        return EXIT_OK
    for name in sorted(files):
        path = os.path.join(args.output_dir, name)
        _write(path, files[name])
        if name == emit.FETCH_FILE:
            os.chmod(path, 0o755)
    print(f"kb: resolved {result['lock']['image']}: {', '.join(sorted(files))} in {args.output_dir}")
    return EXIT_OK


def _lock_for(args):
    """The current lock: --lock, else kb-image.lock.yml beside the image (unless --no-lock), else none."""
    if args.no_lock:
        return None
    path = args.lock
    if path is None:
        beside = os.path.join(os.path.dirname(os.path.abspath(args.image)), emit.LOCK_FILE)
        if os.path.isfile(beside):
            path = beside
            print(f"kb: current versions from {path}", file=sys.stderr)
    return yamlio.load(path) if path else None


def _targets(values):
    out = {}
    for item in values or []:
        name, sep, version = item.partition("=")
        if not sep or not name or not version:
            raise yamlio.InputError(f"--to {item}: expected NAME=VERSION, e.g. kb-core=0.4.1 or bmp581=0.2.0")
        if name in out:
            raise yamlio.InputError(f"--to names {name} twice")
        out[name] = version
    return out


def _plan(args):
    targets = _targets(getattr(args, "to", None))
    image = yamlio.load(args.image)
    cat = catalog.load_any(yamlio.load(args.catalog), args.catalog)
    result = upgrade.plan(cat, image, _lock_for(args), targets, args.allow_rc)
    _report(result["warnings"], sys.stderr, "warning")
    _report(result["errors"], sys.stderr, "error")
    return result


def _outdated_table(rows):
    head = ("Component", "Current", "Upgrade to", "Change", "Breaking", "Newest release", "Newer rc", "Held back")
    body = [(r["name"], r["from"], r["to"] if r["change"] != "none" else "-", r["change"],
             "yes" if r["breaking"] else "no", r["newest_release"] or "-", r["newest_rc"] or "-",
             ", ".join(f"{h['version']} ({h['code']})" for h in r["held"]) or "-") for r in rows]
    widths = [max(len(row[i]) for row in [head, *body]) for i in range(len(head))]
    return "".join("  ".join(cell.ljust(widths[i]) for i, cell in enumerate(row)).rstrip() + "\n"
                   for row in [head, *body])


def cmd_outdated(args):
    result = _plan(args)
    if not result["ok"]:
        print(f"kb: cannot plan an upgrade of {args.image} ({len(result['errors'])} error(s))", file=sys.stderr)
        return EXIT_RESOLVE
    if args.json:
        sys.stdout.write(emit.canonical_json({"rows": result["rows"], "warnings": result["warnings"]}))
    else:
        sys.stdout.write(_outdated_table(result["rows"]))
    return EXIT_OUTDATED if any(r["change"] != "none" for r in result["rows"]) else EXIT_OK


def cmd_upgrade(args):
    result = _plan(args)
    if not result["ok"]:
        print(f"kb: {args.image} cannot be upgraded ({len(result['errors'])} error(s))", file=sys.stderr)
        return EXIT_RESOLVE
    files = upgrade.files(result, args.allow_rc)
    if args.output_dir is None:
        sys.stdout.write(files[upgrade.REPORT_FILE])
        return EXIT_OK
    for name in sorted(files):
        path = os.path.join(args.output_dir, name)
        _write(path, files[name])
        if name == emit.FETCH_FILE:
            os.chmod(path, 0o755)
    changed = sum(1 for r in result["rows"] if r["change"] != "none")
    print(f"kb: upgraded {result['lock']['image']} ({changed} component(s) changed): "
          f"{', '.join(sorted(files))} in {args.output_dir}")
    return EXIT_OK


def _created(args):
    if args.created:
        return args.created
    epoch = os.environ.get("SOURCE_DATE_EPOCH")
    if epoch and epoch.isdigit():
        import datetime

        return datetime.datetime.fromtimestamp(int(epoch), datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    return None


def cmd_sbom(args):
    lock = yamlio.load(args.lock)
    if not isinstance(lock, dict) or lock.get("schema") != 1:
        raise yamlio.InputError(f"{args.lock}: not a kb lock (schema 1)")
    created = _created(args)
    cat = yamlio.load(args.catalog) if args.catalog else None
    doc = (sbom.cyclonedx(lock, created, cat) if args.format == "cyclonedx" else sbom.spdx(lock, created, cat))
    text = emit.canonical_json(doc)
    if args.output in (None, "-"):
        sys.stdout.write(text)
    else:
        _write(args.output, text)
    return EXIT_OK


def parser():
    p = argparse.ArgumentParser(prog="kb",
                                description="Catalogue, resolver, upgrades and SBOMs of kb-core images (ADR-0014).")
    p.add_argument("--version", action="version", version=f"kb {__version__}")
    sub = p.add_subparsers(dest="command", required=True)

    cat = sub.add_parser("catalog", help="the catalogue of drivers, ports, RTOS and kb-core versions")
    cat_sub = cat.add_subparsers(dest="catalog_command", required=True)
    build = cat_sub.add_parser("build", help="build catalog.json from driver clones (offline) or GitHub (online)")
    build.add_argument("--drivers-dir", action="append", metavar="DIR",
                       help="a driver clone, or a folder of driver-* clones (repeatable)")
    build.add_argument("--online", action="store_true", help="read tags and driver.yml with gh api")
    build.add_argument("--org", default="kokeb-labs-drivers", help="organisation of the drivers (default: %(default)s)")
    build.add_argument("--repo", action="append", metavar="NAME", help="with --online: only these repositories")
    build.add_argument("--kb-core-dir", metavar="DIR", help="a kb-core clone: its tags and drivers.lock.yml pins")
    build.add_argument("--include-head", action="store_true",
                       help="add the kb-core working revision as an unreleased (dev) version")
    build.add_argument("--platforms", metavar="FILE",
                       help="binding facts (requires_owner) from the configurator's platforms.json, until a release has them")
    build.add_argument("-o", "--output", default="catalog.json", help="output file, - for stdout")
    build.set_defaults(func=cmd_catalog_build)

    res = sub.add_parser("resolve", help="resolve kb-image.yml into a lock, presets, kb_image.cmake, kb-fetch.sh")
    res.add_argument("image", help="kb-image.yml (or .json)")
    res.add_argument("--catalog", required=True, help="catalog.json from kb catalog build or a release")
    res.add_argument("-o", "--output-dir", help="write every file here; without it the lock goes to stdout")
    res.set_defaults(func=cmd_resolve)

    def plan_args(sp):
        sp.add_argument("image", help="kb-image.yml (or .json) of the project")
        sp.add_argument("--catalog", required=True,
                        help="catalog.json from kb catalog build or a release, or the configurator's data/catalog.json")
        sp.add_argument("--lock", help="the project's current kb-image.lock.yml (default: the one beside the image)")
        sp.add_argument("--no-lock", action="store_true", help="ignore any lock: unnamed versions are today's defaults")
        sp.add_argument("--allow-rc", action="store_true", help="also pick release candidates")

    out = sub.add_parser("outdated", help="list the newer catalogued versions an image can move to")
    plan_args(out)
    out.add_argument("--json", action="store_true", help="the plan's rows and warnings as JSON")
    out.set_defaults(func=cmd_outdated)

    up = sub.add_parser("upgrade", help="write the upgraded kb-image.yml, its lock and files, and kb-upgrade.md")
    plan_args(up)
    up.add_argument("--to", action="append", metavar="NAME=VERSION",
                    help="move kb-core, port-<family> or a driver to exactly this version (repeatable)")
    up.add_argument("-o", "--output-dir", help="write every file here (. for the project itself); "
                    "without it the upgrade report goes to stdout")
    up.set_defaults(func=cmd_upgrade)

    sb = sub.add_parser("sbom", help="CycloneDX 1.6 or SPDX 2.3 JSON of a lock")
    sb.add_argument("lock", help="kb-image.lock.yml")
    sb.add_argument("--format", choices=("cyclonedx", "spdx"), default="cyclonedx")
    sb.add_argument("--created", help="creation time (ISO 8601, UTC); default SOURCE_DATE_EPOCH, else none/epoch")
    sb.add_argument("--catalog", help="catalog.json whose versions carry known-vulnerability scans (osv)")
    sb.add_argument("-o", "--output", help="output file, - for stdout")
    sb.set_defaults(func=cmd_sbom)
    return p


def main(argv=None):
    args = parser().parse_args(argv)
    try:
        return args.func(args)
    except (yamlio.InputError, catalog.CatalogError) as exc:
        print(f"kb: {exc}", file=sys.stderr)
        return EXIT_USAGE
