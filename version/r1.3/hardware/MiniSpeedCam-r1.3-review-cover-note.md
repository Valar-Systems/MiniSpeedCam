Subject: MiniSpeedCam r1.3 — PCB design review

Hi [name],

I ran an automated design-review pass over the r1.3 KiCad files (schematic + layout) and triaged
the results by hand. Full report attached (PDF) — here's the short version so you can decide where
to push back.

The board is complete, fully routed, and mechanically clean on the 50×50 mm 4-layer stack. One
theme dominates and is worth a real conversation:

  The 24 GHz Doppler analog front-end shares the digital supply and ground with the ESP32-S3.
  - The radar amplifiers (LMP7731 + MAX9814) are powered from the digital 3V3 rail, which is
    sourced from a switching regulator.
  - The isolated DC-DC (B0505S) that feeds the radar has its secondary ground tied straight back
    to digital GND, so its isolation isn't actually doing anything for noise.

  This is the kind of coupling that forces firmware-side mitigation — today the firmware blanks the
  radar during WiFi bursts and runs WiFi at reduced TX power. Fixing it in hardware (a dedicated
  low-noise analog rail for the amps + a single-point analog ground) is the highest-value change.

One power-architecture item worth a deliberate decision:
  - Three power sources land directly on the 5V net — the Phoenix inlet, the barrel jack, and USB-C
    VBUS — with no ORing / reverse-blocking that I can see. If two can be plugged at once, that's a
    back-feed path. Suggest ideal-diode ORing or Schottky blocking per source (or a deliberate
    "external OR USB, never both").

Secondary items (standard pre-fab cleanup):
  - No EMC filtering on the DC inlets (Phoenix + barrel jack) or USB — add TVS + ferrite/ESD.
  - Ground/3V3 planes read as fragmented with a couple of signals crossing the gaps; camera clocks
    (XCLK/PCLK) change layers with no return via. Worth a plane-integrity + stitching pass.
  - DFM: no fiducials, no test points, two parts (C6, FB1) sit ~0.25 mm from the board edge, and
    U13's thermal pad has 5/9 vias.
  - BOM: only 3/36 parts carry MPNs in the symbols (LCSC numbers are in the symbol property) — a
    documentation gate, not an electrical issue.
  - No Gerber set is committed yet — needs export + a gerber check before release.

Things I could NOT fully verify (so treat with judgement):
  - No Gerbers to check, and no SPICE was run, so front-end gain/filter values aren't simulated.
  - The LMP7731 supply pin is confirmed, but please eyeball the op-amp's input/output pin order
    against the connection diagram. It uses the standard SOT-23-5 layout, so most likely fine.

Happy to walk through any of it. My main ask: does the analog-front-end isolation change fit in
this spin, or do we phase it?

Thanks,
Daniel

---
Attachment: MiniSpeedCam-r1.3-design-review.pdf  (Valar Systems · Doc MSC-R1.3-DR-001 Rev A)
