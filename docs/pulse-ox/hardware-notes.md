# BHV badge pulse-ox — hardware notes

Derived by reading the KiCad sources in
[nathantsmith/BHV_Badge_2026](https://github.com/nathantsmith/BHV_Badge_2026) directly (schematic, PCB,
BOM, and the Bambu Studio enclosure), cross-checked against serial captures from a real badge.

Everything here is stated with its evidence so it can be re-derived or disproved.

---

## 1. Defects to resolve before boards are manufactured

These are the only items on this page with a deadline. If boards and parts have already been ordered,
items 1 and 2 still matter — they change what arrives in the bag.

### 1.1 D17 is missing from the BOM — building to it leaves all 14 LEDs dark

`D17` (BAT54W-HG3-18) is in the schematic with `(in_bom yes) (on_board yes) (dnp no)` and placed on the
PCB, but `bhvBadge2026.csv` lists only `"D15,D16","2","BAT54W-HG3-18"`.

Why it is not cosmetic: net `Net-(D1-VDD)` has exactly two pads — D17's cathode and D1's VDD. D1 is the
**head of the WS2812B data chain** (`LED_SIGNAL → R3 → D1.DIN`, then `D1.DOUT → D2 … → D14`). An
unpowered WS2812B never regenerates its data output, so an absent D17 means **all fourteen LEDs stay dark,
permanently**, with nothing in firmware reporting it. It presents exactly like a GPIO47/RMT fault, so it
would likely be chased in software first.

**Fix:** regenerate the BOM from the schematic. BAT54W-HG3-18 quantity **3**, not 2. A reversed SOD-123
produces the identical symptom, so first-article assembly should verify the cathode band.

### 1.2 C4 has no value anywhere in the design

`C4`'s schematic Value field contains the literal library symbol name `C_Polarized`, with Datasheet `~`;
the BOM row matches. Its `CP_Elec_8x10.5` footprint accepts anything from roughly 100 µF to 470 µF.
**Nobody can order this part** without guessing.

It also matters functionally: C4 is the LED rail's bulk reservoir, and its value sets the `+5VL` turn-on
ramp. Firmware previously assumed 1 ms; the real ramp to the WS2812B's 3.5 V minimum is roughly 7 ms at
220 µF and ~11 ms at 470 µF. (The firmware side is fixed — see `HeartbeatPixelThread::powerStrips()` —
but the part still needs a value.)

**Fix:** specify 100 µF / 10 V low-ESR, or a pair of 100 µF / 10 V X5R MLCCs which removes ESR as a
variable and fits the same keep-out.

### 1.3 R3 has three conflicting values

PCB and schematic say **220 Ω**; the BOM says **500**; and the BOM's own datasheet link points at an
RC0603JR-13680RL, which is **680 Ω**. Electrically survivable, but it is the proof that the CSV is stale
relative to the schematic — which is how D17 fell out in the first place.

**Fix:** regenerate the BOM rather than patching individual rows.

---

## 2. What the hardware actually is

| Item | Finding |
|---|---|
| Sensor | **GY-MAX30102 breakout module** on a 5-pin 2.54 mm header (VIN, GND, SCL, SDA, INT) |
| Sensor supply | **+3V3** (not 5 V). The module carries its own regulator and decoupling |
| I²C | SDA = Heltec **GPIO4**, SCL = **GPIO3** |
| Sensor face | **B.Cu** (back); all 14 WS2812Bs are on **F.Cu** (front) |
| Board | 2-layer, ~1.51 mm FR4, copper pours on both faces |
| LED data | `LED_SIGNAL` = GPIO47, two strips of 7 |
| LED power gate | `ENABLE_VLED` = **GPIO48** via Q1 (2N7002H) + Q2 (DMP2012SN) high-side switch |
| LED rail | TPS61040 boost, R1 620k / R2 200k → Vout ≈ 1.233 × (1 + 620/200) ≈ **5.05 V** |
| Enclosure | Two-part: red PLA frame + translucent TPU 95A light guide over the LEDs |

### 2.1 Measure geometry from the die, not the footprint origin

The header footprint origin sits at (148.876, 135.537), but the **MAX30102 die is ~10–11 mm away in local
coordinates**. Two independent markers in the footprint agree: the STEP model at local offset `(11, −7)`
and a Margin-layer rectangle centred at `(10.25, −7.0)`.

Distances measured from the origin are wrong by roughly 12 mm. Corrected, from the die:

