#!/usr/bin/env python3
"""Offline checks of the pitch engine with synthetic voices. Run: python3 tests/run_tests.py"""
import os, subprocess, sys, tempfile
import numpy as np

ROOT = os.path.dirname(os.path.abspath(__file__))
SR = 48000
TMP = tempfile.mkdtemp(prefix="snaptune_")
EXE = os.path.join(TMP, "engine_cli")

subprocess.check_call(["g++", "-O2", "-std=c++17", "-o", EXE, os.path.join(ROOT, "engine_cli.cpp")])


def run(sig, mask=0xFFF, retune=0.0, amount=1.0, formant=0.0, minhz=85, hum=0.0, flex=0.0, vib=0.0, track=0.6, transpose=0.0, lowlat=0):
    a, b = os.path.join(TMP, "in.f32"), os.path.join(TMP, "out.f32")
    sig.astype(np.float32).tofile(a)
    subprocess.check_call([EXE, a, b, str(SR), hex(mask), str(retune), str(amount), str(formant), str(minhz),
                           str(hum), str(flex), str(vib), str(track), str(transpose), str(lowlat)],
                          stdout=subprocess.PIPE)
    return np.fromfile(b, dtype=np.float32)


def latency_ms(minhz=85, lowlat=0):
    a, b = os.path.join(TMP, "in.f32"), os.path.join(TMP, "out.f32")
    np.zeros(4800, dtype=np.float32).tofile(a)
    out = subprocess.check_output([EXE, a, b, str(SR), "0xFFF", "0", "1", "0", str(minhz), "0", "0", "0", "0.6", "0", str(lowlat)]).decode()
    return float(out.split("(")[1].split(" ms")[0])


def voice(f0_of_t, dur, formants=((700, 120), (1200, 150), (2600, 200))):
    """Glottal-ish pulse train (sum of harmonics) shaped by formant peaks."""
    t = np.arange(int(dur * SR)) / SR
    f0 = f0_of_t(t) if callable(f0_of_t) else np.full_like(t, f0_of_t)
    phase = 2 * np.pi * np.cumsum(f0) / SR
    y = np.zeros_like(t)
    for k in range(1, 60):
        fk = k * f0
        env = sum(np.exp(-0.5 * ((fk - fc) / bw) ** 2) for fc, bw in formants) + 0.02
        env = env / (1 + 0.0 * k) * (fk < SR / 2 - 500)
        y += env * np.sin(k * phase) / np.sqrt(k)
    return 0.3 * y / np.max(np.abs(y))


def est_f0(y, lo=70, hi=700):
    """Independent f0 estimate with long-window autocorrelation (FFT) + parabolic interpolation."""
    y = y - np.mean(y)
    n = len(y)
    f = np.fft.rfft(y, 2 * n)
    ac = np.fft.irfft(np.abs(f) ** 2)[:n]
    ac /= ac[0] + 1e-12
    a, b = int(SR / hi), int(SR / lo)
    k = a + np.argmax(ac[a:b])
    y0, y1, y2 = ac[k - 1], ac[k], ac[k + 1]
    d = (y0 - y2) / (2 * (y0 - 2 * y1 + y2))
    return SR / (k + d)


def cents(a, b):
    return 1200 * np.log2(a / b)


def centroid(y, lo, hi):
    """Power-weighted spectral centroid: a smooth proxy for where the formant energy sits."""
    w = np.hanning(len(y))
    p = np.abs(np.fft.rfft(y * w, 1 << 16)) ** 2
    fr = np.fft.rfftfreq(1 << 16, 1 / SR)
    m = (fr > lo) & (fr < hi)
    return np.sum(fr[m] * p[m]) / np.sum(p[m])


results = []


def check(name, ok, info=""):
    results.append(ok)
    print(("PASS " if ok else "FAIL ") + name + "  " + info)


A3 = 220.0
seg = slice(int(0.5 * SR), int(1.4 * SR))

# 1. steady note 35 cents sharp -> snaps to A3 (chromatic, instant)
x = voice(A3 * 2 ** (35 / 1200), 1.6)
y = run(x)
e = cents(est_f0(y[seg]), A3)
check("hard-tune: +35 cents snaps to 220 Hz", abs(e) < 4, f"error {e:+.2f} cents (input was +35)")

# 2. 40 cents flat
x = voice(A3 * 2 ** (-40 / 1200), 1.6)
y = run(x)
e = cents(est_f0(y[seg]), A3)
check("hard-tune: -40 cents snaps to 220 Hz", abs(e) < 4, f"error {e:+.2f} cents")

