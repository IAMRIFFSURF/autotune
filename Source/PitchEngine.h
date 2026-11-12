// SnapTune pitch-correction engine (header-only, no JUCE dependency).
//
//  * YIN pitch detector (on a ~24 kHz decimated copy of the input, 200 analyses / second)
//  * Note quantiser with scale mask, hysteresis and retune-speed smoothing
//  * Pitch-synchronous overlap-add resynthesis (PSOLA style): grains are copied, not resampled,
//    so formants stay where they are. The optional "formant" control resamples the grains.
//
// Latency is fixed once setMinFreq() has been called: see getLatency().

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace snaptune
{

struct Settings
{
    unsigned noteMask   = 0xFFF;   // allowed pitch classes, absolute (bit 0 = C, bit 1 = C#, ...)
    float    retuneMs   = 0.0f;    // 0 = instant (hard-tune effect)
    float    amount     = 1.0f;    // 0..1, how much of the correction is applied
    float    hysteresis = 0.2f;    // semitones the voice must go past the midpoint before changing note
    float    formant    = 0.0f;    // semitones, -4..+4 (ignored in low-latency mode)
    float    refHz      = 440.0f;
    float    humanize   = 0.0f;    // 0..1  slower retune on held notes, short notes stay tight (Antares "Humanize")
    float    flex       = 0.0f;    // 0..1  leaves pitch alone when it is already close to the note (Antares "Flex-Tune")
    float    vibrato    = 0.0f;    // 0..1  how much of the singer's vibrato survives (0 = flattened)
    float    tracking   = 0.6f;    // 0..1  detector strictness (0 = relaxed, 1 = choosy)
    float    transpose  = 0.0f;    // semitones, -7..+12
};

inline unsigned rotateMask (unsigned relativeMask, int root)
{
    unsigned out = 0;
    for (int i = 0; i < 12; ++i)
        if (relativeMask & (1u << i))
            out |= 1u << ((i + root) % 12);
    return out;
}

class PitchCorrector
{
public:
    static constexpr double kMaxFormantSemis = 4.0;

    void prepare (double sampleRate, float minHz)
    {
        ready = false;
        fs = sampleRate > 1000.0 ? sampleRate : 44100.0;

        // --- detector (decimated) ---
        dec = std::max (1, (int) std::lround (fs / 24000.0));
        fd  = fs / dec;
        tauMaxWorst = (int) std::ceil (fd / 60.0) + 2;
        tauMin      = std::max (2, (int) std::floor (fd / 1000.0));
        W           = (int) std::lround (0.020 * fd);
        hopDec      = std::max (1, (int) std::lround (0.005 * fd));
        hopSeconds  = hopDec * dec / fs;

        dRing.assign ((size_t) nextPow2 (2 * (W + tauMaxWorst) + 16), 0.0f);
        dMask = (int64_t) dRing.size() - 1;
        win.assign ((size_t) (W + tauMaxWorst + 4), 0.0f);
        diff.assign ((size_t) tauMaxWorst + 4, 0.0f);
        cmnd.assign ((size_t) tauMaxWorst + 4, 1.0f);

        // --- resynthesis ---
        const int maxPcap = (int) std::ceil (fs / 60.0);
        const int maxLat  = latencyFor (maxPcap, false);
        ringSize = (int64_t) nextPow2 (maxLat + 6 * maxPcap + 256);
        ringMask = ringSize - 1;
        x.assign ((size_t) ringSize, 0.0f);
        acc.assign ((size_t) ringSize, 0.0f);
        wsum.assign ((size_t) ringSize, 0.0f);

        setMinFreq (minHz);
        reset();
        ready = true;
    }

    // Lowest voice frequency we will track. Sets latency (lower = more latency).
    // lowLatency drops the formant-shift reserve (about -12 % latency) and disables the formant control.
    void setMinFreq (float minHz, bool lowLatency = false)
    {
        minHz = std::max (60.0f, minHz);
        lowLat = lowLatency;
        pCap   = (int) std::ceil (fs / minHz);
        tauMax = std::min (tauMaxWorst, (int) std::ceil (fd / minHz) + 2);
        lat    = latencyFor (pCap, lowLat);
        pDefault = std::min ((double) pCap, fs / 180.0);
    }

    int getLatency() const { return lat; }

    void reset()
    {
        std::fill (x.begin(), x.end(), 0.0f);
        std::fill (acc.begin(), acc.end(), 0.0f);
        std::fill (wsum.begin(), wsum.end(), 0.0f);
        std::fill (dRing.begin(), dRing.end(), 0.0f);
        n = 0;
        dPos = 0;
        dAcc = 0.0f;
        dCount = 0;
        hopCount = 0;
        curP = pDefault;
        tsf = 0.0;
        cPrev = -curP;
        corrS = 0.0;
        vSmooth = 0.0;
        voicedActive = false;
        voicedRun = unvoicedRun = 0;
        histCount = 0;
        curTarget = -1;
        mc = 0.0; lastStable = 0.0; heldHops = 0; suspectRun = 0;
        detMidi = 0.0f;
        tgtMidi = 0.0f;
        lastRaw = false;
    }

    void setSettings (const Settings& s)
    {
        cfg = s;
        cfg.amount     = clampf (cfg.amount, 0.0f, 1.0f);
        cfg.hysteresis = clampf (cfg.hysteresis, 0.0f, 0.5f);
        cfg.formant    = lowLat ? 0.0f : clampf (cfg.formant, (float) -kMaxFormantSemis, (float) kMaxFormantSemis);
        cfg.humanize   = clampf (cfg.humanize, 0.0f, 1.0f);
        cfg.flex       = clampf (cfg.flex, 0.0f, 1.0f);
        cfg.vibrato    = clampf (cfg.vibrato, 0.0f, 1.0f);
        cfg.tracking   = clampf (cfg.tracking, 0.0f, 1.0f);
        cfg.transpose  = clampf (cfg.transpose, -7.0f, 12.0f);
        fStride = std::pow (2.0, (double) cfg.formant / 12.0);
    }

    // One sample in, one sample out (delayed by getLatency()).
    float process (float in)
    {
        if (! ready)                       // host called process before prepare(): pass audio through
            return in;

        x[(size_t) (n & ringMask)] = in;

        dAcc += in;
        if (++dCount == dec)
        {
            dRing[(size_t) (dPos & dMask)] = dAcc / (float) dec;
            ++dPos;
            dAcc = 0.0f;
            dCount = 0;
            if (++hopCount >= hopDec)
            {
                hopCount = 0;
                analyse();
            }
        }

        for (;;)
        {
            const double R = curP * std::max (1.0, fStride) + 3.0;
            if (tsf + R > (double) n)
                break;
            placeGrain (R);
        }

        // Emit the sample that is now complete. Slots with e < 0 are cleared too, because the
        // very first grains spill into negative indices which wrap around the ring.
        const int64_t e = n - lat;
        const size_t idx = (size_t) (e & ringMask);
        const float wet = e >= 0 ? acc[idx] / std::max (wsum[idx], 0.3f) : 0.0f;
        acc[idx] = 0.0f;
        wsum[idx] = 0.0f;
        ++n;
        return wet;
    }

    // for the UI
    float getDetectedMidi() const { return detMidi; }
    float getTargetMidi()   const { return tgtMidi; }
    bool  isVoiced()        const { return voicedActive; }

private:
    // ------------------------------------------------------------------ helpers
    static float clampf (float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

    static int nextPow2 (int v)
    {
        int p = 1;
        while (p < v) p <<= 1;
        return p;
    }

    static int latencyFor (int pcap, bool low)
    {
        const double sMax = low ? 1.0 : std::pow (2.0, kMaxFormantSemis / 12.0);
        return (int) std::ceil (pcap * (1.0 + sMax)) + 24;
    }

    bool allowed (int note) const
    {
        return (cfg.noteMask >> (((note % 12) + 12) % 12)) & 1u;
    }

    int pickTarget (double m)
    {
        if (cfg.noteMask == 0)
            return (int) std::lround (m);

        const int r = (int) std::floor (m + 0.5);
        int best = -1;
        double bestDist = 1e9;
        for (int cand = r - 7; cand <= r + 7; ++cand)
        {
            if (! allowed (cand)) continue;
            const double d = std::fabs (m - cand);
            if (d < bestDist) { bestDist = d; best = cand; }
        }
        if (best < 0)
            return r;

        if (curTarget >= 0 && curTarget != best && allowed (curTarget)
            && std::fabs (m - curTarget) < bestDist + cfg.hysteresis)
            best = curTarget;

        return best;
    }

    // ------------------------------------------------------------------ YIN
    void analyse()
    {
        const int len = W + tauMax;
        for (int i = 0; i < len; ++i)
            win[(size_t) i] = dRing[(size_t) ((dPos - len + i) & dMask)];

        double energy = 0.0;
        for (int i = tauMax; i < len; ++i)
            energy += (double) win[(size_t) i] * win[(size_t) i];
        const double rms = std::sqrt (energy / W);

        bool voiced = false;
        double period = 0.0;

        const float thr = 0.25f - 0.15f * cfg.tracking;
        if (rms > 0.0006 + 0.0016 * cfg.tracking)
        {
            const float* cur = win.data() + tauMax;
            for (int tau = 1; tau < tauMax; ++tau)
            {
                const float* lag = cur - tau;
                float sum = 0.0f;
                for (int j = 0; j < W; ++j)
                {
                    const float d = cur[j] - lag[j];
                    sum += d * d;
                }
                diff[(size_t) tau] = sum;
            }

            cmnd[0] = 1.0f;
            double run = 0.0;
            for (int tau = 1; tau < tauMax; ++tau)
            {
                run += diff[(size_t) tau];
                cmnd[(size_t) tau] = run > 1e-12 ? (float) (diff[(size_t) tau] * tau / run) : 1.0f;
            }

            int best = -1;
            const int last = tauMax - 2;
            for (int tau = tauMin; tau <= last; ++tau)
            {
                if (cmnd[(size_t) tau] < thr)
                {
                    while (tau + 1 <= last && cmnd[(size_t) tau + 1] < cmnd[(size_t) tau]) ++tau;
                    best = tau;
                    break;
                }
            }
            if (best < 0)
            {
                float mn = 1e9f;
                for (int tau = tauMin; tau <= last; ++tau)
                    if (cmnd[(size_t) tau] < mn) { mn = cmnd[(size_t) tau]; best = tau; }
                if (mn > thr + 0.10f) best = -1;
            }

            if (best >= tauMin && best < tauMax - 1)
            {
                const float s0 = cmnd[(size_t) best - 1], s1 = cmnd[(size_t) best], s2 = cmnd[(size_t) best + 1];
                const float den = s0 - 2.0f * s1 + s2;
                double shift = den > 1e-9f ? 0.5 * (s0 - s2) / den : 0.0;
                shift = std::max (-1.0, std::min (1.0, shift));
                period = (best + shift) * dec;
                if (period >= fs / 1000.0 && period <= (double) pCap)
                    voiced = true;
            }
        }

        updateState (voiced, period);
    }

    void updateState (bool voiced, double period)
    {
        bool onset = false;
        double m = 0.0;

        // --- octave-jump guard: a sudden +-12 / +-19 semitone jump must persist before we believe it
        double midi = 0.0;
        if (voiced)
        {
            midi = 69.0 + 12.0 * std::log2 ((fs / period) / cfg.refHz);
            if (voicedActive)
            {
                const double d = std::fabs (midi - lastStable);
                if ((std::fabs (d - 12.0) < 1.0 || std::fabs (d - 19.0) < 1.0) && suspectRun < 5)
                {
                    ++suspectRun;
                    voiced = false;                        // ignore this frame, hold previous state
                }
                else
                    suspectRun = 0;
            }
        }

        if (voiced)
        {
            if (! voicedActive && voicedRun == 0)
                histCount = 0;                       // fresh note: forget old history

            hist[histCount % 3] = midi;
            ++histCount;
            const int cnt = std::min (histCount, 3);
            if (cnt == 3)
            {
                double a = hist[0], b = hist[1], c = hist[2];
                m = std::max (std::min (a, b), std::min (std::max (a, b), c));   // median of 3
            }
            else
                m = midi;

            ++voicedRun;
            unvoicedRun = 0;
            if (! voicedActive && voicedRun >= 2)
            {
                voicedActive = true;
                onset = true;
                curTarget = -1;
                suspectRun = 0;
            }
            lastStable = m;
        }
        else
        {
            ++unvoicedRun;
            voicedRun = 0;
            if (voicedActive && unvoicedRun > 5)
                voicedActive = false;
        }

        if (voicedActive && voiced)
        {
            // slow pitch centre: what the note "is" once vibrato is averaged away
            if (onset || std::fabs (m - mc) > 1.2) mc = m;
            else mc += (1.0 - std::exp (-hopSeconds / 0.12)) * (m - mc);

            const double vib = cfg.vibrato;
            const int previous = curTarget;
            const int target = pickTarget (vib > 0.01 ? mc : m);
            heldHops = (target == previous) ? heldHops + 1 : 0;
            curTarget = target;

            // correction that lands on the note but keeps `vib` of the singer's own movement
            double corrT = ((double) target - mc) - (1.0 - vib) * (m - mc);

            // Flex-Tune: the closer the voice already is to the note, the less we touch it
            if (cfg.flex > 0.001f)
            {
                const double w = 0.5 * cfg.flex;                      // semitones
                const double x = std::min (1.0, std::fabs (corrT) / w);
                corrT *= x * x * (3.0 - 2.0 * x);
            }
            corrT = std::max (-7.0, std::min (12.0, corrT + (double) cfg.transpose));

            // Retune speed, relaxed on held notes by Humanize
            const double heldSec = heldHops * hopSeconds;
            const double speed = cfg.retuneMs + cfg.humanize * 250.0 * std::min (1.0, heldSec / 0.5);
            const double alpha = speed <= 0.5 ? 1.0 : 1.0 - std::exp (-hopSeconds * 1000.0 / speed);

            if (onset) corrS = corrT;
            else       corrS += alpha * (corrT - corrS);

            const double f0 = cfg.refHz * std::pow (2.0, (m - 69.0) / 12.0);
            curP = std::max (fs / 1000.0, std::min ((double) pCap, fs / f0));
            detMidi = (float) m;
            tgtMidi = (float) (m + corrS * cfg.amount);
        }

        vSmooth += ((voicedActive ? 1.0 : 0.0) - vSmooth) * (voicedActive ? 0.6 : 0.4);
        lastRaw = voiced;
    }

    // ------------------------------------------------------------------ synthesis
    float readCubic (double pos) const
    {
        const int64_t i = (int64_t) std::floor (pos);
        const float f = (float) (pos - (double) i);
        const float y0 = x[(size_t) ((i - 1) & ringMask)];
        const float y1 = x[(size_t) ((i)     & ringMask)];
        const float y2 = x[(size_t) ((i + 1) & ringMask)];
        const float y3 = x[(size_t) ((i + 2) & ringMask)];
        const float c0 = y1;
        const float c1 = 0.5f * (y2 - y0);
        const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * f + c2) * f + c1) * f + c0;
    }

    void placeGrain (double R)
    {
        const double P = curP;
        const double s = fStride;
        const double ratio = std::pow (2.0, vSmooth * (double) cfg.amount * corrS / 12.0);
        const double Ps = P / ratio;

        // choose analysis centre: a whole number of periods after the previous one
        double m = std::floor ((tsf - cPrev) / P + 0.5);
        if (m < 0.0) m = 0.0;
        double c = cPrev + m * P;
        while (m > 0.0 && c + R > (double) n)
        {
            m -= 1.0;
            c -= P;
        }
        cPrev = c;

        const int64_t ti = (int64_t) std::floor (tsf);
        const double frac = tsf - (double) ti;
        const int H = (int) std::ceil (P) + 1;
        const double piOverP = 3.14159265358979323846 / P;

        for (int j = -H; j <= H + 1; ++j)
        {
            const double u = (double) j - frac;
            if (std::fabs (u) >= P) continue;
            const float w = 0.5f * (1.0f + (float) std::cos (piOverP * u));
            const float v = readCubic (c + u * s);
            const size_t idx = (size_t) ((ti + j) & ringMask);
            acc[idx] += w * v;
            wsum[idx] += w;
        }
        tsf += Ps;
    }

    // ------------------------------------------------------------------ state
    Settings cfg;
    bool ready = false;
    double fs = 44100.0, fd = 22050.0, hopSeconds = 0.005;
    int dec = 2, tauMin = 20, tauMax = 300, tauMaxWorst = 400, W = 440, hopDec = 110;
    int pCap = 600, lat = 1000;
    bool lowLat = false;
    double pDefault = 300.0;

    std::vector<float> dRing, win, diff, cmnd;
    int64_t dMask = 0, dPos = 0;
    float dAcc = 0.0f;
    int dCount = 0, hopCount = 0;

    std::vector<float> x, acc, wsum;
    int64_t ringSize = 1, ringMask = 0, n = 0;

    double curP = 300.0, tsf = 0.0, cPrev = 0.0;
    double corrS = 0.0, vSmooth = 0.0, fStride = 1.0;
    double mc = 0.0, lastStable = 0.0;
    int heldHops = 0, suspectRun = 0;

    bool voicedActive = false, lastRaw = false;
    int voicedRun = 0, unvoicedRun = 0, histCount = 0, curTarget = -1;
    double hist[3] = { 0, 0, 0 };
    float detMidi = 0.0f, tgtMidi = 0.0f;
};

} // namespace snaptune
