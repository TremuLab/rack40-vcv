# Rack40 for VCV Rack

**ZOIA hardware is required.** Rack40 modules describe patches for
a real Empress Effects ZOIA or Euroburo. They do not reproduce ZOIA audio DSP
inside VCV Rack.

Rack40 provides two related workflows:

1. Build and inspect a ZOIA patch graph with Rack-style modules and cables,
   then export the graph for the Rack40 browser editor to compile as a ZOIA
   `.bin` patch.
2. Enable Live Control to send MIDI controls and notes to a matching patch
   loaded on connected ZOIA hardware.

The initial direct-download build supports Apple Silicon macOS and was tested
with VCV Rack Free 2.6.6. Other platforms require their own matching Rack SDK
build or a later VCV Library build.

## Important host safety note

Close every module, parameter, and cable menu before loading or replacing a
VCV patch. VCV Rack 2.6.6 can leave a menu attached to a removed module; this
host behavior also reproduces with a stock module.

See [Installation](installation.md), [Create ZOIA patches](authoring.md),
and [Live Control](Live.md). For individual ZOIA module descriptions, see the
[official documentation](https://docs.google.com/spreadsheets/d/1a9q2OCo_Sd_IbWsH99wlFvjcTIN20e7wb5Qgn5fLxms/edit?gid=729380617#gid=729380617).
