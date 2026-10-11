# The DAC test: what the Seed's output does at and above full scale

A step-by-step guide for the owner's Daisy Seed Rev7 and an audio interface. It answers the two
questions the output limiter needs settled before its sound revision is minted
([output-limiter.md](../docs/design/output-limiter.md) §6.4, lane L1b, decision **D10**):

1. **The 0 dBFS output level:** how loud the Seed's output is when the samples reach full scale.
2. **Inter-sample peaks:** whether the PCM3060's DAC reproduces a waveform whose samples stay at
   full scale but whose reconstructed peaks go **3.01 dB above it**, or clips it. If it reproduces
   it, the limiter's ceiling stays at C = 1.0. If it clips it, the ceiling must come down, or the
   detector must be oversampled, before revision 10.

You play one 59.5-second test program from the live image's console, record it on two inputs of
your interface, and run one script on the file. The script prints the verdict. About 20 minutes,
most of it wiring.

## What you need

- The Seed Rev7 running **`brainscape_live.bin` built from this branch** (it has the `tone`
  command; the CI run's `firmware-images` artifact, or built as in [README §7](README.md#7-building)),
  flashed as in [README §1](README.md#1-what-to-flash-in-order).
- An audio interface with **two line inputs** (instrument inputs work if they take +10 dBu or more
  at their lowest gain), and a DAW or any recorder that writes WAV.
- Two mono (TS) jack cables, or a breakout with two jacks, and jumper wires to the Seed's header,
  as for the live-audio wiring in [README §5](README.md#5-image-c-live-audio).
- Python 3 and this repository on the computer.

## 1. Wire the outputs to the interface

**Everything unpowered first:** the Seed's USB cable unplugged, the interface's input gains at
minimum, its **phantom power (48 V) off**, and speakers and headphones turned down.

| Seed pin | Signal | Goes to |
| --- | --- | --- |
| 18 | Audio Out L (`out[0]`) | the **tip** of interface input **1** |
| 19 | Audio Out R (`out[1]`) | the **tip** of interface input **2** |
| 20 | AGND | the **sleeve** of both inputs |
| 40 | DGND | **tied to pin 20** at the Seed, as for every use of the board |
| 16, 17 | Audio In L, R | nothing: leave the inputs unconnected |

- **The pins.** Pin numbers are the Seed's 40-pin numbering, the same on Rev4, Rev5 and Rev7.
  Confirmed against libDaisy's own pin table at the pinned v9.0.0
  (`doc/Daisy_Seed_Rev4_Pinout.csv`: 16 AUDIO IN L, 17 AUDIO IN R, 18 AUDIO OUT L, 19 AUDIO OUT R,
  20 AGND, 40 GND), the live-audio wiring in [README §5](README.md#5-image-c-live-audio), and the
  Rev7 schematic as read in
  [daisy-pedal-platforms.md](../docs/research/daisy-pedal-platforms.md) §1.3–1.4. This guide gives
  numbers only: find where each one sits on Electrosmith's pinout card or the board's silkscreen
  before connecting.
- **The circuit.** On the Rev7 each output is the codec's output through a 4.7 µF capacitor, with
  47 kΩ to AGND, then 100 Ω to the pin: AC-coupled, line level, 100 Ω source. Any line or
  instrument input (10 kΩ or more) is a safe load. Do not short an output to ground, and do not
  plug headphones into it.
- **Unbalanced into a balanced input.** Use **TS (mono) plugs**: the sleeve then grounds the
  input's cold leg, the correct way to feed an unbalanced source into a TRS input. With TRS plugs,
  join ring and sleeve at the Seed end. Never use an XLR input: phantom power lives there.
- **Ground.** AGND to the sleeves, and AGND tied to DGND (pin 40) at the board: libDaisy's guide
  requires the two to be joined outside the Seed. Powering the Seed from the same computer as the
  interface is fine. If the report shows hum (a noise floor above about -80 dBFS), you can run the
  laptop on battery or try another USB port, but hum does not change the verdict: the script
  measures every tone at its own frequency, and decides whether an input carries the program from
  the 1 kHz sine alone.
- **Levels.** The Seed's full scale is about 3.6 Vpp, 1.27 Vrms, +4.3 dBu by TI's formula for the
  PCM3060 (Electrosmith states 1 Vrms; this test measures it). The inter-sample test asks for
  3.01 dB more, up to about +7.3 dBu: a line input at low gain takes that easily. Some instrument
  inputs clip near +8 dBu; the script detects that and says so.

## 2. Set up the interface

- **Sample rate: 96 kHz** if the interface offers it, otherwise **48 kHz** (44.1 kHz works too).
  The Seed always plays at 48 kHz; recording at 96 kHz also captures the 12 kHz test tone's third
  harmonic, at 36 kHz, which the report prints as extra evidence.
- **Bit depth: 24-bit**, or 32-bit float.
- **Inputs 1 and 2 as line inputs** (or instrument, see above), **no pad switching** during the
  session, phantom power off.
- **Processing off:** no compressor, limiter, "Air", high-pass filter, noise suppression or
  auto-gain on the inputs; in the DAW, no plug-ins on the record tracks and no normalising on
  export.
- **Monitoring down:** mute direct monitoring, or keep speakers and headphones low. The test plays
  a full-scale 12 kHz tone and a full-scale square for two seconds each: piercing at monitoring
  level.
- Record **one stereo track (input 1 left, input 2 right)**, or two mono tracks exported as one
  stereo file.

## 3. Check the wiring and set the gain

Plug the Seed's USB in (the live image starts; its LED pulses), then open the console:

```text
python tools/hil/console.py --port auto
info
```

Keep the `info` line: it names the image and its commit. Then:

| Type | What you should see |
| --- | --- |
| `tone sine -20` | A 1 kHz tone on **both** input meters, about 20 dB below where it will be at full scale. One meter silent: check that input's tip and sleeve |
| `tone sine 0` | The 1 kHz tone with sample peaks at full scale. **Set both input gains so the meters peak about -12 dBFS** (anywhere from -20 to -6 is fine), and the same on both if the interface lets you |
| `tone isp 0` | The 12 kHz inter-sample test. The meters read up to 3 dB higher than for `tone sine 0` (less if the DAC clips it). **No clip light** |
| `tone square 0` | The full-scale square, about 2 dB above `tone sine 0`. **No clip light**. If one lights, lower the gain 6 dB and repeat from `tone sine 0` |
| `tone silence` | The meters fall to the noise floor |

Do not touch the gains after this. Every `tone` command replies with what it plays, for example
`tone: isp, 12000 Hz, sample peak 0.00 dBFS (code 8388608, codec 8388482), phase 45 degrees, true
peak +3.01 dBTP`. While a tone plays the engine is stopped, and `preset`, `set` and the other
commands that would change it are refused.

## 4. Record the test

1. **Start recording** in the DAW. Let it run 2 seconds.
2. Type **`tone test`**. The reply reads `tone: the DAC test program v1, 88 steps, 59.5 s
   (2856000 frames), then silence`.
3. **Wait a full minute without typing**: 1 s of silence; the 1 kHz sine at -20, -6, -1 and
   0 dBFS; the 12 kHz tone at -12 dBFS, its 0° control at full scale, and the 45° tone with true
   peaks at 0 and +3.01 dBTP; the square; 2 s of silence for the noise floor (each tone 2 s, with
   half a second of silence between); then a slow 1 kHz level sweep from -40 to 0 dBFS (20.5 s)
   and a 12 kHz sweep from -12 to 0 dBFS (12.5 s). Then it stays silent.
4. **Stop recording** once the 12 kHz sweep has ended and 2 more seconds of silence have gone by:
   at least 62 seconds after you typed `tone test`, so about 70 seconds in all is safe. A longer
   take does no harm; one that stops before the program's end is refused.
5. Type **`tone off`**: the engine plays again.
6. **Export the take as WAV**, 24-bit or 32-bit float, at the rate you recorded, without
   normalising or dithering to 16 bits.

The order of the program is fixed in the firmware (`firmware/live/Tone.h`, version 1); the
script finds it in the file by its silences, so what was recorded before and after does not
matter.

## 5. Analyse it

```text
python tools/hil/dac_test.py dac-test.wav --json dac-test.json
```

If the interface's manual gives the input's level at 0 dBFS for the gain you used (for example
"+20 dBu maximum input level" at minimum gain), add `--interface-fs-dbu 20` and the report gives
the Seed's full scale in dBu, Vrms and Vpp too. `--steps` also prints every step of both sweeps.

The report ends like this:

```text
VERDICT
  recording: OK (channels with the program: 1, 2)
  D10 0 dBFS output level: MEASURED (ch1 -12.03 dBFS peak; ch2 -12.05 dBFS peak)
  D10 inter-sample peaks: PASS
  analog chain at full scale: PASS
RESULT: PASS
```

- **PASS:** the fs/4 sine at 45° with sample peaks at 1.0 (true peak +3.01 dBTP) comes out within
  0.05 dB of the same tone at -12 dBFS, so the DAC reproduces inter-sample peaks of +3 dB and the
  Seed's output swings full scale + 3 dB. D10: the ceiling stays at C = 1.0.
- **FAIL on inter-sample peaks:** the DAC, or the output after it, clips them. The report gives the
  compression, how far above full scale the 12 kHz sweep stayed clean, and the ceiling that would
  keep an fs/4 peak inside that. D10: you choose a lower ceiling or an oversampled detector
  before revision 10. Not a fault of the board: Benchmark Media reports that every D/A chip it
  tested clips them (output-limiter.md §6.4).
- **FAIL on the analog chain:** the 1 kHz sine compresses or distorts at full scale. Record again
  with the interface's gain 6 dB lower: a failure that follows the gain is the interface's, one
  that stays is the Seed's.
- **INCONCLUSIVE:** the 12 kHz path compresses even without an inter-sample peak; see the analog
  chain line, then record again.
- **INVALID** (exit code 2): the recording reached the interface's own full scale, or no input
  carries the program. The report says how many dB to lower the gain, or, for an input whose
  1 kHz sine does not stand 60 dB clear of the silence at 1 kHz, `no usable test signal`: check
  that input's wiring.

## 6. What to send back

- The **WAV file** as exported (a 70-second take in 24-bit is about 20 MB at 48 kHz, 40 MB at
  96 kHz; zipping is fine).
- The **report** dac_test.py printed and the **`dac-test.json`** it wrote.
- From the console: the **`info` line** and the **`tone test` reply**.
- The **interface's make and model**, the inputs used (line or instrument), the gain setting, the
  sample rate, and its full-scale input level in dBu if the manual gives it.
- If anything looked odd (a clip light, hum, one channel quiet), a sentence on it, and a photo of
  the wiring if you have one.

These become the record under [`records/`](records/) that D10 is decided from.

## Other ways to use the tone command

| Command | Plays (both outputs, until `tone off` or another `tone`) |
| --- | --- |
| `tone sine DB` | 1 kHz sine, sample peaks at DB dBFS (-120 to 0, up to two decimals; 0 puts the crest at 1.0) |
| `tone isp DB` | fs/4 (12 kHz) at 45°: samples +P, +P, -P, -P at DB; the reconstructed peak is DB + 3.01 dBTP |
| `tone fs4 DB` | fs/4 at 0°: 0, +P, 0, -P, whose true peak is its sample peak: `isp`'s control |
| `tone square DB` | 1 kHz square, 24 samples at +P and 24 at -P; at 0 dBFS its reconstruction overshoots to +2.17 dBTP |
| `tone sweep` / `tone sweep isp` | The program's 1 kHz sweep (-40 to 0 dBFS in 1 dB steps) or 12 kHz sweep (sample peaks -12 to 0 dBFS in 0.5 dB steps), 0.5 s a step, then 1 s of silence, repeating: slow enough to watch a meter |
| `tone silence` | Digital zeros |
| `tone test` | The program above, once, then silence |
| `tone` | What plays, how long it has played, and the usage |
| `tone off` | The engine's output again |

**With an oscilloscope instead of an interface:** `tone sine 0` on pin 18 (ground on pin 20) shows
the 0 dBFS level directly as Vpp; `tone isp 0` shows a 12 kHz sine whose peaks should be 1.41
times as high, and flat tops if the DAC clips them; `tone fs4 0` is the same frequency at 1.0. The
script needs a recording, so the scope gives the level and a look, not the verdict.

**Exactly what reaches the codec.** Each sample is an integer code q on the codec's 24-bit grid,
handed to libDaisy as the exact float q · 2⁻²³ ([`live/Tone.h`](live/Tone.h)). libDaisy's `f2s24`
then sends the codec q itself, except that full scale, 2²³, arrives as 8,388,482, which is what the
engine's own 1.0 gives (output-limiter.md §2.2). The tone code runs from QSPI, outside the engine
and ITCM, and calls none of the engine's code: it is not a sound revision, the engine's archive is
the same with or without it, and the live image's ITCM holds the same functions at the same
addresses (only the veneers through which ITCM calls the tempo core's code in QSPI point to where
that code has moved).
