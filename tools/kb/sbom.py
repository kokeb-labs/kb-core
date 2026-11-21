# `kb sbom`: CycloneDX 1.6 and SPDX 2.3 JSON of exactly the components in a lock (ADR-0013, ADR-0014), plus the
# SDK and RTOS the image runs on as SOUP, and each component's known-vulnerability scan (kb:osv) when a catalogue
# that carries scans is given. The same rules as the configurator's downloads (configurator/js/generate.js).
# Deterministic: the serial number and namespace derive from the lock's own SHA-256, and no clock is read unless a
# creation time is given (--created or SOURCE_DATE_EPOCH).
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""Software bills of materials from a kb lock (pure)."""

import hashlib
import uuid

from . import __version__
from .emit import canonical_json

EPOCH = "1970-01-01T00:00:00Z"
# The ThreadX release kb-core pins as the third_party/threadx submodule (docs/assurance/soup.md, ADR-0015).
THREADX_PIN = "v6.5.2.202603b_rel"


def soup(lock):
    """The SOUP of an image: the SDK of its MCU family and its RTOS (docs/assurance/soup.md)."""
    out = []
    if lock["mcu"]["family"] == "esp32":
        out.append({"name": "esp-idf", "version": "v5.5", "licence": "Apache-2.0", "supplier": "Espressif Systems",
                    "note": "SDK of the ESP32 port"})
    if lock["rtos"] == "freertos":
        out.append({"name": "freertos-kernel", "version": None, "licence": "MIT", "supplier": "Amazon Web Services",
                    "note": "RTOS, via esp-idf" if lock["mcu"]["family"] == "esp32" else "RTOS backend"})
    if lock["rtos"] == "threadx":
        # The image's kernel is the product's or the pinned one; the lock does not say which, so no version is
        # asserted (a product SBOM records its kernel's own).
        out.append({"name": "threadx", "version": None, "licence": "MIT", "supplier": "Eclipse Foundation",
                    "note": f"RTOS backend; kb-core tests against {THREADX_PIN} (third_party/threadx)"})
    return out


class _NoScan:
    """No catalogue: every top-level component reports `not scanned`."""

    def get(self, ref):
        return None


def osv_index(catalog, lock):
    """ref -> the scan of that component's selected version (None: not scanned), from a catalogue."""
    if catalog is None:
        return _NoScan()
    out = {}
    kb = next((v for v in catalog.get("kb_core", {}).get("versions", []) if v.get("version") == lock["kb_core"]["version"]),
              None)
    out["kb-core"] = kb.get("osv") if kb else None
    for port in lock["ports"]:
        if not port["in_tree"]:
            versions = []
            for m in catalog.get("mcus", []):
                if m.get("port") and m["port"].get("name") == port["name"]:
                    versions = m["port"].get("versions", [])
            v = next((x for x in versions if x.get("version") == port["version"]), None)
            out[f"port-{port['name']}"] = v.get("osv") if v else None
    for drv in lock["drivers"]:
        entry = next((d for d in catalog.get("drivers", []) if d.get("name") == drv["name"]), {})
        v = next((x for x in entry.get("versions", []) if x.get("version") == drv["version"]), None)
        out[f"driver-{drv['name']}"] = v.get("osv") if v else None
    return out


def _osv_text(osv):
    if not osv:
        return "not scanned"
    return f"scanned {osv['scanned_at']} ({osv['database']}), {len(osv.get('findings', []))} finding(s)"


def lock_digest(lock):
    return hashlib.sha256(canonical_json(lock).encode("utf-8")).hexdigest()


def _purl(repo, tag, commit):
    ref = tag if tag is not None else commit
    return f"pkg:github/{repo}@{ref}"


def components(lock):
    """The components of a lock, in a fixed order: kb-core, its parts, an extracted port, the drivers."""
    kb = lock["kb_core"]
    out = [{"ref": "kb-core", "name": "kb-core", "version": kb["version"], "repo": kb["repo"], "tag": kb["tag"],
            "commit": kb["commit"], "license": kb["license"], "authors": kb["authors"], "kind": "framework",
            "parent": None}]
    for part in lock["parts"]:
        out.append({"ref": f"kb-core/{part}", "name": f"kb-{part}", "version": kb["version"], "repo": kb["repo"],
                    "tag": kb["tag"], "commit": kb["commit"], "license": kb["license"], "authors": kb["authors"],
                    "kind": "library", "parent": "kb-core"})
    for port in lock["ports"]:
        if port["in_tree"]:
            out.append({"ref": f"kb-core/port-{port['name']}", "name": f"port-{port['name']}",
                        "version": kb["version"], "repo": kb["repo"], "tag": kb["tag"], "commit": kb["commit"],
                        "license": kb["license"], "authors": kb["authors"], "kind": "library", "parent": "kb-core"})
        else:
            out.append({"ref": f"port-{port['name']}", "name": f"port-{port['name']}", "version": port["version"],
                        "repo": port["repo"], "tag": port["tag"], "commit": port["commit"], "license": "MIT",
                        "authors": [f"The port-{port['name']} authors"], "kind": "library", "parent": None})
    for drv in lock["drivers"]:
        out.append({"ref": f"driver-{drv['name']}", "name": drv["folder"], "version": drv["version"],
                    "repo": drv["repo"], "tag": drv["tag"], "commit": drv["commit"], "license": drv["license"],
                    "authors": drv["authors"], "kind": "library", "parent": None})
    return out


