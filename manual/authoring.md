# Create ZOIA patches

Rack40 modules represent ZOIA modules, parameters, and connections.
They do not process audio in VCV Rack.

For module descriptions, see the
[official ZOIA documentation](https://docs.google.com/spreadsheets/d/1a9q2OCo_Sd_IbWsH99wlFvjcTIN20e7wb5Qgn5fLxms/edit?gid=729380617#gid=729380617).

1. Add Rack40 modules from VCV Rack's module browser.
2. Right-click a module and choose **Module configuration** when you need a
   different ZOIA configuration.
3. Connect Rack40 jacks with Rack cables.
4. Right-click a Rack40 module and choose **Export ZOIA patch**.
5. Open the exported document in the Rack40 browser editor.
6. Review the patch, choose a ZOIA slot, and export its `.bin` file.
7. Transfer the `.bin` file to ZOIA using your normal SD-card workflow.

Rack40 rejects connections to ordinary VCV DSP modules during graph export.
This prevents a VCV-only signal path from being mistaken for a ZOIA patch.