# 3. vibrato (+-50 cents @5.5 Hz) around A3 gets flattened
vib = lambda t: A3 * 2 ** ((50 * np.sin(2 * np.pi * 5.5 * t)) / 1200)
x = voice(vib, 2.0)
y = run(x)
seg2 = slice(int(0.6 * SR), int(1.8 * SR))
chunks = [est_f0(y[seg2][i:i + 4800]) for i in range(0, len(y[seg2]) - 4800, 2400)]
spread_in = np.ptp([est_f0(x[seg2][i:i + 4800]) for i in range(0, len(x[seg2]) - 4800, 2400)])
spread_out = np.ptp(chunks)
check("hard-tune flattens vibrato", cents(A3 + spread_out, A3) < 0.5 * cents(A3 + spread_in, A3),
      f"in {cents(A3 + spread_in, A3):.1f} cents p-p, out {cents(A3 + spread_out, A3):.1f} cents p-p")

# 4. slow retune keeps most of the vibrato (natural mode)
y = run(x, retune=250)
chunks = [est_f0(y[seg2][i:i + 4800]) for i in range(0, len(y[seg2]) - 4800, 2400)]
spread_slow = np.ptp(chunks)
check("slow retune keeps more vibrato than instant", spread_slow > spread_out,
      f"instant {cents(A3 + spread_out, A3):.1f}, slow {cents(A3 + spread_slow, A3):.1f} cents p-p")

# 5. scale: C major only. Input = C#4 (277.18 Hz) -> goes to C4 or D4, never stays at C#
f_in = 277.18
x = voice(f_in, 1.6)
major = sum(1 << i for i in (0, 2, 4, 5, 7, 9, 11))
y = run(x, mask=major)
f_out = est_f0(y[seg])
notes = {261.63: "C4", 293.66: "D4"}
nearest = min(notes, key=lambda f: abs(cents(f_out, f)))
check("C major: C#4 lands on a scale note", abs(cents(f_out, nearest)) < 6, f"output {f_out:.2f} Hz -> {notes[nearest]}")

# 6. amount = 0 leaves pitch and level alone
x = voice(A3 * 2 ** (35 / 1200), 1.6)
y = run(x, amount=0.0)
e = cents(est_f0(y[seg]), est_f0(x[seg]))
lvl = 20 * np.log10(np.sqrt(np.mean(y[seg] ** 2)) / np.sqrt(np.mean(x[seg] ** 2)))
check("amount 0 leaves pitch and level alone", abs(e) < 1.5 and abs(lvl) < 0.5, f"pitch {e:+.2f} cents, level {lvl:+.2f} dB")

rng = np.random.default_rng(1)
nz = 0.1 * rng.standard_normal(SR)
y = run(nz)
snr = 20 * np.log10(np.sqrt(np.mean(nz[2000:-2000] ** 2)) / (np.sqrt(np.mean((y[2000:len(nz) - 2000] - nz[2000:len(nz) - 2000]) ** 2)) + 1e-12))
check("noise / sibilants pass through untouched", snr > 40, f"SNR {snr:.1f} dB")

# 7. formants preserved: pitch up ~4 semitones (A3 -> only C#4 allowed) and the formant peak stays near 700 Hz region
only_cs = 1 << 1                       # C# only
x = voice(A3, 1.6, formants=((800, 300),))
y = run(x, mask=only_cs)
f_out = est_f0(y[seg])
pk_in = centroid(x[seg], 300, 3000)
pk_out = centroid(y[seg], 300, 3000)
check("pitch up 4 semitones", abs(cents(f_out, 277.18)) < 8, f"output {f_out:.2f} Hz (want 277.18)")
check("formants stay put when pitch moves (PSOLA)", abs(cents(pk_out, pk_in)) < 150, f"spectral centroid in {pk_in:.0f} Hz, out {pk_out:.0f} Hz")
y_naive = np.interp(np.arange(0, len(x) - 1, 220 / 277.18), np.arange(len(x)), x)      # plain resampling for reference
pk_naive = centroid(y_naive[seg], 300, 3000)
print(f"     (a plain resampler would move the centroid to {pk_naive:.0f} Hz)")

# 8. formant knob moves the envelope
y2 = run(x, mask=only_cs, formant=3.0)
pk_f = centroid(y2[seg], 300, 3000)
check("formant knob +3 st moves formants up", pk_f > pk_out * 1.1, f"centroid {pk_out:.0f} -> {pk_f:.0f} Hz")

# 9. pitch glide gets stepped through semitones
glide = lambda t: 200 * 2 ** (np.clip(t - 0.3, 0, 1.0) * 2 / 1.0 / 12 * 1.0)
x = voice(glide, 1.6)
y = run(x)
tail = est_f0(y[int(1.3 * SR):int(1.5 * SR)])
semis = 12 * np.log2(tail / 440) + 69
check("glide ends on a semitone", abs(semis - round(semis)) < 0.1, f"final {semis:.2f} MIDI")

