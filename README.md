# SnapTune – autotune VST3 pentru FL Studio

Pitch correction în timp real: detecție YIN, cuantizare pe scală, viteză de retune (0 ms = efect hard-tune
T-Pain), formanți păstrați (resinteză PSOLA), control separat de formant, intrare MIDI pentru note.

## Cum îl compilezi (varianta ușoară, fără să instalezi nimic)
1. Creează un repository gol pe GitHub și urcă tot conținutul acestui folder (inclusiv `.github`).
2. Deschide tab-ul **Actions** → workflow-ul *Build SnapTune* rulează singur (3-6 minute).
3. Intră pe rularea terminată → **Artifacts** → descarcă `SnapTune-Windows-VST3`.

## Varianta locală (Windows)
Ai nevoie de Visual Studio 2022 (workload „Desktop development with C++”), CMake și Git:

    cmake -S . -B build -G "Visual Studio 17 2022" -A x64
    cmake --build build --config Release

Rezultatul: `build/SnapTune_artefacts/Release/VST3/SnapTune.vst3`

## Instalare în FL Studio
1. Copiază folderul `SnapTune.vst3` în `C:\Program Files\Common Files\VST3\`
2. FL Studio → Options → Manage plugins → **Find installed plugins**
3. Pune SnapTune ca efect pe un insert din Mixer, pe pista cu vocea.

## Ce controale are (și de ce)
Lista vine din recenziile și manualele Auto-Tune, Waves Tune Real-Time, Graillon, Melodyne, bx_crispytuner.

| Control | Ce face |
|---|---|
| Retune Speed | 0 ms = efect T-Pain; 40-60 ms = natural |
| Humanize | relaxează corecția pe notele lungi, notele scurte rămân strânse (ca la Antares) |
| Flex-Tune | nu atinge ce e deja aproape de notă, dar prinde notele false |
| Keep Vibrato | 0 % = vibrato aplatizat, 100 % = nota se aliniază, dar vibrato-ul rămâne (ca la Waves) |
| Note Hold | histerezis: nota nu sare la vecina până nu treci clar de mijloc (Waves: Transition tolerance) |
| Tracking | cât de strictă e detecția vocii; mai sus = mai puține erori la zgomot |
| Formant | schimbă timbrul separat de pitch; formanții sunt păstrați implicit |
| Transpose | -7 … +12 semitonuri |
| Amount | cât din corecție se aplică |
| Scale, Key, note on/off | notele „greșite” se pot opri una câte una; MIDI notes cântă tu notele țintă |
| Voice Range, Low latency | compromis între voci joase și latență |
| Presets | Hard Tune, Trap Melodic, Modern Pop, Natural Fix, Subtle Polish, Robot |

Sibilantele și respirațiile (sunete fără pitch) trec netratate, iar salturile bruște de octavă ale detecției sunt ignorate.

## Setări rapide
- **T-Pain:** preset „Hard Tune”, setezi Key + Scale corecte (greșeala nr. 1 din recenzii).
- **Natural:** preset „Natural Fix”.
- Înregistrează vocea curată (fără clipping), tunează la redare: FL compensează latența automat.

## Teste DSP
    python3 tests/run_tests.py     (are nevoie de g++ și numpy)

## Licență
JUCE 8 este AGPLv3 sau licență comercială. Pentru uz personal e ok; dacă vinzi pluginul, ai nevoie de licența JUCE.
