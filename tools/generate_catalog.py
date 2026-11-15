"""Regenerate Rack40's VCV manifest and native catalogs."""

import json
import runpy
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
registry = json.loads((ROOT / "shared/module-configurations.v1.json").read_text())
panels = runpy.run_path(str(ROOT / "tools/panel_layouts.py"))["generate"]()
(ROOT / "shared/module-panels.v1.json").write_text(json.dumps(panels, indent=2) + "\n")
(ROOT / "res/module-panels.json").write_text(json.dumps(panels, indent=2) + "\n")
(ROOT / "res/module-catalog.json").write_text(json.dumps(registry, indent=2) + "\n")

manifest = {
    "slug": "Rack40",
    "name": "Rack40",
    "version": "2.3.0",
    "license": "GPL-3.0-only",
    "brand": "Rack40",
    "author": "Rack40 contributors",
    "authorEmail": "hellobanjoroll@gmail.com",
    "description": "Virtual ZOIA patch building and live MIDI control; no ZOIA DSP emulation.",
    "modules": [{
        "slug": "Live",
        "name": "Live Control",
        "description": "Eight MIDI CC macros and a monophonic MIDI note interface for a loaded ZOIA patch.",
        "tags": ["Controller", "MIDI", "External"],
    }],
}

visible_types = set()
for configuration in registry["configurations"]:
    hidden = (
        configuration["id"] != "sequencer-32-loop"
        if configuration["type"] == "Sequencer"
        else configuration["type"] in visible_types
    )
    if not hidden:
        visible_types.add(configuration["type"])
    manifest["modules"].append({
        "slug": configuration["id"],
        "name": configuration["type"] if not hidden else configuration["name"] + " / " + configuration["id"],
        "hidden": hidden,
        "description": "ZOIA patch module (not audio DSP). " + configuration["description"],
        "tags": ["External", "Utility"],
    })

(ROOT / "plugin.json").write_text(json.dumps(manifest, indent=2) + "\n")
