# MiniSpeedCam r1.3 — PCB Design Review

**Valar Systems — Hardware Engineering** · Doc MSC-R1.3-DR-001 Rev A · 2026-06-22 · **CONFIDENTIAL**

Radar speed-camera motherboard (ESP32-S3 + STM32F301 + CDM324 24 GHz Doppler) · 4-layer, 50 × 50 mm
Repo: `Valar-Systems/MiniSpeedCam @ version/r1.3` · Method: kicad-happy analyzer suite

| Components | Stack | Routed | EMC risk | Errors / Warnings |
|---|---|---|---|---|
| 84 | 4-layer (F / GND / PWR / B), 1.6 mm | 100% | 13 / 100 | 7 / 31 |

---

## 1. Scope & Method

The open-source [kicad-happy](https://github.com/aklofas/kicad-happy) analyzer suite was run against the r1.3 KiCad source
(`version/r1.3/hardware/pcb/kicad/MiniSpeedCam-r1.3.{kicad_sch,kicad_pcb}`), and the results were triaged by hand against the raw
netlist. Findings carry the analyzer rule IDs.

| Analyzer | Status | Result |
|---|---|---|
| analyze_schematic.py | RAN | 84 parts, 92 nets, 36 unique — 1 error / 5 warnings / 37 info |
| analyze_pcb.py --full | RAN | 4-layer, 50×50 mm, fully routed (0 unrouted) — 6 errors / 26 warnings / 50 info |
| cross_analysis.py | RAN | 5 plane-integrity findings (GND / 3V3 fragmentation) |
| analyze_emc.py (US market) | RAN | 61 findings, EMC risk 13/100 (conservative) |
| analyze_gerbers.py | **SKIPPED** | **No Gerber/drill set exported yet** — copper/aperture/drill DFM not yet checked (release gate, see §6) |
| Datasheet verification | PARTIAL | Front-end supply/ground pins confirmed against the netlist (§7); op-amp signal-pin order is a designer check |
| analyze_thermal.py | PARTIAL | Ran, but no power-budget input → junction temps not computed |
| SPICE | SKIPPED | No simulator on host |

Layer stack: `F.Cu` / `In1.Cu = GND plane` / `In2.Cu = power plane` / `B.Cu`, 1.6 mm.

## 2. Executive Summary

The board is complete, fully routed, and mechanically tidy on a 50 × 50 mm 4-layer stack. The issue that matters most clusters around
one theme: **the sensitive 24 GHz Doppler analog front-end is not isolated from the digital/RF noise of the ESP32-S3.** The radar
amplifiers run from the digital `3V3` rail and reference the digital ground, and the isolated DC-DC that feeds the radar has its
secondary ground bonded straight back to digital `GND`. This is the kind of coupling that forces firmware-side mitigation — the
current firmware blanks the radar during WiFi bursts and runs WiFi at reduced TX power to keep readings usable. Fixing it in hardware
is the highest-value content for this board.

There is also **one power-architecture item worth a deliberate decision** (§3.2): **three power sources — the Phoenix inlet J1, the
barrel jack DC1, and USB-C VBUS (USB2) — land directly on the `5V` net** with no ORing / reverse-blocking shown. If two can be plugged
at once, that's a back-feed / contention path. The rest are standard pre-fab DFM and EMC hygiene items.

> **Top priority** — Give the radar amplifier chain (U4 LMP7731, U12 MAX9814) a dedicated low-noise analog supply and a single-point
> analog ground, instead of the shared digital `3V3` / `GND`. This is the change most likely to remove the need for firmware
> radar-blanking and let WiFi TX power return to full range.

## 3. Priority Findings

### 3.1 Radar Front-End Shares the Digital Supply & Ground (HIGH)

```
RADAR SUPPLY  [GOOD: dedicated, filtered diode rail]
  5V -> U5 B0505S (isolated DC-DC) -> +Vo (pin4)
        -> FB1 ferrite -> C22 bulk -> CDM324 V+

RADAR SIGNAL  [uV signal amplified on the noisy digital rail]
  CDM324 IF out -> C11/R12 (AC couple)
    -> U4 LMP7731 (gain)   [U4.5 (V+)  = 3V3]
    -> U12 MAX9814 (AGC)   [U12.2/5 (VDD) = 3V3]
    -> ADC

GROUND: single GND net (no analog island); U5 secondary 0V tied to GND
```

| Observation | Evidence (netlist) | Severity |
|---|---|---|
| Doppler amplifiers on shared digital rail | `U4.5 (V+) → 3V3` and `U12.2/5 (VDD) → 3V3` — the same rail the ESP32-S3 / STM32 logic runs on. `3V3` is sourced from the **U9 AP63203 *switching* regulator**, so the amps see switching ripple as well as WiFi-TX transients. Supply pins confirmed (§7). | **HIGH** |
| Isolated converter ground bonded to digital ground | `U5.1 (−Vin) → GND` and `U5.3 (0V) → GND`; no analog/isolated ground net exists. The B0505S isolation buys nothing for noise — the radar return shares digital ground. | **HIGH** |
| Diode rail done well | `U5.4 (+Vo) → FB1 → C22` then on to the CDM324 — a separate, ferrite-filtered radar supply. | OK |

**Recommended changes**

- Derive a clean analog rail for U4/U12 from the isolated B0505S `+Vo` via a small **low-noise (high-PSRR) LDO** — explicitly *not*
  the switcher-derived digital `3V3`.
- Give the front-end a local **analog ground island**, tied to digital `GND` at a **single star point** near the STM32 ADC return.
- Decide deliberately whether the B0505S secondary `0V` joins `GND` at one point (quiet) or stays isolated (true isolation). Today it's
  a common-ground bond, giving up most of the part's benefit.

*Note: `VOUT` is the MAX9814 audio output to the ADC, not a supply — so `RS-001 "VOUT has no source"` is a false positive (§8).*

### 3.2 Three Power Sources Merged on `5V` (MEDIUM)

The `5V` net is driven by **three inlets in parallel** — Phoenix `J1.1`, barrel jack `DC1.4`, and USB-C `USB2.VBUS (pins 2/11)` —
plus `U5.+Vin` and the `U9` switcher input all on the same node. The only series parts on the net are `D1` and `R1`.

- If a user can have **USB plugged *and* the barrel jack / Phoenix energized at the same time**, current can back-feed from one source
  into another; at minimum the host USB port sees the external supply on its VBUS.
- **Action:** add ideal-diode ORing or Schottky reverse-blocking per source (or a deliberate "external *or* USB, never both" mechanical
  interlock / 0Ω select). Confirm D1's role — if it's the only reverse protection it should sit on each source, not just the rail.

## 4. Ground / Plane Integrity & EMC

| Rule | Severity | Finding & action |
|---|---|---|
| PS-002 | WARN | `GND` resolves into **5 copper islands, 2 signals crossing (GPIO43, GPIO44)**; `3V3` into **22 islands, 2 crossing**. Verify `In1.Cu` is a continuous ground reference (no traces carved through it) and stitch the pours. 3V3 fragmentation matters because the analog amps reference 3V3. |
| GP-001 | ERROR (×2) | Two signals cross a significant reference-plane gap (plus several partial). Reroute to avoid the void, or add a bridge cap if a split is intentional. |
| RP-001 | ERROR (×2) | `CAM.PCLK` and `CAM.XCLK` change layers with **no return-path stitching via** (plus ~20 warning-level nets: camera data, UART, BOOT0). Add a ground via at each clock layer-transition. |
| IO-001 | ERROR (×2) | No EMC filtering near the DC inlets / USB. Add TVS + pi/ferrite on the **J1 Phoenix inlet** and the **DC1 barrel jack**; add common-mode choke + ESD on the **USB2** data pair. |
| CK-001 / CK-003 | WARN | Camera `XCLK`/`PCLK` and `SD_HOST_CLK` on outer layers, routed near connectors J8/J10 — move inner-layer or add guard ground. |
| DC-003 | WARN | ~13 decoupling caps (C6/C7/C9/C11/C12/C17/C42…) far from their plane via — shorten cap→via path to cut PDN inductance. |

EMC pre-compliance score **13/100** (conservative) — the analog-isolation and I/O-filtering gaps dominate.

## 5. DFM / Assembly

| Rule | Severity | Finding & action |
|---|---|---|
| FD-001 | ERROR | **No fiducials** either side (fine-pitch QFN present) — add ≥3 global fiducials. |
| PM-002 | ERROR | **C6 and FB1 sit 0.25 mm from board edge** — move in ≥0.5 mm (crack risk on depanel). J1/J3 overhang is intentional edge-mount; J4 at 0.52 mm is borderline. |
| TE-001 | WARN | **0 test points / 91 nets.** Add TPs on every rail, the radar analog output, and GND scope points. |
| DFM-001 | WARN | 0.1 mm / 0.116 mm trace+space forces JLC **advanced** tier. Relax non-camera nets to 0.127 mm to drop to standard if cost matters. |
| PM-001 | WARN | Courtyard overlaps (U15/C2, U16/C26, U15/C56) — decoupling caps deliberately under their LDOs; confirm and waive. |
| TV-001 | WARN | STM32 (U13) thermal pad has 5/9 recommended vias — top up to 9 (minor). |
| SS-001 | ERROR | MPN coverage **3/36 unique parts (8.3%)** in symbols — pre-fab sourcing blocker. Populate MPN fields from the LCSC numbers already in the symbols' `lcsc` property. |

## 6. Schematic / Release Items

| Rule | Severity | Finding & action |
|---|---|---|
| NT-001 | WARN | STM32 `U13.PA2` and `U13.PA3` are **single-pin (floating) bidirectional nets** — add wires or no-connect flags so ERC is clean and the pins aren't left undefined. |
| RS-001 / pwr_flag | WARN | `5V` (and `1V2` / `2V8` / `3V3`) carry power_in pins but have no `PWR_FLAG` / power_out — ERC housekeeping; add a `PWR_FLAG` on the inlets. |
| GERBER | GATE | **No Gerber/drill set committed.** Export the fab package and re-run `analyze_gerbers.py` before release — copper/aperture/drill DFM is otherwise unverified. |

## 7. Datasheet Verification

| Part | Pin checked | Result | Basis |
|---|---|---|---|
| U5 B0505S-1W | 1=−Vin, 2=+Vin, 3=−Vout(0V), 4=+Vout | CONFIRMED | Standard B0505S 4-pin isolated DC-DC. `−Vin` and `0V` both on `GND` verified in netlist. |
| U12 MAX9814 | 5=VDD, 6=MICOUT, 8=MICIN, GND=4/7/9/10/11/15 | CONFIRMED | MAX9814 14-pin TDFN. `VDD → 3V3` verified. |
| U4 LMP7731 | 5 = V+ | CONFIRMED | Pin 5 = V+ for SOT-23-5 single op-amp. `V+ → 3V3` verified. |
| U4 LMP7731 | 1–4 = OUT/+IN/−IN order | **DESIGNER CHECK** | Symbol uses standard SOT-23-5 order (1=OUT, 2=V−, 3=+IN, 4=−IN). Confirm visually against the LMP7731 connection diagram before fab. Not a flagged discrepancy. |

## 8. Triaged as False-Positive / Expected (no action)

- `RS-001` on `VOUT` — `VOUT` is the MAX9814 audio output to the ADC, not a power rail.
- `CC-002` (~18×) "narrow signal" on camera / I²C nets — 0.1 mm is fine for low-current signals.
- `PM-001` courtyard overlaps (U15/C2, U16/C26, U15/C56) — decoupling caps deliberately under their LDOs.
- `DP-004` USB diff pair on outer layer — acceptable for this short full-speed run.

## 9. Review Gaps & Confidence

- **No Gerber check** (none exported) — copper / aperture / drill DFM not positively cleared.
- **No SPICE simulation** (no simulator on host): front-end gain/AGC/filter cutoffs not numerically verified (RC corners detected: R12/C11 ≈ 72 Hz, R12/C42 ≈ 1.25 MHz).
- **Thermal inconclusive:** no power-budget data, so junction temps not computed. Unlikely a blocker, but not positively cleared.
- **Datasheet sync partial:** front-end supply/ground pins verified; LMP7731 I/O pin order and the STM32/ESP pin maps not exhaustively checked against rendered figures.

> **Bottom line:** the board is electrically functional and fully routed, but its radar receive path is coupled to the digital domain
> through both supply (`3V3`) and ground. Fixing that — a dedicated low-noise analog rail + single-point analog ground for U4/U12, a
> continuous ground reference, and basic I/O filtering — is the highest-value work, and directly targets the noise problem the firmware
> is currently working around. Before fabricating, also close the 3-source `5V` merge (§3.2), export a Gerber set, and clear the
> standing DFM gates (fiducials, edge clearance, test points, MPN coverage).

---

*Generated with the kicad-happy analyzer suite. Rule IDs reference kicad-happy detectors. Findings are engineering guidance for human
review, not a substitute for DRC/ERC sign-off or full datasheet verification.*
