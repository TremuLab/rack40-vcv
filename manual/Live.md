# Live Control

Rack40 Live Control sends user-directed MIDI to a matching patch on real ZOIA
hardware. It has eight MIDI CC macros plus monophonic pitch and gate inputs.

1. Load the matching patch on ZOIA.
2. Right-click Live Control and select the correct MIDI output and channel.
3. Import a Rack40 control map if the patch provides one.
4. Confirm the physical MIDI connection reaches ZOIA MIDI IN.
5. Deliberately enable **LIVE**.

Opening a saved patch starts muted. Changing the MIDI output, driver, profile,
or channel also mutes the controller and releases a held note. If the selected
MIDI device disappears, Rack40 disarms rather than automatically resuming when
the device returns.

Rack40 does not read ZOIA's state back from the pedal and does not identify the
connected hardware. Verify the loaded patch and MIDI channel before enabling
LIVE.