# 10. different sample rates
ok = True
for sr_test in (44100, 96000):
    SR = sr_test
    x = voice(A3 * 2 ** (30 / 1200), 1.6)
    y = run(x)
    e = cents(est_f0(y[int(0.5 * SR):int(1.4 * SR)]), A3)
    ok &= abs(e) < 5
    print(f"     {sr_test} Hz: {e:+.2f} cents")
SR = 48000
check("works at 44.1 k and 96 k", ok)


# 11. vibrato control: keep 100% -> output vibrato ~ input vibrato but the centre sits on the note
vib_in = lambda t: A3 * 2 ** ((12 + 30 * np.sin(2 * np.pi * 5.5 * t)) / 1200)   # centre 12 cents sharp, +-30 ct vibrato
x = voice(vib_in, 2.4)
seg3 = slice(int(0.8 * SR), int(2.2 * SR))
def track_cents(sig, hop=1200, win=4800):
    return np.array([cents(est_f0(sig[seg3][i:i + win]), A3) for i in range(0, len(sig[seg3]) - win, hop)])
c_in = track_cents(x)
ranges = {}
for v in (0.0, 0.5, 1.0):
    c = track_cents(run(x, vib=v))
    ranges[v] = (np.mean(c), np.ptp(c))
check("vibrato 100%: centre snapped, movement kept",
      abs(ranges[1.0][0]) < 10 and ranges[1.0][1] > 0.6 * np.ptp(c_in),
      f"input centre {np.mean(c_in):+.0f} ct p-p {np.ptp(c_in):.0f}; out centre {ranges[1.0][0]:+.1f} ct p-p {ranges[1.0][1]:.0f}")
check("vibrato knob is monotonic (0% < 50% < 100%)", ranges[0.0][1] < ranges[0.5][1] < ranges[1.0][1] + 1 and ranges[0.0][1] < 0.5 * ranges[1.0][1],
      " / ".join(f"{k:.0%}: {v[1]:.0f} ct" for k, v in ranges.items()))

# 12. flex-tune leaves near-in-tune pitch alone but still fixes bad notes
x_near = voice(A3 * 2 ** (10 / 1200), 1.6)
x_far = voice(A3 * 2 ** (45 / 1200), 1.6)
e_hard = cents(est_f0(run(x_near)[seg]), A3)
e_flex = cents(est_f0(run(x_near, flex=1.0)[seg]), A3)
far_flex = cents(est_f0(run(x_far, flex=1.0)[seg]), A3)
check("flex-tune: +10 ct left mostly alone", abs(e_hard) < 3 and e_flex > 6, f"without {e_hard:+.1f} ct, with flex {e_flex:+.1f} ct")
check("flex-tune: +45 ct still pulled in", abs(far_flex) < 20, f"{far_flex:+.1f} ct")

# 13. humanize: on a long vibrato note, instant retune flattens it; humanize lets it breathe after a while
x = voice(vib_in, 2.4)
c0 = track_cents(run(x))
c1 = track_cents(run(x, hum=1.0))
check("humanize relaxes held notes", np.ptp(c1) > np.ptp(c0) + 8, f"p-p without {np.ptp(c0):.0f} ct, with {np.ptp(c1):.0f} ct")

# 14. transpose
x = voice(A3, 1.6)
f_up = est_f0(run(x, transpose=12)[seg])
f_dn = est_f0(run(x, transpose=-5)[seg])
check("transpose +12 -> 440 Hz, -5 -> 164.8 Hz", abs(cents(f_up, 440)) < 6 and abs(cents(f_dn, 164.81)) < 8, f"{f_up:.1f} Hz / {f_dn:.1f} Hz")

# 15. low-latency mode
l_norm, l_low = latency_ms(85, 0), latency_ms(85, 1)
x = voice(A3 * 2 ** (30 / 1200), 1.6)
e = cents(est_f0(run(x, lowlat=1)[seg]), A3)
check("low-latency mode is shorter and still in tune", l_low < l_norm * 0.92 and abs(e) < 4, f"{l_norm:.1f} ms -> {l_low:.1f} ms, error {e:+.2f} ct")
l_fast = latency_ms(180, 1)
print(f"     fastest setting (180 Hz+, low-latency): {l_fast:.1f} ms")

# 16. quiet singing and a note change (A3 -> C4) both handled
x = np.concatenate([voice(A3 * 2 ** (25 / 1200), 0.8), voice(261.63 * 2 ** (-30 / 1200), 0.8)])
y = run(0.05 * x)
f1 = est_f0(y[int(0.4 * SR):int(0.75 * SR)]); f2 = est_f0(y[int(1.2 * SR):int(1.55 * SR)])
check("quiet input, two notes", abs(cents(f1, A3)) < 6 and abs(cents(f2, 261.63)) < 6, f"{f1:.1f} Hz, {f2:.1f} Hz")

print(f"\n{sum(results)}/{len(results)} passed")
sys.exit(0 if all(results) else 1)