| | from die | (from origin — wrong) |
|---|---|---|
| D1 | **17.6 mm** | 5.5 mm |
| D14 | **17.8 mm** | 17.3 mm |
| L1 (boost inductor) | **33.9 mm** | 21.8 mm |
| U1 (TPS61040) | **36.3 mm** | 24.0 mm |

D1 and D14 are **equidistant** — there is no single "nearest LED", and any mitigation aimed at D1 alone is
misdirected.

### 2.2 There is a through-board slot at the sensor

A **4.953 × 6.731 mm** slot is milled clean through the board under the die — 8 `Edge.Cuts` primitives
forming a closed loop with 0.381 mm router fillets, concentric with the die model to 0.02 mm.

So although the copper pours on both faces form an optical wall between the LEDs and the photodiode, that
wall has a deliberate hole exactly where the sensor looks. The slot walls are **bare, router-cut FR4** —
pale and strongly diffusely reflective.

**This is the leading hypothesis for why the badge reports SpO2 = 100%**: a diffuse DC pedestal that
carries no pulsatile component depresses the red/IR ratio into the region where the vendor table is flat.

**Cheap mitigation, no respin:** blacken the routed slot edge with a matte-black paint pen or a black
polyimide collar before the sensor module is fitted. Pennies and about a minute per board.

### 2.3 INT is not connected to the MCU

Net `/MAX30102_INT` reaches only the header pad — no Heltec pin, no test point. (The three test points are
GND, CEREAL_RX and CEREAL_TX.) Interrupt-driven FIFO servicing therefore needs a bodge.

**If bodging, GPIO6 is the only safe target.** Most apparently-free pins are not: GPIO2/7/46 are LoRa FEM
control, GPIO21 is `RESET_OLED`, GPIO19/20 are USB D−/D+, GPIO45/46 are ESP32-S3 strapping pins, and
GPIO26/37 belong to the flash/PSRAM interface.

Whether it is worth doing is a separate question — at 25 Hz the 32-sample FIFO holds 1.28 s, and the
service tick is 200 ms, so polling has ample margin in normal operation.

---

## 3. Power: quieter than it looks

The intuitive "14 × 60 mA = 840 mA of switching transients" is wrong by more than an order of magnitude,
and several earlier conclusions built on it do not survive.

The TPS61040 runs in DCM peak-current PFM from Vext − Vf(D15) ≈ 2.85 V with L1 = 3.3 µH. It leaves DCM at
about **105 mA typical** and roughly **57 mA on a low battery** — that is where regulation is lost, *not*
where the rail fails, and an earlier revision of this document conflated the two. The shipped animation
(`kOutputScale = 0.35`, one colour channel per LED) draws a mean of ~35 mA and peaks near **55 mA**.

Two consequences:

- **Margin is real, but only at the weak-battery corner.** Modelling the datasheet's 400 ns minimum
  off-time (SLVS413L 6.4.1), `+5VL` reaches the WS2812B 3.5 V minimum at ~255 mA on a good battery
  (VBAT 3.7, Ipk 400 mA) and ~107 mA at the worst corner (VBAT 3.0, Ipk 250 mA) — roughly 2× the
  loss-of-regulation figures above. The shipped animation's 55 mA holds 5.07 V and 4.72 V respectively.
  The white preset's ~137 mA holds 5.03 V on a good battery and 4.10 V at VBAT 3.0 with a typical part;
  only the joint corner — a flat battery **and** a low-current-limit part — brings it near 3.0 V.
  So the preset is a weak-battery consideration, not a general overload. `kOutputScale` is
  supply-limited at that corner, not across the board.
- **Conducted LED→PPG coupling is not the problem.** Solving the ground pour as a resistor grid puts
  LED-to-sensor transfer resistance in the low milliohms, giving tens of microvolts of ground bounce
  against a measured cardiac AC of ~11,000 ppm. Every conducted path computes to **≥70 dB below** the
  signal. Do not spend a respin on rail filtering, π filters, star grounds or ferrites — if crosstalk
  exists here it is **optical**.

---

## 4. Open questions that need a person and a badge

1. **Which face does the finger touch?** The sensor is on the back, but a milled slot under the die
   suggests it looks *through* the board. This single fact decides whether LED crosstalk is geometrically
   possible at all. Ten seconds with an assembled badge settles it.
2. **Is the slot present in the fabricated board**, and does the TPU shell cover it?
3. **What is the module's standoff height** from the badge's front soldermask to the sensor's optical
   face? It has never been checked against the enclosure, and the module body is absent from every 3D
   deliverable.

See [bench-runbook.md](bench-runbook.md) for the measurements that resolve the rest.
