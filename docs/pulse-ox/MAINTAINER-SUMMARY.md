# BHV badge pulse-ox — findings and fixes

Issues found in the MAX3010x heart-rate / SpO2 path, with fixes implemented and validated on a real badge.
Written to be short; the detailed analysis behind each point is available if useful.

The badges are already built and in the field, so this is all **firmware** — changes existing owners could
pick up in an update. Happy to send any of it as PRs, individually or together, in whatever order suits you.

---

## 1. Firmware issues fixed

**Measurement was impossible for some wearers.** The active-mode power-down gate used an absolute DC bar
(`meanIr >= 70000`) roughly 30× stricter than the finger-detection threshold that had just decided a finger
was present, and it armed at 3000 ms while the first analysis window can't exist before 4000 ms. Measured
dead band: **IR DC 4000–70000** — finger detected, session force-slept, heart rate never displayed. An
off-centre grip on our badge measured 49,672, inside it. Optical coupling varies by more than an order of
magnitude between people, so the gate is now relative to each session's own starting DC.

**The SpO2 stability gate couldn't withhold a reading.** When a value was in range but failed the stability
test, the code still latched it *and refreshed the hold timestamp*, so the hold was always valid — "stable"
only chose between showing the median and the raw value, never between showing a number and showing none.
Captured on hardware: a finger sliding off the sensor produced *"SpO2 98%, stable"* while the heart-rate
estimator was swinging between 33 and 214 bpm.

**Die temperature was sent as body temperature.** `HealthMetrics.temperature` is documented in the .proto as
"Body temperature in degrees Celsius", and it leaves the badge over LoRa and MQTT into third-party clients
that render it as exactly that. The MAX3010x only has a die-temperature sensor. It's now shown on-screen
explicitly labelled `die:33C` and never transmitted.

**Nothing verified the signal was a heartbeat.** Finger detection accepted an absolute AC threshold *or*
pulsatility, and sensor noise clears any absolute threshold — so a flat DC level plus noise was accepted and
fed to the kernel, which produced heart rates and SpO2 values from it. There's now a periodicity check
(Pearson autocorrelation over physiological lags), which does nearly all the quality work.

**A dead red channel reported a healthy number.** The vendor R→SpO2 table is non-monotonic: it peaks at 100
and returns 95–97 as the ratio approaches zero, so a failed red channel produces a *reassuring* 96–97%
rather than an obvious fault. Replaying with red scaled to zero gave 97% on 44 of 44 evaluations. SpO2 now
requires the red channel to carry its own pulsatile signal.

**Biometrics broadcast by default.** `health_measurement_enabled` defaulted true, and the "show health
telemetry" menu item silently enabled it too — so turning on a local display started transmitting the
wearer's heart rate and SpO2 to the mesh. Now opt-in, with the screen working independently.

**Smaller ones:** empty packets (`heart_bpm=0, spO2=0`) went out whenever nothing was measured; the LED rail
was clocked 1 ms after power-on against a 7–11 ms ramp; and the presence-detection log had never printed
real data (a rate-limiter interaction meant it always fired on the one call per wake where the buffer had
just been cleared — 158 of 158 lines read `mean_ir=0`).

---

## 2. What was already right

Checked against measurements, these hold up and we changed none of them:

- **100 sps + 4× FIFO averaging → exactly 25 Hz**, matching the vendor kernel's hard-coded rate and its
  100-sample window. Deliberate and correct.
- **4096 nA ADC range** — measured peak occupancy 75% with zero clipped samples across every condition
  including firm pressure. 2048 would have clipped badly.
- **Presence thresholds (3200 DC / 4500 peak)** — a badge face-down on a desk, the worst realistic
  false-positive surface, reads 826. Roughly 4× margin, and no false activation in any lighting we tried.
- **411 µs pulse width**, and the **presence→active low-power architecture**, both kept as-is.

---

## 3. Result

On comparable signal quality, measured before and after on the same badge and finger:

| | before | after |
|---|---|---|
| SpO2 shown, good contact | 42% | **93%** |
| Heart rate, good contact | 100% | **100%** |
| Heart rate on deliberately poor contact | shown | withheld |

The behaviour change worth knowing about: the badge now shows `--` rather than a number it can't stand
behind, so a wearer will see nothing more often than before — particularly for SpO2 in poor contact or
bright light. That's intentional.

**One limitation to be honest about:** all of this was validated on one person's fingers, across cold, firm,
light and offset contact. That brackets the edges usefully but doesn't establish that the thresholds
generalise. Multi-person data would be the obvious next step, and the capture tooling to do it is in
`docs/pulse-ox/bench-runbook.md`.

---

## 4. Also included

- Native unit tests for the signal-quality logic (`test/test_ppg_signal_quality/`, 9 cases) — the decision
  logic previously couldn't be tested at all, because the sensor file compiles to an empty translation unit
  in the only host environment.
- A CI workflow that actually runs on this fork. The inherited test jobs are gated on
  `github.repository == 'meshtastic/firmware'`, so no test had ever run here — which is also how two
  broken tests (`test_local_led` didn't compile; `test_bhv_flasher` is a Python test the C++ runner tries
  to build) went unnoticed. Both fixed.

---

## 5. Notes on the hardware, for reference only

Nothing here needs action — the badges work. Recording it in case it's useful for a future revision or for
anyone debugging one.

**The BOM is stale relative to the schematic.** `bhvBadge2026.csv` omits D17 (present in the schematic with
`in_bom yes` and placed on the PCB), and lists R3 as 500 Ω where the PCB and schematic say 220 Ω, with a
datasheet link pointing at a 680 Ω part. `C4` carries no capacitance at all — its Value field holds the
library symbol name `C_Polarized`. Since the LEDs evidently work on shipped badges, D17 was clearly
populated regardless; the file just doesn't describe what was built. Regenerating it from the schematic
would resolve all three.

Worth knowing: D17 sits in series with D1's VDD, and D1 is the head of the WS2812B data chain — an
unpowered WS2812B never regenerates its data output, so if a rebuild ever did follow that CSV, the symptom
would be all fourteen LEDs dark, looking exactly like a GPIO47/RMT fault.

**Two capabilities the firmware doesn't currently use.** The MAX30102 INT line is routed but doesn't reach
the MCU (of the apparently-free pins, only GPIO6 actually is — the others are LoRa FEM control, RESET_OLED,
USB, strapping or flash/PSRAM). And `ENABLE_VLED` on GPIO48 can de-energise all 14 LEDs; the firmware
already defines the pin as `HEARTBEAT_NEOPIXEL_POWER_PIN`. That second one is the clean way to measure
whether the LEDs optically leak into the sensor, if anyone wants to settle it.
