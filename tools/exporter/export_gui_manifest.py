# SPDX-License-Identifier: GPL-3.0-or-later
"""Export the reference GUI's parameter manifest (labels, ranges, steps,
tooltips, tiers, choices) to JSON, so the app's panels present every parameter
exactly as spektrafilm's own GUI does.

    python tools/exporter/export_gui_manifest.py path/to/spektrafilm apps/spectra-app/resources/params_manifest.json

Only the presentation metadata is read; no GUI toolkit is imported.
"""
import dataclasses
import enum
import importlib.util
import json
import sys
import types
import typing
from pathlib import Path


def load(src: Path):
    pkg = types.ModuleType("spektrafilm_gui")
    pkg.__path__ = [str(src / "spektrafilm_gui")]
    sys.modules["spektrafilm_gui"] = pkg
    for name in ("options", "params_manifest"):
        spec = importlib.util.spec_from_file_location(f"spektrafilm_gui.{name}", src / "spektrafilm_gui" / f"{name}.py")
        mod = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = mod
        spec.loader.exec_module(mod)
    return sys.modules["spektrafilm_gui.params_manifest"]


def field(spec):
    d = {"path": "/" + spec.path.replace(".", "/"), "tier": spec.tier}
    if spec.label:
        d["label"] = spec.label
    if spec.tooltip:
        d["tooltip"] = spec.tooltip
    for k in ("min", "max", "step", "decimals"):
        v = getattr(spec, k)
        if v is not None:
            d[k] = v
    if spec.enum is not None:
        d["choices"] = [e.value for e in spec.enum]
    return d


def main():
    repo, out = Path(sys.argv[1]), Path(sys.argv[2])
    sys.path.insert(0, str(repo / "src"))
    m = load(repo / "src")
    groups = []
    for g in m.ALL_MANIFESTS:
        hints = typing.get_type_hints(g.group_cls)
        fields = []
        for s in g.fields:
            f = field(s)
            fields.append(f)
        groups.append({
            "title": g.title,
            "path": "/" + g.group_path.replace(".", "/"),
            "fields": fields,
            "subsections": [{"title": s.title, "fields": list(s.field_names)} for s in g.subsections],
        })
    extra = {}
    for name in ("INPUT_PANEL_FIELDS", "SPECTRAL_PANEL_FIELDS", "SIMULATION_SPECIAL_BORROWED_FIELDS",
                 "SIMULATION_EXPOSURE_PANEL_FIELDS", "SIMULATION_ENLARGER_PANEL_FIELDS",
                 "SIMULATION_OUTPUT_PANEL_FIELDS", "SIMULATION_FOOTER_FIELDS"):
        extra[name] = [field(s) for s in getattr(m, name)]
    import subprocess
    commit = subprocess.run(["git", "-C", str(repo), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    doc = {
        "source": "spektrafilm_gui/params_manifest.py",
        "upstream_commit": commit,
        "licence": "GPL-3.0 (spektrafilm, Andrea Volpato)",
        "groups": groups,
        "panels": extra,
    }
    out.write_text(json.dumps(doc, indent=1) + "\n", encoding="utf-8")
    print(f"{len(groups)} groups, {sum(len(g['fields']) for g in groups)} fields -> {out}")


if __name__ == "__main__":
    main()
