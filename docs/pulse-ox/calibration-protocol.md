# SpO2 calibration and validation — what each tier would actually license

The badge currently uses the vendor's generic red/IR lookup table. That is fine for a demonstration and
insufficient for any accuracy claim. This document sets out what it would take to say something stronger,
and — just as importantly — what each level of effort does **not** license you to say.

It is written for a volunteer badge project, so it is honest about which tiers are realistic.

---

## The claim ladder

| Claim | Minimum evidence | Realistic here? |
|---|---|---|
| "Experimental, uncalibrated SpO2 estimate. Not a medical device." | None beyond current behaviour | **In use today** |
| "Heart rate accurate to ±X bpm at rest vs an ECG reference" | Tier 2 HR study | **Yes** — genuinely achievable |
| "Detects gross desaturation" | Tier 3 | No |
| "SpO2 accurate to ±X%" | Tier 3 controlled desaturation with arterial reference | No |

The gap between rows 2 and 3 is not effort — it is a different kind of study, discussed below.

---

## Tier 1 — bench, no human subjects

Cheap, immediately actionable, and the only tier that is unambiguously worth doing now. It does not
calibrate anything; it establishes that the measurement chain is sound enough to be worth calibrating.

**1.1 Red-channel starve sweep — the highest-value bench test in this document.**
Progressively attenuate the red emitter (neutral density film, or reduce red LED current) while leaving IR
untouched, and record reported SpO2 at each step.

Expected without the integrity gate: SpO2 stays in the 95–100% range all the way to a *completely dead*
red channel, because the vendor table folds back — degrading the channel makes the number look healthier.
With the gate active, readings should be **refused** rather than reassuring. This converts a code-reading
into a demonstrated failure with a number attached, and it is the single most persuasive artefact for
anyone who doubts the gating work.

**1.2 Optical pedestal.** Experiment 1 of the [bench runbook](bench-runbook.md). Establishes whether stray
light is depressing R.

**1.3 Interference matrix.** Vary LED state (off / static / animating), radio (idle / TX), power
(battery / USB / charging / low battery), and display activity. Record `mean_ir`, `mean_red`, `ac_ir`,
`ac_red`, `rho`.

Temper expectations: conducted LED→PPG coupling computes to ≥70 dB below the cardiac signal, so this is
mostly confirmatory. Its real value is catching anything *unmodelled*.

**1.4 ADC occupancy.** Confirm headroom across users and pressures. A measured badge sits at ~68% of
18-bit full scale with no clipping, which is why 4096 nA is the right range and 2048 would clip.

**1.5 Replay regression.** Field captures become permanent test vectors. The seam exists:
`test/test_ppg_signal_quality/`.

---

## Tier 2 — human comparative study

**What it can establish: heart-rate accuracy. Genuinely.**

Use a chest-strap ECG (e.g. Polar H10) as reference. This is the key asymmetry in the whole document: an
ECG is an **electrically independent** measurement, not another optical estimator, so agreement is
meaningful evidence.

Protocol: 10–20 subjects, 2–3 minutes of steady contact each, resting and after mild exertion for rate
range. Report bias, precision, and limits of agreement (Bland–Altman), not just correlation — correlation
hides systematic offset.

**What it cannot establish: SpO2 accuracy. Not even in principle.**

Two independent reasons, and the second is the one usually missed:

1. A consumer fingertip oximeter is **another optical estimator** with its own uncertainty. Agreement
   between two uncalibrated optical devices is not ground truth; it can just as easily mean they share a
   bias.
2. **Healthy resting subjects only occupy 96–100%.** Even with a perfect reference, a study of healthy
   volunteers samples a range narrower than the measurement error, and can never exercise the part of the
   curve where an oximeter's accuracy actually matters. You would be fitting a line through a single
   cluster.

Tier 2 is still worth running for SpO2 — as a **failure-detection** exercise. If the badge reads 100% while
a reference reads 97%, that is informative. It just does not become a calibration.

**Skin pigmentation.** Optical path length varies with pigmentation, and this is a documented source of
pulse-oximeter bias. A badge study cannot resolve it — the sample size and saturation range are both far
too small. What Tier 2 *can* do is ensure a diverse subject group and report per-subject results rather
than only aggregates, so a large outlier is visible rather than averaged away. Claiming anything stronger
from a dozen healthy volunteers would be overreach.

---

## Tier 3 — controlled desaturation

The only tier that produces a real SpO2 calibration: subjects are brought to controlled reduced
saturation (typically ~70–100%) under medical supervision while arterial blood samples are analysed by
co-oximetry, and coefficients are fitted to the paired data.

**This is out of scope for a badge project**, and not merely because of cost. It requires ethical
approval, medical supervision, arterial sampling, and — critically — a **frozen optical path**. Coefficients
are only valid for the exact geometry they were fitted to: the module, the board cutout, the enclosure, and
how the finger sits. This badge's optical path is not frozen and would need to be before calibration data
meant anything.

There is also a ceiling that no calibration removes: the vendor curve's parabola peaks near a ratio of
0.34, while real badge measurements land near 0.41 — a region where the table returns 100% across a wide
input range. Fitting new coefficients could move the usable region, but the underlying resolution problem
is in the estimator and the optics, not in the numbers on the curve.

---

## Data to log for any of this

Per evaluation, alongside every derived value:

```
timestamp, mean_ir, mean_red, ac_ir, ac_red, min/max per channel,
rho, lag, autocorr_bpm, kernel_hr, kernel_spo2, valid/stable flags,
red_pi_ok, ratio_ok, LED currents, ADC range, profile (presence/active),
die temperature, battery voltage, USB/charge state, radio TX state,
LED animation state, and for human studies the reference instrument's reading
```

Die temperature belongs in this list even though it is never displayed as a vital sign: the red emitter's
wavelength shifts with temperature, so a calibration campaign that does not log it cannot later test
whether R has a thermal dependence. That is the reason it is retained internally.

---

## Recommendation

Do Tier 1. It is inexpensive, it is the only tier that changes what the badge should do today, and 1.1
alone justifies the effort.

Do Tier 2 **for heart rate**, against an ECG strap, if someone wants a real accuracy number to put in the
README. That claim is achievable and defensible.

Do not attempt Tier 3. Instead, keep the honest wording already in the README: an experimental,
uncalibrated estimate, not a medical device.