def _cdx_component(c, osv=None, scanned=False):
    comp = {
        "type": c["kind"],
        "bom-ref": c["ref"],
        "name": c["name"],
        "version": c["version"],
        "purl": _purl(c["repo"], c["tag"], c["commit"]),
        "externalReferences": [{"type": "vcs", "url": f"https://github.com/{c['repo']}"}],
        "properties": [{"name": "kb:commit", "value": c["commit"]}],
    }
    if scanned:
        comp["properties"].append({"name": "kb:osv", "value": _osv_text(osv)})
    if c["license"] != "NOASSERTION":
        comp["licenses"] = [{"license": {"id": c["license"]}}]
    if c["authors"]:
        comp["authors"] = [{"name": a} for a in c["authors"]]
    return comp


def cyclonedx(lock, created=None, catalog=None):
    comps = components(lock)
    scans = osv_index(catalog, lock)
    # Top-level components carry their own scan; parts and an in-tree port share kb-core's.
    top = [_cdx_component(c, scans.get(c["ref"]), True) for c in comps if c["parent"] is None]
    for c in comps:
        if c["parent"] is not None:
            parent = next(t for t in top if t["bom-ref"] == c["parent"])
            parent.setdefault("components", []).append(_cdx_component(c))
    for item in soup(lock):
        top.append({
            "type": "framework", "bom-ref": f"soup:{item['name']}", "name": item["name"],
            "version": item["version"] or "unspecified", "supplier": {"name": item["supplier"]},
            "licenses": [{"license": {"id": item["licence"]}}],
            "properties": [{"name": "kb:role", "value": "soup"}, {"name": "kb:note", "value": item["note"]}],
        })
    image_ref = f"image:{lock['image']}"
    metadata = {
        "component": {"type": "firmware", "bom-ref": image_ref, "name": lock["image"],
                      "properties": [{"name": "kb:mcu", "value": f"{lock['mcu']['family']}/{lock['mcu']['target']}"},
                                     {"name": "kb:rtos", "value": lock["rtos"]}]},
        "tools": {"components": [{"type": "application", "name": "kb", "version": __version__}]},
    }
    if created is not None:
        metadata["timestamp"] = created
    return {
        "bomFormat": "CycloneDX",
        "specVersion": "1.6",
        "serialNumber": f"urn:uuid:{uuid.uuid5(uuid.NAMESPACE_URL, 'kb-lock:' + lock_digest(lock))}",
        "version": 1,
        "metadata": metadata,
        "components": top,
        "dependencies": [{"ref": image_ref, "dependsOn": sorted(t["bom-ref"] for t in top)}],
    }


def _spdx_scan(osv):
    return osv["scanned_at"] if osv else "not scanned"


def _spdx_id(ref):
    return "SPDXRef-" + "".join(ch if ch.isalnum() or ch in ".-" else "-" for ch in ref)


def spdx(lock, created=None, catalog=None):
    digest = lock_digest(lock)
    scans = osv_index(catalog, lock)
    image_id = _spdx_id(f"image-{lock['image']}")
    packages = [{
        "SPDXID": image_id, "name": lock["image"], "downloadLocation": "NOASSERTION", "filesAnalyzed": False,
        "licenseConcluded": "NOASSERTION", "licenseDeclared": "NOASSERTION", "copyrightText": "NOASSERTION",
        "primaryPackagePurpose": "FIRMWARE",
    }]
    relationships = [{"spdxElementId": "SPDXRef-DOCUMENT", "relationshipType": "DESCRIBES",
                      "relatedSpdxElement": image_id}]
    for c in components(lock):
        ref = c["tag"] if c["tag"] is not None else c["commit"]
        holders = ", ".join(c["authors"])
        packages.append({
            "SPDXID": _spdx_id(c["ref"]), "name": c["name"], "versionInfo": c["version"],
            "downloadLocation": f"git+https://github.com/{c['repo']}.git@{ref}", "filesAnalyzed": False,
            "licenseConcluded": c["license"], "licenseDeclared": c["license"],
            "copyrightText": f"Copyright (c) {holders}" if holders else "NOASSERTION",
            "supplier": "Organization: " + c["repo"].split("/")[0],
            "primaryPackagePurpose": "LIBRARY" if c["kind"] == "library" else "FRAMEWORK",
            "externalRefs": [{"referenceCategory": "PACKAGE-MANAGER", "referenceType": "purl",
                              "referenceLocator": _purl(c["repo"], c["tag"], c["commit"])}],
            "comment": f"commit {c['commit']}" + ("" if c["parent"] is not None else
                                                  f"; known-vulnerability scan: {_spdx_scan(scans.get(c['ref']))}"),
        })
        parent = image_id if c["parent"] is None else _spdx_id(c["parent"])
        relationships.append({"spdxElementId": parent, "relationshipType": "CONTAINS",
                              "relatedSpdxElement": _spdx_id(c["ref"])})
    for item in soup(lock):
        sid = _spdx_id(f"soup-{item['name']}")
        packages.append({
            "SPDXID": sid, "name": item["name"], "versionInfo": item["version"] or "NOASSERTION",
            "downloadLocation": "NOASSERTION", "filesAnalyzed": False, "licenseConcluded": item["licence"],
            "licenseDeclared": item["licence"], "copyrightText": "NOASSERTION",
            "supplier": f"Organization: {item['supplier']}", "primaryPackagePurpose": "FRAMEWORK",
            "comment": f"SOUP, {item['note']}",
        })
        relationships.append({"spdxElementId": image_id, "relationshipType": "CONTAINS", "relatedSpdxElement": sid})
    return {
        "spdxVersion": "SPDX-2.3",
        "dataLicense": "CC0-1.0",
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": f"kb-image-{lock['image']}",
        "documentNamespace": f"https://kokeb-labs.github.io/kb-core/spdx/{lock['image']}-{digest}",
        "creationInfo": {"created": created or EPOCH, "creators": [f"Tool: kb-{__version__}"]},
        "packages": packages,
        "relationships": relationships,
    }
