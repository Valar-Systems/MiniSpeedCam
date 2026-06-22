# MiniSpeedCam r1.1 — PCB Design Review

**Valar Systems — Hardware Engineering** · Doc MSC-R1.1-DR-001 Rev A · 2026-06-22 · **CONFIDENTIAL**

Radar speed-camera motherboard (ESP32-S3 + STM32F301 + CDM324 24 GHz Doppler) · 4-layer, 50 × 50 mm
Repo: `Valar-Systems/MiniSpeedCam @ version/r1.1` · Method: kicad-happy analyzer suite

| Components | Stack | Routed | EMC risk | Errors / Warnings |
|---|---|---|---|---|
| 94 | 4-layer (F / GND / PWR / B), 1.6 mm | 100% | 10 / 100 | 7 / 32 |

---

## 1. Scope & Method

Produced by cloning the open-source [kicad-happy](https://github.com/aklofas/kicad-happy) review suite and running its
Python analyzers against the **r1.1** KiCad source (`version/r1.1/hardware/pcb/kicad/`). Findings carry analyzer rule IDs
and were triaged by hand against the raw netlist.

| Analyzer | Status | Result |
|---|---|---|
| analyze_schematic.py | RAN | 94 parts, 101 nets, 37 unique — 1 error / 3 warnings |
| analyze_pcb.py --full | RAN | 4-layer, 50×50 mm, fully routed — 6 errors / 26 warnings |
| analyze_gerbers.py | RAN | 11 layers complete, PTH+NPTH — 1 warning |
| cross_analysis.py | RAN | 6 cross-domain findings |
| analyze_emc.py (44 rules) | RAN | 65 findings, EMC risk 10/100 (conservative) |
| Datasheet verification | PARTIAL | Front-end supply/ground pins confirmed (§6); op-amp signal-pin order is a designer check |
| analyze_thermal.py | PARTIAL | Ran, but no power-budget input → junction temps not computed |
| SPICE | SKIPPED | No simulator on host |

Layer stack: `F.Cu` / `In1.Cu = GND plane` / `In2.Cu = split power plane` / `B.Cu`, 1.6 mm.

## 2. Executive Summary

The board is complete, fully routed, and mechanically tidy. The issues that matter cluster around one theme: **the sensitive
24 GHz Doppler analog front-end is not adequately isolated from the digital/RF noise of the ESP32-S3.** This is the electrical
root cause behind the firmware workarounds in the codebase (a radar-blanking handshake that discards FFT frames during every
WiFi burst, plus WiFi TX power reduced to 11 dBm). A final spin should spend its budget here. The rest are standard pre-fab
DFM hygiene items.

> **Top priority** — Give the radar amplifier chain (U4 LMP7731, U12 MAX9814) a dedicated low-noise analog supply and a
> single-point analog ground, instead of the shared digital `3V3` / `GND`. This is the change most likely to remove the need
> for firmware radar-blanking and let WiFi TX power return to full range.

## 3. Priority Finding — Radar Front-End Shares the Digital Supply & Ground

```
USB 5V (VUSB) -> U5 B0505S (isolated DC-DC) -> +Vo -> FB1 ferrite -> CDM324 V+   [GOOD: dedicated, filtered diode rail]
                       |
                       +- U5.1 (-Vin)=GND  and  U5.3 (0V, secondary gnd)=GND     [isolation collapsed to common ground]

CDM324 IF out -> C11/R12 (AC couple) -> U4 LMP7731 (gain) -> U12 MAX9814 (AGC) -> ADC
                                         U4.V+ = 3V3              U12.VDD = 3V3   [uV signal amplified on the noisy digital rail]
```

| Observation | Evidence (netlist) | Severity |
|---|---|---|
| Doppler amplifiers on shared digital rail | `U4.5 (V+) → 3V3` and `U12.5 (VDD) → 3V3` — same rail as ESP32-S3/STM32 logic; a WiFi-TX transient on 3V3 lands in the gain stage. Both supply pins datasheet-confirmed (§6). | **HIGH** |
| Isolated converter ground bonded to digital ground | `U5.1 (−Vin) → GND` and `U5.3 (0V) → GND`; no analog/isolated ground net exists in the design. The B0505S isolation is unused — radar return shares digital ground. | **HIGH** |
| Diode rail done well | `U5.4 (+Vo) → FB1 → J3.3 (CDM324 V+)` with C22 bulk — a separate, ferrite-filtered radar supply. | OK |

**Recommended changes**

- Derive a clean analog rail for U4/U12 from the isolated B0505S `+Vo` via a small **low-noise (high-PSRR) LDO**, not the digital `3V3`.
- Give the front-end a local **analog ground island**, tied to digital `GND` at a **single star point** near the STM32 ADC return.
- If the B0505S stays, decide deliberately whether its secondary `0V` joins `GND` at one point (quiet) or not at all (true isolation). Today it's a common-ground bond, giving up most of the part's benefit.

*Note: `VOUT` is the MAX9814 audio output to the ADC, not a supply — so `RS-001 "VOUT has no source"` is a false positive.*

## 4. Ground & Reference-Plane Integrity

| Rule | Severity | Finding & action |
|---|---|---|
| PS-002 / GP-001 | ERROR | `GND` resolves into **6 copper islands, 2 signals crossing a gap**; `3V3` into **23 islands**, 2 crossing. Verify `In1.Cu` is a continuous ground reference (no traces carved through it) and stitch the pours. 3V3 fragmentation matters here because the analog amps reference 3V3. |
| RP-001 | ERROR | Camera `PCLK` changes layers with **no return-path stitching via**; same for several STM/UART/BOOT0 nets. Add a ground via at each clock layer-transition. |

## 5. EMC Pre-Compliance & DFM

| Rule | Severity | Finding & action |
|---|---|---|
| IO-001 | ERROR | No filtering on **J1 (Phoenix power input), USB1, USB2**. Add TVS + pi/ferrite on the J1 inlet; common-mode choke + ESD on each USB-C data pair. |
| CK-001 / CK-003 | WARN | Camera `XCLK`/`PCLK` and `SD_HOST_CLK` on outer layers, routed near connectors J8/J10 — move inner-layer or add guard ground. |
| DC-003 | WARN | 13 decoupling caps (C6/C7/C9/C12/C17/C42…) far from their plane via — shorten cap→via path to cut PDN inductance. |
| FD-001 | ERROR | **No fiducials** either side (fine-pitch QFN present) — add ≥3 global fiducials. |
| PM-002 | ERROR | **C6 and FB1 sit 0.25 mm from board edge** — move in ≥0.5 mm (crack risk on depanel). J1/J3 overhang is intentional edge-mount — ignore. |
| TE-001 | WARN | **0 test points.** Add TPs on every rail, the radar analog output, and GND scope points. |
| DFM-001 | WARN | 0.1 mm / 0.116 mm trace+space forces JLC **advanced** tier. Relax non-camera nets to 0.127 mm to drop to standard if cost matters. |
| TV-001 | WARN | STM32 (U13) thermal pad has 5/9 recommended vias — top up to 9 (minor). |
| SS-001 | ERROR | MPN coverage 3/37 in symbols. Populate MPN fields from the existing `MiniSpeedCam_BOM.csv` LCSC column — BOM hygiene, not an electrical defect. |

## 6. Datasheet Verification

| Part | Pin checked | Result | Basis |
|---|---|---|---|
| U5 B0505S-1W | 1=−Vin, 2=+Vin, 3=−Vout(0V), 4=+Vout | CONFIRMED | Standard B0505S 4-pin isolated DC-DC pinout. −Vin and −Vout both on `GND` verified. |
| U12 MAX9814 | 5=VDD, 6=MICOUT, 8=MICIN, GND=4/7/11/EP | CONFIRMED | Exact match to documented MAX9814 14-pin TDFN. `VDD → 3V3` verified. |
| U4 LMP7731 | 5 = V+ | CONFIRMED | Pin 5 = V+ for SOT-23-5 single op-amp. `V+ → 3V3` verified. |
| U4 LMP7731 | 1–4 = OUT/+IN/−IN order | DESIGNER CHECK | Symbol uses standard SOT-23-5 order (1=OUT, 2=V−, 3=+IN, 4=−IN). Datasheet figure couldn't be auto-rendered here — confirm visually against the LMP7731 connection diagram before fab. Not a flagged discrepancy. |

## 7. Triaged as False-Positive / Expected (no action)

- `CC-002` (16×) "narrow signal" on camera/I²C nets — 0.1 mm is fine for low-current signals.
- `GR-004` B-paste 26/187 apertures — back side has only 2 components; expected.
- `DP-005` USB diff-pair 7% / 1.3 mm mismatch — negligible at USB full-speed.
- `PM-001` courtyard overlaps (U15/C2, U16/C26, …) — decoupling caps deliberately under their LDOs.
- `RS-001` on `VOUT` — `VOUT` is a signal, not a power rail.

## 8. Review Gaps & Confidence

- **No full datasheet sync.** Front-end supply/ground pins spot-verified (§6); LMP7731 I/O pin order and STM32/ESP pin maps not exhaustively checked against rendered figures.
- **No SPICE simulation** (no simulator on host): gain/AGC/filter cutoffs not numerically verified.
- **Thermal inconclusive:** no power-budget data, so junction temps not computed. Unlikely to be a blocker, but not positively cleared.

> **Bottom line:** r1.1 is electrically functional and routable, but its radar receive path is coupled to the digital domain
> through both supply (3V3) and ground. Fixing that — a dedicated low-noise analog rail + single-point analog ground for U4/U12,
> a continuous ground reference, and basic I/O filtering — is the highest-value content for a final spin and directly targets
> the noise problem the firmware is currently working around.

---

*Generated with the kicad-happy analyzer suite. Rule IDs reference kicad-happy detectors. Findings are engineering guidance
for human review, not a substitute for DRC/ERC sign-off or full datasheet verification.*
