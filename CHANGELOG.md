# Rack40 native changelog

## 2.3.1

- Adds Rack40 website, manual, source, changelog, support contact, and minimum
  Rack version metadata for VCV Library submission.
- Replaces copied module-description prose with a single link to the official
  ZOIA module documentation.
- Expands Apple Silicon installation, troubleshooting, and first-patch
  instructions for new VCV Rack users.
- Contains no DSP, MIDI, patch-format, or module-identity changes from 2.3.0.

## 2.3.0

- Synchronizes the native catalog, panel layouts, parameters, and sockets with all 740 shared ZOIA configurations, including firmware 5.40 MIDI Pitch Bend Out and MIDI Pressure Out.
- Retains the reviewed 2.2.21 reliability work: recording-only disconnect monitoring, starts-muted Live Control behavior, held-note cleanup, atomic preset writes, and cleanup on leave/remove/reset.
- Retains Pushbutton Live MIDI bindings with bounded press/release behavior and confirmation safety.
- Preserves extra sequencer page/track banks through save, clone, history, portable presets, and browser/native exchange.
- Keeps configured-gain indicators for supported VCA/mixer/output controls. These are patch-building displays, not audio meters.
- Adds a catalog drift check and six fixed owner/test-built bidirectional exchange fixtures.
- Labels every panel as requiring ZOIA hardware so Rack40 modules are not mistaken for standalone VCV DSP processors.

### Deferred

- Native editing UI for extra sequencer pages and tracks.
- Factory templates and a factory preset collection.
- Advanced knob modes and engineering-unit entry conveniences.
- Richer multiselection menus and browser-specific cable conveniences.
- Broad compatibility work for the 135 blocked historical corpus patches beyond a clear error.

### Known host issue

VCV Rack 2.6.6 can crash if a context menu remains open while a patch is replaced. Press Escape or click empty rack space to close every module, parameter, and cable menu before loading another patch. This also reproduces with a stock Rack module and is treated as a host limitation.

The macOS ARM64 direct-download build passed the automated native suite, package audit, browser/native/ZOIA exchange, and owner Live Control listening check. The unverified Mother FX performance fixture is not distributed.
