// Offline test driver: engine_cli in.f32 out.f32 sampleRate mask retuneMs amount formant minHz
// Reads/writes raw mono float32. Output is latency-compensated (same length as input).
#include "../Source/PitchEngine.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

int main (int argc, char** argv)
{
    if (argc < 9) { std::fprintf (stderr, "usage: in out sr mask retuneMs amount formant minHz [humanize flex vibrato tracking transpose lowLatency]\n"); return 1; }

    const double sr = std::atof (argv[3]);
    snaptune::Settings s;
    s.noteMask = (unsigned) std::strtoul (argv[4], nullptr, 0);
    s.retuneMs = (float) std::atof (argv[5]);
    s.amount   = (float) std::atof (argv[6]);
    s.formant  = (float) std::atof (argv[7]);
    const float minHz = (float) std::atof (argv[8]);
    s.humanize  = argc > 9  ? (float) std::atof (argv[9])  : 0.0f;
    s.flex      = argc > 10 ? (float) std::atof (argv[10]) : 0.0f;
    s.vibrato   = argc > 11 ? (float) std::atof (argv[11]) : 0.0f;
    s.tracking  = argc > 12 ? (float) std::atof (argv[12]) : 0.6f;
    s.transpose = argc > 13 ? (float) std::atof (argv[13]) : 0.0f;
    const bool lowLat = argc > 14 && std::atoi (argv[14]) != 0;

    std::vector<float> in;
    if (FILE* f = std::fopen (argv[1], "rb"))
    {
        float buf[4096]; size_t k;
        while ((k = std::fread (buf, sizeof (float), 4096, f)) > 0) in.insert (in.end(), buf, buf + k);
        std::fclose (f);
    }
    else return 2;

    snaptune::PitchCorrector pc;
    pc.prepare (sr, minHz);
    pc.setMinFreq (minHz, lowLat);
    pc.setSettings (s);
    const int lat = pc.getLatency();

    std::vector<float> out;
    out.reserve (in.size());
    for (size_t i = 0; i < in.size() + (size_t) lat; ++i)
    {
        const float y = pc.process (i < in.size() ? in[i] : 0.0f);
        if ((int) i >= lat) out.push_back (y);
    }

    if (FILE* f = std::fopen (argv[2], "wb")) { std::fwrite (out.data(), sizeof (float), out.size(), f); std::fclose (f); }
    std::printf ("latency %d samples (%.1f ms)\n", lat, 1000.0 * lat / sr);
    return 0;
}
