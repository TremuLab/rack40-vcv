# Installation and requirements

## Requirements

- VCV Rack 2
- a real Empress Effects ZOIA or Euroburo
- for live control: a MIDI connection from the computer to ZOIA MIDI IN

The reviewed direct-download package is for **Apple Silicon macOS** and was
tested with **VCV Rack Free 2.6.6**. It is not a Windows, Linux, or Intel Mac
build.

## Direct-download installation on Apple Silicon Mac

1. Confirm that your Mac uses Apple Silicon (M1, M2, M3, M4, or newer), not
   an Intel processor.
2. Download the Rack40 file ending in `.vcvplugin`. Do not unzip or rename it.
3. In VCV Rack, choose **Help → Open user folder**.
4. Open the `plugins-mac-arm64` folder inside the Rack user folder. Create that
   folder if it does not already exist.
5. Put the downloaded `.vcvplugin` file inside `plugins-mac-arm64`.
6. Fully quit VCV Rack, then reopen it.
7. Open the module browser and search for the brand **Rack40**.

## If Rack40 does not appear

- Confirm that the computer is an Apple Silicon Mac.
- Confirm that the unchanged `.vcvplugin` file is directly inside
  `plugins-mac-arm64`, not inside another folder.
- Search for **Rack40** as the module brand.
- Open **Help → Open user folder** and inspect `log.txt` for a Rack40 loading
  error.

## Create your first ZOIA patch

1. Add Rack40 modules from VCV Rack's module browser.
2. Right-click a module and choose **Module configuration** when you need a
   different ZOIA configuration.
3. Connect the Rack40 modules with Rack cables.
4. Right-click a Rack40 module and choose **Export ZOIA patch**.
5. Open the exported document in the Rack40 browser editor.
6. Review the patch, choose a ZOIA slot, and export the `.bin` file.
7. Transfer the `.bin` file to ZOIA with an SD card.

Live Control is a separate, optional MIDI workflow. Finish the basic patch
workflow first, then see [Live Control](Live.md) if you want to control a
matching patch loaded on ZOIA.

Rack40 modules require ZOIA hardware. They do not generate ZOIA audio on the
computer.

For ZOIA module descriptions, see the
[official documentation](https://docs.google.com/spreadsheets/d/1a9q2OCo_Sd_IbWsH99wlFvjcTIN20e7wb5Qgn5fLxms/edit?gid=729380617#gid=729380617).
