# Pulse-ox bench runbook

Measurements that resolve the remaining open questions, ordered by value per minute. Each says what to do,
what to record, and — stated **before** you run it — what each outcome would mean. Pre-registering the
interpretation is what stops a null result being explained away afterwards.

All of these use log lines the firmware already emits. No special build is required.

## Setup

Connect the badge by USB and capture the console:

```bash
stty -F /dev/ttyACM0 115200 raw -echo -hupcl
timeout 120 cat /dev/ttyACM0 | tr -d '\r' > capture_$(date +%H%M%S).log
```

The lines that matter:

```
MAX30102 presence stats(active): present= mean_ir= mean_red= max_ir= max_red= n=
SpO2 input: mean_ir= mean_red= max_ir= max_red=
HR eval:   hr= valid= stable= hr_out= hold= rho= lag= ac_bpm=
SpO2 eval: spo2= valid= stable= spo2_out= hold= red_pi_ok= ratio_ok= ac_ir= ac_red=
MAX30102 downshift gate: hold= hasWindow= finger= mean_ir= mean_red= anchor= streak=
MAX30102 active epoch anchor: n= mean_ir= anchor=
```

---

## Experiment 1 — the optical pedestal (highest value)

**Question.** Is the badge's red/IR ratio depressed by a DC pedestal that carries no pulse — stray light
reaching the photodiode without passing through tissue? This is the leading explanation for SpO2 railing
at 100%, and it is the difference between "fix the optics" and "the reading is real".

**Why it is now answerable.** `ENABLE_VLED` (GPIO48) de-energises all 14 LEDs in hardware, so the LED
contribution can be switched off and differenced rather than argued about.

**Procedure.** Four arms, ~30 s each, same finger and same pressure throughout:

| Arm | Finger | LEDs |
|---|---|---|
| A | off | powered |
| B | off | de-energised |
| C | on | powered |
| D | on | de-energised |

Record `mean_ir` and `mean_red` from `SpO2 input` in every arm.

**Pre-registered interpretation.**
- `leak_ir = A.mean_ir − B.mean_ir`, `leak_red = A.mean_red − B.mean_red`.
- If `leak_red` is a **meaningful fraction of C.mean_red**, LED light is adding a pedestal to the red
  channel with no pulsatile component — which depresses R exactly as observed, and the fix is optical
  (blacken the slot walls, §2.2 of the hardware notes).
- If both leaks are **near zero**, the LEDs are innocent and R ≈ 0.41 is genuine physiology. In that case
  stop pursuing crosstalk entirely — the reading is real and the resolution ceiling is in the vendor
  curve, which no optical change can lift.
- Compare `D` against `C`: if R moves when only the LEDs change, that is the effect size, directly.

**This is the experiment to run first.** Everything else is refinement.

---

## Experiment 2 — does the dead-band fix work for a weak finger?

**Question.** The active-mode gate used to abort measurement for anyone whose coupling fell between the
finger-detect threshold and a much stricter power-down threshold. Does the epoch-relative replacement
actually rescue those users?

**Procedure.** Three contact styles, 30 s each: firm normal contact; **light touch** (barely resting);
and finger offset to one side of the sensor.

**Record.** `mean_ir` from `SpO2 input`, the `active epoch anchor` line, `streak` from `downshift gate`,
and whether a heart rate ever appears.

**Pre-registered interpretation.** A light touch should now anchor on its own DC and keep measuring.
Previously any session below ~70000 IR DC was force-slept after one evaluation. If a light touch still
dies, capture the `downshift gate` lines — `anchor` and `streak` say which condition fired.

---

## Experiment 3 — the honest-gating cost, on a real finger

**Question.** Replay predicted SpO2 would display in roughly a quarter of on-finger windows. Is that
right, and is heart rate unaffected?

**Procedure.** 60 s of steady, comfortable contact. Do not try to hold unnaturally still — the point is
typical use.

**Record.** From `SpO2 eval`: the fraction of evaluations with `valid=1`, and the `red_pi_ok` / `ratio_ok`
breakdown for the rejections. From `HR eval`: the fraction with `valid=1`, plus `rho`.

**Pre-registered interpretation.**
- Heart rate should be available most of the time, with `rho` comfortably above 0.4.
- SpO2 will be much sparser. **Which gate is rejecting matters more than the rate**: `ratio_ok=0`
  dominating means R is sitting in the table's flat region (an optics problem, Experiment 1);
  `red_pi_ok=0` dominating means the red channel is genuinely weakly modulated.
- If heart rate is *also* sparse, the periodicity threshold is mistuned for real signal — capture and
  report, as that would be a regression.

---

## Experiment 4 — ambient light, properly this time

**Question.** Does room or direct light cause false presence detection on an unshielded sensor?

**Why repeat it.** An earlier attempt was **vacuous**: every presence line printed hard-coded zeros,
because the rate-limited log always fired on the first call after wake, when only one sample existed —
below the minimum needed to compute the statistics. That is fixed, so the log now carries real numbers.

**Procedure.** No finger, sensor facing up, 3 min each: dark room; normal room lighting; bright
direct light (a phone torch at ~10 cm); and if available, direct sunlight.

**Record.** `presence stats(low)` — `mean_ir`, `max_ir`, `present`. Note any `presence detected` line.

**Pre-registered interpretation.** False activations waste battery at roughly 40% presence-scan duty and
would justify a shade or collar. Rising `mean_ir` without activation still matters: it is the ambient
pedestal that competes with the detection threshold, and it tells you how much margin exists before
sunlight trips it.

---

## Experiment 5 — LED brightness against the supply

**Question.** The white preset draws ~137 mA. Modelling says that is fine on a good battery (5.03 V) and
survivable at VBAT 3.0 with a typical part (4.10 V), reaching ~3.0 V only when a flat battery coincides
with a low-current-limit part. Does hardware agree, and specifically does it hold up on a nearly flat
battery?

**Procedure.** Set the LEDs to the white preset. Observe on a full battery and again below ~20%.

**Record.** Whether colours render correctly, whether frames drop, and whether artefacts track the
heartbeat animation rate.

**Pre-registered interpretation.** Colour corruption or dropped frames synchronised with the animation is
supply collapse, not a firmware bug. The model predicts this should **not** happen on a good battery — if
it does, the model is wrong and should be corrected before anything else here is trusted. Seeing it only
on a nearly flat battery confirms the model. **Do not raise `kOutputScale` to compensate** — that makes it
worse.

---

## What to send back

The raw capture files are the deliverable; per-line data matters more than summaries. Note for each
capture: which experiment, contact style, lighting, battery state, and anything felt but not logged
(finger slipping, badge warm, LEDs visibly flickering).
