Subject: MiniSpeedCam r1.1 — PCB design review ahead of the final spin

Hi [name],

Ahead of the last PCB revision, I ran an automated design-review pass over the r1.1 KiCad
files (schematic + layout + gerbers) and triaged the results by hand. Full report attached
(PDF) — here's the short version so you can decide where to push back.

The board is complete, fully routed, and mechanically clean. One theme dominates and is worth
a real conversation:

  The 24 GHz Doppler analog front-end shares the digital supply and ground with the ESP32-S3.
  - The radar amplifiers (LMP7731 + MAX9814) are powered from the digital 3V3 rail.
  - The isolated DC-DC (B0505S) that feeds the radar has its secondary ground tied straight
    back to digital GND, so its isolation isn't actually doing anything for noise.

  This is almost certainly why the firmware has to blank the radar during every WiFi burst and
  run WiFi at reduced TX power. Fixing it in hardware — a dedicated low-noise analog rail for
  the amps + a single-point analog ground — is the highest-value change for the final spin.

Secondary items (all standard pre-fab cleanup):
  - No EMC filtering on the power inlet (J1) or the two USB-C ports — add TVS + common-mode chokes.
  - Ground/3V3 planes read as fragmented with signals crossing the gaps; camera clock changes
    layers with no return via. Worth a quick plane-integrity check.
  - DFM: no fiducials, no test points, and two parts (C6, FB1) sit ~0.25 mm from the board edge.
  - BOM: only 3/37 parts have MPNs in the symbols (the LCSC numbers live in the BOM CSV, not the
    schematic) — a documentation gate, not an electrical issue.

Things I could NOT fully verify (so treat with judgement):
  - No SPICE was run (no simulator on hand), so front-end gain/filter values aren't simulated.
  - The LMP7731 supply pin is datasheet-confirmed, but I couldn't render the datasheet figure to
    confirm the op-amp's input/output pin order — please eyeball that against the connection
    diagram. It uses the standard SOT-23-5 single-op-amp layout, so most likely fine.

Happy to walk through any of it. My main ask for the review: does the analog-front-end isolation
change fit in this spin, or do we phase it?

Thanks,
Daniel

---
Attachment: MiniSpeedCam-r1.1-design-review.pdf  (Valar Systems · Doc MSC-R1.1-DR-001 Rev A)
