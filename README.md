# Rack40 for VCV Rack

**Build ZOIA patches like a modular synth inside VCV Rack.**

Rack40 is a visual patch-building interface for Empress ZOIA and Euroburo
hardware. Each Rack40 module represents a corresponding ZOIA module. Arrange
and connect them in VCV Rack, then export the patch for compilation and SD-card
transfer to the hardware. Rack40 also includes optional live MIDI control for a
compatible patch already loaded on ZOIA.

ZOIA remains the audio-processing engine. Rack40 does not emulate ZOIA DSP, and
its module jacks are not intended to connect to ordinary Rack DSP modules. ZOIA
or Euroburo hardware is required.

Download the Apple Silicon beta and read the installation instructions at the
[Rack40 for VCV Rack website](https://zoia-rack-lab.pages.dev/vcv/).

## Building from source

Build against the official matching VCV Rack 2 SDK:

```
python3 tools/generate_catalog.py
make RACK_DIR=/path/to/Rack-SDK
make dist RACK_DIR=/path/to/Rack-SDK
```

Rack40 modules use the canonical catalog. Their jacks describe ZOIA connections; they do not simulate audio or transmit topology to hardware. Right-click a ZOIA module and choose **Export ZOIA patch**. Open the document in the standalone editor to compile/export. For reverse exchange or preserved documents, save a Rack patch and use Rack40's documented browser/native exchange workflow to convert it to/from `.zoia.json`. Compile/load via SD card. Rack's browser shows one card per ZOIA module type; right-click Module configuration to select its variants after insertion. Stable variant model IDs remain loadable from saved sessions. Hardware configurations remain experimental until device-tested.

Live Control: right-click to import `.zoia-controls.json` and select MIDI output/channel. Load the matching patch on ZOIA, then enable LIVE. Eight knobs send configured CC values with optional additive CV (0–10V). CCs coalesce and are limited to 120 messages/second across the bank. Pitch/gate inputs send monophonic notes; 0V pitch is MIDI note 60. Test holds that note. Opening a saved patch starts muted; output/profile/channel changes mute and release a held note. No MIDI readback or pedal identity detection is claimed.

Audio: ZOIA → amp. MIDI: Mac → USB MIDI interface → included Type A adapter → ZOIA MIDI IN. Scarlett/audio interface is not required. SD handles patch/firmware transfer, not live MIDI.

Current limitations: connections to ordinary Rack DSP modules are rejected by export. Ordinary cables start at unity. Both hosts support multiple cables per input. Right-click a Rack40 jack to edit each ZOIA cable strength; strengths survive rewiring, module duplication with cables, and file exchange. This plugin does not implement DSP simulation or live topology sync.

GPL-3.0-only. Original panel/knob drawing code. Standard Rack port/switch widgets use installed Rack resources; no VCV artwork is redistributed in res/.

## Validation and installation

Run from the repository root:

```
RACK_DIR=/path/to/Rack-SDK bash scripts/check-native.sh
```

The SDK integration harness creates an isolated Rack engine and a recording-only MIDI driver; it never sends to physical devices. The harness uses Rack's internal Engine constructor only to stand up that test environment. Production code uses public module, model, MIDI and engine-access APIs. The public source includes the complete plugin build and catalog-generation inputs.

The reviewed direct-download build is `dist/Rack40-2.3.1-mac-arm64.vcvplugin`. It targets Apple Silicon Mac and VCV Rack 2.6.6; other platforms require their own matching SDK build or a VCV Library build.

## Known host issue: open menus during patch replacement

On VCV Rack Free 2.6.6 for macOS ARM64, loading another patch while a module context menu is open can leave the old menu attached to a deleted module. Hovering its **Info** submenu can crash Rack. This was reproduced with VCV's stock **Blank** module in a separate profile without Rack40 installed. An exact-value parameter menu also crashed during replacement in Rack40 testing; that parameter-specific case has not been separately reproduced with a stock module.

**Workaround:** close module, parameter, and cable menus with **Escape** or a click on empty rack space before opening a different patch.

Rack40 2.2.17 defensively dismisses its own module context menu on removal. That does not fix all menus in the host. No matching public upstream issue or acknowledged fix was located in the reviewed changelog and issue searches. This is a documented native-host limitation, not a defect in the Rack40 browser editor or a blocker for its web release. Broader host workarounds are deferred.

## Shared module panels (2.0.1)

Rack40 modules now use `shared/module-panels.v1.json`, packaged as `res/module-panels.json`. The standalone Rack40 editor uses the same definitions. Regenerate with `python3 native/RackZoia/tools/generate_catalog.py`; this also refreshes panels from `tools/panel_layouts.py` and the canonical module registry.

Layouts group actual parameters by module function and place each matching CV input below its knob. Panel widths accommodate the controls; codec IDs and parameter/jack order do not change. Right-click a module for its full configuration option listing. These are patch-building controls; live per-instance hardware binding is still under development.

Catalog update 2.0.2 adds Granular (mono/stereo and grain limits), Gate, Ping Pong Delay and CV Mixer configurations. There are now 140 configurations covering all 108 types in the pinned codec index, plus Live Control. This does not imply all firmware/options have been tested on hardware.

## Interaction audit (2.0.3)

See `docs/research/interaction-audit.md` in the repository root for the shared interaction contract, evidence and remaining host differences. Rack uses its native knob, module and cable gestures. Rack40's browser interface follows those default gestures while retaining ZOIA port types and raw parameter semantics. Rack bypass has no generic ZOIA equivalent and is rejected during export. No hardware topology protocol is claimed.

## Codec mapping update (2.2.1)
Shared catalog regenerated against Sheoak combined eaef3697. Existing model, parameter, and port identities remain stable; corrected hardware block positions and Delay Line option byte ordering are used for export. UI parameter order is deliberately independent of binary storage order. Pedal validation remains pending.

## Compact panels (2.2.2)

Shared panels now use narrower one-control layouts and compact stereo I/O rows, preserving all control/port keys. Gate mono is 120 logical pixels wide; Plate and Room Reverb are 180. These remain patch-building modules. Community skin loading is not enabled.


## Portable module presets

The module context menu's **Rack40 portable preset** submenu saves/loads `.rack40-module.json` control-state files shared with the browser. Load into the exact same catalog configuration; controls are validated before any mutation, and one history entry restores the prior state. Cables, MIDI devices and hardware positions are excluded. Preserved binary documents remain protected. Extra sequencer page/track banks are retained in portable presets, native saves/clones/history and the file bridge, including dormant cells and legacy embedded source banks. Invalid cells are rejected before preset controls change. Loading an older preset without a bank explicitly clears cached cells. Configuration changes with extra saved cells remain guarded; this does not certify firmware playback or add a native page/track editor. Rack's regular Preset menu still uses native `.vcvm` files. Native Module configuration switches compatible variants, preserving keyed controls and cables; removed sockets disconnect. One Undo restores the previous variant and wiring. Preserved source modules remain protected. Group duplication retains internal connection strengths. Live Control randomization disarms and releases notes. The history harness uses installed Rack assets (RACK_SYSTEM_DIR may override the macOS app resource path).

## Cable presentation exchange (2.2.5)

Explicit Rack cable colors now travel with ZOIA patch exports and native/browser file exchange. They affect only presentation, never ZOIA binary color data or signal behavior. Native port/knob gestures still come from the Rack host. GUI, API/history, and codec paths have been reviewed separately.

Rack browser consolidation (2.2.5): one visible card per ZOIA type, with catalog variants retained as loadable model IDs and available in Module configuration. The host API test verifies hidden variants still resolve.

## Editable-document exchange (2.2.10)

Browser-authored free graphs can switch ordinary module configurations in native Rack without treating their document metadata as a protected binary import. Configuration Undo retains the metadata. Save the Rack file and open it in Rack40 (or use the file bridge) to retain Notes, document identity, layout and weighted connections, even after removing the first module. Conflicting original documents, binary-backed topology changes and ambiguous extra saved-state edits fail explicitly.

The direct **Export ZOIA patch** shortcut still requires the file bridge when original document metadata is present. Extra native sequencer page/track editing remains pending. Version 2.2.10 has passed headless host/API and file tests; its GUI installation/restart is a separate validation step.


## Export file safety (2.2.11)

Native patch JSON, portable presets and control-map saves now serialize to a temporary file in the destination folder, flush it and replace the destination only after writing succeeds. A failed save preserves the previous file. The file bridge also prepares codec output before replacing exports and rejects source/output path collisions. Multiple output files are replaced individually; this is not a multi-file transaction or a power-loss durability guarantee.

The bridge rejects duplicate module/cable/control identities, invalid numeric/index types and nonfinite positions, and checks cable signal kinds in both directions. macOS headless tests cover successful replacement and failure preservation; the Windows implementation has not been built or tested here.

2.2.20 adds segmented configured-gain indicators beside VCA level, Audio Output gain (when enabled), Euro Headphone Amp level, and Audio Mixer channel gains. Eighteen segments represent estimated linear gain through unity; two amber segments indicate boost. They use the same approximate metadata curve as browser control readouts. These indicators do not measure audio, compression, clipping, or ZOIA modulation. Other filter/EQ/input/feedback gains are not treated as total output gain. The existing primary Rack session is not upgraded automatically.

### Reliability changes in 2.3.0

Live Control checks the selected output every 250 ms on a background monitor, independently of the Rack window and outside the audio callback. A missing device or failed subscription disarms control, attempts held-note release on the original subscription, and clears the selected output. A device returning with the same ID remains unselected and disarmed. Enumeration detects only what the MIDI driver reports; very brief disappear/reappear events between checks and DIN cable wiring cannot be identified. Recording-driver tests pass, and the owner confirmed audible VCV-to-ZOIA control with the Welcome/Bloom performance patch on November 13, 2026.


## Native Pushbutton in 2.3.0

Only the actual ZOIA Pushbutton module has the remote pad. The placed module and library preview use the same pad artwork. Action/normally remain official configuration options; the pad is not a new module or an audio engine.

Right-click the Pushbutton → Pushbutton Live MIDI setup. Choose Rack40 Live Control, physical zero-based pedal page, row/column and ZOIA Config MIDI channel. Select the MIDI output and enable Live Control. Verify the matching physical patch, the actual Pushbutton cell and current page with no pedal menu open, then confirm in the setup menu. A CC map is optional for remote buttons; an unassigned controller sends no unrelated CC controls. Page state is not read back. Reconfirm after changing pedal pages.

Momentary action holds while the mouse or Space/Enter is held; latching action sends a bounded press/release pair without pretending to know the pedal's toggled state. Pointer leave, drag end, deselection, hide/removal and the controller's independent 500ms renewal lease release held presses. Stop, output/profile changes, driver failures, disconnect and state restore invalidate confirmations. Runtime controller/page/cell association is not saved, cloned or exported as patch wiring. Set it up again after reloading or cloning. Existing browser binding metadata is not automatically reused by this native candidate.

SDK/API and recording-only event tests pass. Physical Pushbutton timing remains an explicitly documented hardware follow-up rather than a claim of pedal certification.

## Example status

The Welcome/Bloom performance example passed the owner listening check. The separate Mother FX performance fixture did not produce audio during the final owner check and is excluded from the distributable; it is not a verified release example.
