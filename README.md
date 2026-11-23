> **Disclaimer:** this project was made 100 % by Claude (Anthropic's AI). I only brought the ideas to build.

# vinylclean — real-time vinyl cleaning on a Raspberry Pi

*[Version française : README.fr.md](README.fr.md)*

vinylclean removes clicks and crackles from your records **while you listen**, on a Raspberry Pi placed
between the turntable and the amplifier. It is controlled from a web page on your phone or on a touchscreen.

```
Turntable → (phono preamp) → USB sound card → Pi: vc-live → sound card output → amplifier
                                                  ↑ control page: phone, computer or touchscreen
```

**Features**

- **Declick** (large clicks) and **decrackle** (small, dense crackles), sensitivity **0 to 5** with a single slider
  (separate settings available).
- **Music protection** (none / normal / maximum): drum hits, plucked notes and periodic sounds are left alone.
- **Multiband repair**: only the frequency bands where the click dominates are repaired.
- **Linked stereo channels**: a click on one side only is easier to recognize, and long clicks are rebuilt with
  help from the other channel.
- **Built-in phono preamp** (digital RIAA correction within ±0.012 dB) for a turntable connected without a preamp.
- **Recording** of your records (raw or cleaned sound) in **FLAC or WAV**, 16 / 24 bits or 32-bit float, at the
  sample rate of your choice, with download and delete buttons.
- **End-of-side anti-distortion**: enter your tonearm length, alignment (Stevenson, Baerwald, Löfgren B or custom)
  and stylus type; the software computes and removes the playback distortion.
- **Levels**: meters with peak hold and latched clipping, level advice, L/R balance, stereo / mono / phase check,
  background noise between tracks, and the **dynamics of the side** (DR, peak / loudness, loudness) measured on the
  original sound: how "loud" was this record mastered?
- **Two different sound cards** (input on one, output on another): the drift of their clocks is corrected.
- **One core per channel**: the two channels are processed in parallel (same sound, about 1.7× less time per period).
- **Bilingual control page** (English / French), synchronized across all open screens.
- **Desktop version** for Windows (.exe) and Linux (AppImage), with **file cleaning** (WAV / FLAC).

Original work ("clean-room"): no code from any commercial restoration software was used, decompiled or imitated.
Only published methods are used; the sources are listed at the end of this document.

---

## The tools

| Tool | Purpose |
|---|---|
| `vc-live` | Real-time listening + control page on `http://<pi-name>.local:8080`. |
| `vc-file` | Same processing applied to a WAV file. |
| `vc-bench` | CPU load on **your** Pi (sensitivities 0, 1, 3 and 5). |
| `vc-tests`, `vc-tests-crackle`, `vc-tests-loudness`, `vc-tests-rec`, `vc-tests-riaa`, `vc-tests-antidist`, `vc-tests-side`, `vc-tests-drift` | Automatic tests. |

---

## Hardware

- **Raspberry Pi 3B+** or newer (64-bit Raspberry Pi OS recommended). A Pi 4 or 5 has more headroom.
- A **USB sound card** with a stereo line input and output (or an input/output HAT).
- A **phono preamp**, or the built-in digital one (see below) if your cartridge is a moving-magnet (MM) type.

---

## Installation on the Pi

On the Pi (in a terminal or over SSH):

```bash
sudo apt-get update && sudo apt-get install -y git
git clone https://github.com/Daxabeur/vinylclean.git
cd vinylclean
./scripts/install.sh --service
```

The script builds, tests and installs the tools, then starts the cleaning **automatically at every boot**.
At the end it prints the address of the control page.

**Update** to a new version:

```bash
cd ~/vinylclean
git pull
./scripts/install.sh --service
```

Then open **http://\<pi-name\>.local:8080** on your phone (same Wi-Fi network), for example
`http://raspberrypi.local:8080`. If it does not open, use the numeric address printed by the script
(`hostname -I` on the Pi). Tip: "Add to home screen" turns the page into an app icon.

Only one program can use the sound card at a time: stop any other audio software using it first.

Measure the CPU load on your Pi:

```bash
vc-bench --rate 48000 --seconds 10
```

---

## The control page

- **Listening**: Original (no processing) / Removed (what is taken out) / Cleaned.
- **Phono preamp**: External / Built-in (RIAA), phono gain, rumble filter.
- **Sensitivity**: clicks and crackles 0 to 5, music protection.
- **Levels** (foldable): meters with peak hold and latched clipping, level advice, balance, stereo / mono, background
  noise, dynamics of the side (DR, peak / loudness, loudness).
- **Recording**: file name, **raw or cleaned** sound, **FLAC or WAV**, bits, sample rate, record / stop, list of
  recordings with download and delete.
- **Anti-distortion**: turntable and cartridge settings, tracking-error graph.
- **Advanced settings**: separate declick / decrackle sensitivities, fine intensities, maximum click length,
  multiband / full band, linked / separate channels.
- **Sound card and quality**: input, output and sample rate among what the cards really accept, with the
  estimated load. Changing the card or the rate cuts the sound for about 1 second; if the new configuration
  fails, the previous one is restored automatically.
- **EN / FR** button in the top bar (the choice is remembered by the browser).
- **Help** card (at the bottom, foldable): what each setting does. The explanations are no longer under the
  settings, so the cards stay short.
- **⇅ Arrange** button in the top bar: every card folds to its title with **▲ ▼** arrows; move the cards you use most
  to the top. On a computer the page has two columns: ▲ on the first card of the right column moves it to the end of the
  left one (and ▼ the other way). The order is saved by the program: the same on every screen. "Default order" puts
  everything back.

All open screens stay **synchronized**. Settings are saved in `~/.config/vinylclean/settings.json` and
reloaded at startup.

The page is read from disk every time it is opened: you can edit `/usr/local/share/vinylclean/index.html`
and simply reload the page. `sudo cmake --install build --prefix /usr/local` restores the original.

**Quality**: always the best native format of the card (24 or 32 bits), with no conversion and no resampling.
Outside the repaired clicks, the signal comes out bit-identical. Rates: 44.1 / 48 / 88.2 / 96 kHz, and
176.4 / 192 kHz if the card accepts them. On a Pi 3B+, **48 kHz is recommended**.

---

## Settings

### Sensitivity 0 to 5

The **Clicks and crackles** slider sets the declicker and the decrackler together. To set them separately:
**Advanced settings** (the main slider then shows "custom").

| Sensitivity | Meaning |
|---|---|
| **0** | Off. |
| **1** | Very light: only obvious clicks. |
| **2** | Light. |
| **3** | Medium (default). |
| **4** | Strong. |
| **5** | Very strong: also removes tiny ticks, at the risk of touching very dense music (distorted guitars). |

### Music protection

| Protection | Meaning |
|---|---|
| **None** | Also repairs on drum hits and notes; on a sharp attack, only the treble (> 3 kHz) is softened. A perfectly periodic synth sound stays protected. Removes the most large clicks. |
| **Normal** (default) | Raises the threshold on attacks and protects periodic sounds. |
| **Maximum** | Never touches an attack. May let through a click hidden *inside* a drum hit. |

### Advanced settings

- **Declick and decrackle intensity** (0 to 10, in half steps): sensitivities 1 to 5 are precise positions of
  these sliders (declick 2.5 / 4.5 / 6 / 7 / 8; decrackle 2.5 / 4.5 / 6.5 / 7 / 8).
- **Maximum click length** (0.5 to 5 ms, default 2.5 ms).
- **Repair**: Multiband (default) or Full band.
- **Stereo**: Linked channels (default) or Separate channels.

### Levels

The **Levels** card can be folded (its title then shows the advice only).

- **Meters** with zones (green, then orange above −12 dB, red near 0 dB), marks at −12 and −6 dB, and a **peak hold**
  that stays 2 seconds. A clip (0 dBFS) shows "CLIP" and a red message that **stays until you tap it**.
- **Advice** in plain words from the last 20 seconds: good level, input too weak / too loud (sound card gain), or phono
  gain to change when the built-in preamp is used.
- **Record information** (measured on the turntable signal):
  - **Balance L / R** over the last 30 s of music ("centered", or which side is louder; orange above 2 dB: cartridge
    or cable to check);
  - **Stereo / mono**: Mono, Stereo, Wide stereo, or **Phase?** when the channels are opposed (a cable or the cartridge
    wired backwards on one side);
  - **Background noise**: the quietest moment between two tracks (state of the record);
- **Dynamics of the side** — measured on the **original** sound (turntable after the phono preamp, before any
  cleaning), from the stylus drop to now, for the "loudness war" question:
  - **DR**: the published "DR" method (3 s blocks; second highest peak against the RMS of the loudest 20 % of the
    blocks). **DR 14 and more**: very dynamic (green); **8 to 13**: usual; **7 and less**: strongly compressed (red).
  - **Peak / loudness** (PLR): highest peak minus the integrated loudness.
  - **Loudness** (LUFS): integrated loudness ITU-R BS.1770 (silences between tracks are gated out).
  - Duration since the stylus drop. A **new side** starts by itself when the stylus is dropped after 6 s with the arm
    raised: no surface noise in the treble any more (the hum of a raised arm, even loud, is ignored). Also with the
    **New side** button, or with "I am dropping the stylus" (anti-distortion). The label under the duration says "arm
    raised" when it is detected. DR is shown after 12 s of music.
  - Clicks are part of the original sound: they can raise the peaks a little (DR uses the second highest peak to
    limit this). Checked against the DR and BS.1770 meters of FFmpeg on real records (±0.4 dB).

---

## Built-in phono preamp (turntable without a preamp)

A turntable does not output a "normal" sound: when a record is cut, the bass is lowered and the treble raised
(**RIAA** curve). A phono preamp does the opposite. If you do not have one, vinylclean can do it.

**Connection**
- Use the **line input** of the sound card (instrument mode off, no 48 V phantom power — never with a turntable).
- Screw the turntable's ground wire to a metal part (sound card or amplifier). If there is hum, try another point.
- **MM** cartridges (moving magnet, most cartridges, about 3 to 7 mV) work well. **MC** cartridges (moving coil,
  0.2 to 0.5 mV) give too little signal for a line input: they need a preamp or a step-up transformer.

**Setting**
1. Control page → **Phono preamp** → **Built-in**.
2. Raise the **sound card input gain** during a loud passage, without ever clipping. The RIAA correction then
   boosts the bass by almost 20 dB, so the input meters may look low: this is normal.
3. Set the **phono gain** so that the **output** reaches −12 to −6 dB on loud passages. If a "phono gain too high"
   warning appears, lower it.
4. **Rumble filter** (recommended): cuts below 15 Hz the rumble of the motor and of warped records, which the RIAA
   curve boosts (only −0.1 dB at 40 Hz).

**Quality**: official curve (IEC 60098, 3180 / 318 / 75 µs) reproduced within **±0.012 dB** at 44.1 kHz,
±0.004 dB at 48 kHz, less than 0.001 dB at 88.2 kHz and above (20 Hz – 20 kHz), checked by `vc-tests-riaa`.
Switching on/off or changing the gain is faded, with no thump. The correction is applied **before** the
declicker, and recordings contain the corrected sound. A good analog phono preamp remains better for **hiss**
(it amplifies before the conversion).

For a file recorded without a preamp: `vc-file raw.wav corrected.wav --riaa --phono-gain 20`.

---

## Recording your records

On the control page, **Recording** card:
1. Type the **file name** (for example "Artist - Album - Side A").
2. **Record**, then **Stop** at the end of the side.

- **Sound to record**: **Raw** (the turntable sound without declick, with the RIAA correction if the built-in preamp
  is selected) or **Cleaned** (after declick, decrackle and anti-distortion, whatever listening mode you choose). The
  choice is shown next to the timer while recording.
- **File format**: **FLAC** (lossless, compressed) or **WAV** (uncompressed), with the title, date and a comment as tags.
- **Bits**: **Maximum** (default) = FLAC 24 bits or WAV **32-bit float**; or 32-bit float (WAV only), 24 bits, 16 bits.
  - Maximum really is the maximum: the sound card gives 24 bits, and the processing works in 32-bit float. FLAC
    24 bits keeps everything the card gives; WAV 32-bit float keeps every value exactly, even above 0 dBFS (built-in
    phono preamp with a lot of gain). A "32-bit integer" file would only add zeros.
  - 16 bits (CD quality): with TPDF dither, so that quiet passages are not distorted. A value already exact in 16 bits
    (16-bit card, raw sound) is kept as is.
- **Sample rate**: the rates your card accepts. The recording is made at the rate of the card (no conversion): choosing
  another rate changes the card's (about 1 s of silence, not during a recording).
- A WAV file is limited to 4 GB (about 1 h 30 at 96 kHz in 32-bit float): the recording then stops by itself.
- You can **listen and adjust the cleaning** while recording: the recording is not affected.
- Files are stored in `~/recordings` on the Pi. A name already taken gets "(2)", "(3)"… : nothing is ever
  overwritten. With an empty name: "Recording <date> <time>".
- **My recordings**: list with duration and size, **Download** and **Delete** (two taps to confirm).
- Recording stops by itself if the disk is almost full, if the sound card stops, or if the card/rate is changed.

Get all the files at once from a computer:

```bash
scp "<user>@<pi-name>.local:~/recordings/*.{flac,wav}" .
```

---

## End-of-side anti-distortion

Towards the center of the record, the groove runs **2.5 times slower** than at the edge (21 cm/s against
52 cm/s on an LP): the undulations are tighter and the stylus follows them less well. Two causes, both computable:

1. **Stylus radius** (*tracing distortion*): a round stylus of radius ρ rolls on the groove and reads, to first
   order, the velocity `u + (ρ / v²)·u·du/dt` (v: groove speed). The finer the stylus (elliptical, Shibata,
   micro-line), the smaller this is.
2. **Tonearm tracking error** (pivoted arm): the stylus is not exactly tangent to the groove (angle φ). When it
   moves laterally by D, it reads the groove slightly early or late (D·tan φ / v). φ is computed **exactly** from
   the effective arm length and the two null points of the alignment: offset angle sin β = (r1 + r2) / 2L,
   pivot-to-spindle distance d = √(L² − r1·r2), overhang L − d, φ(r) = asin((L² + r² − d²) / 2Lr) − β.

The software recovers the groove velocity (inverse RIAA curve), computes these two terms and removes them at
twice the sample rate (no aliasing), then returns to normal sound.

**Setting** (page → "Anti-distortion" card)
1. **On**.
2. "My turntable and cartridge": **effective arm length** (from the manual), the **alignment** used to set the
   cartridge (Stevenson, Baerwald, Löfgren B, or your two null points), **stylus**, **record** format, **length of a
   side**. The page shows the corresponding overhang, offset angle and pivot-to-spindle distance (to compare with
   your turntable) and the tracking-error curve.
3. **Position**: automatic — press "I am dropping the stylus at the start of the side" (or let the software detect
   the drop: at least 5 s of silence with the arm raised, then music); the radius is estimated from the elapsed
   time. Or manual: enter the radius.
4. **Intensity**: 100 % by default.

**Measured** (`vc-tests-antidist`): the distortion is produced by an **exact physical simulation** (round stylus
rolling on the groove on a grid 32 times finer; exact reading position for the tracking error), independent of
the correction model. Two loud tones, end of side; 2nd and 3rd order distortion products relative to the tone:

| Case | Before | After |
|---|---|---|
| Conical stylus 15.2 µm, 61 mm, 2 × 5 cm/s | −13.6 dB | −24.7 dB |
| Elliptical 0.3 × 0.7, 61 mm, 2 × 5 cm/s | −19.1 dB | −42.9 dB |
| Fine elliptical 0.2 × 0.7 | −22.5 dB | −50.4 dB |
| Shibata | −21.1 dB | −47.9 dB |
| Micro-line | −27.7 dB | −55.5 dB |
| Elliptical 0.3 × 0.7, 65 mm, 2 × 2 cm/s | −27.7 dB | −61.7 dB |
| Tracking error 1.98° (Baerwald, edge), low + high tone | −38.2 dB | −75.9 dB |

**Limitations**
- The **real groove level** (cm/s) is not known to the software: it depends on the cartridge and the gains. It is
  **estimated**: loud passages of a record are cut at about 10 cm/s; the level of the loud passages since the start
  of the side is kept (a quiet passage is still treated as quiet); the correction is active after 20 s of music.
  If the estimate is 2 times too low or 1.5 times too high, the correction only removes about 6 dB; it would take an
  estimate more than 2 times too high to make the distortion worse. Hence the intensity setting.
- Safety: the correction is softly limited (equivalent to 8 cm/s), so a click missed by the declicker or a badly
  estimated level cannot create a pop.
- Stylus radii are usual manufacturer values; a worn stylus is "rounder" than a new one. The real profiles of Shibata
  and micro-line styli are more complex than a circle: the radius given is an approximation.
- Out of reach: **mistracking** (the stylus leaves the groove wall), groove wear, cutting defects.
- The automatic position assumes an evenly cut record from the edge to the center; in manual mode it is exact.
- Cost: about 0.7 % of a PC core (check the load shown on the page on your Pi).

---

## Stereo: both channels help each other

Most vinyl clicks touch **only one channel**, while a musical event is almost always present in both channels
(within ±1 ms, spaced microphones). vinylclean uses this:

- a candidate with **no "twin"** in the other channel gets a slightly lower threshold (never on an attack: an
  instrument panned to one side has no twin either);
- a candidate with a **clear twin** gets a slightly higher threshold (probably musical);
- **long clicks** (≥ 0.6 ms) are rebuilt with a cross predictor learned from the other channel, combined with the
  AR model by weighted least squares;
- on a **mono** record (identical channels), the stereo test turns itself off.

## Multiband repair

The correction of each click is split into 4 bands (< 1 kHz, 1–3, 3–7, > 7 kHz) by zero-phase filters. In each
band a Wiener gain keeps the repair where the click dominates (usually the treble) and the original sound where the
music dominates (usually the bass): the music is not re-invented needlessly.

---

## Touchscreen

Official Raspberry Pi screen (DSI), or an HDMI screen with USB touch. After the installation above:

```bash
cd ~/vinylclean
./scripts/kiosk-install.sh
```

The screen shows the same page full screen, in two columns on an 800 × 480 screen. Lightweight browser "cog"
(WPE WebKit) inside "cage", no desktop; Chromium if cog is not available. The screen runs at low priority: it
cannot disturb the sound, which keeps its real-time priority.

- Remove the screen: `./scripts/kiosk-install.sh --remove` (the sound keeps running).
- Upside-down picture (official screen): add `display_lcd_rotate=2` or, with the KMS driver,
  `video=DSI-1:800x480@60,rotate=180` at the end of `/boot/firmware/cmdline.txt`, then reboot.

---

## Keyboard (if you run `vc-live` by hand over SSH)

Stop the service first: `sudo systemctl stop vinylclean-live`, then `vc-live`.

| Key | Effect |
|---|---|
| `3` / `1` / `2` | Cleaned / Original / Removed |
| `+` / `-` | sensitivity (declick and decrackle) |
| `c` / `v` | decrackle only |
| `p` | protection (none → normal → maximum) |
| `[` / `]` | maximum click length |
| `m` | multiband / full band |
| `s` | linked / separate channels |
| `r` | built-in phono preamp (RIAA) on / off |
| `d` | anti-distortion on / off |
| `n` | start of side (stylus dropped) |
| `q` | quit |

---

## Processing a file

```bash
arecord -D hw:0,0 -f S32_LE -r 48000 -c 2 -d 180 side.wav
vc-file side.wav side_s3.wav --sens 3
vc-file side.wav side_removed.wav --sens 3 --mode removed    # what is taken out
vc-file side.wav side_ad.wav --antidist on --arm-length 222 --align stevenson --stylus elliptical   # whole side, from its start
```

The output is aligned with the input (latency compensated): both files can be compared in an audio editor.
`vc-file --help` lists all the options.

---

## Desktop version (Windows and Linux)

The same cleaning on a computer, with the same control page in its own window, plus a **Clean a file** card.

**Download** (Releases page of the GitHub repository):
- **Windows**: `vinylclean-<version>-windows-x64.exe` — a single portable file, nothing to install. Double-click it.
  Windows SmartScreen may warn about an unknown publisher (the program is not signed): "More info" → "Run anyway".
- **Linux**: `vinylclean-<version>-x86_64.AppImage` — make it executable (`chmod +x vinylclean-*.AppImage`) and run it.
  Works on distributions with glibc 2.27 or newer (Ubuntu 18.04+, Debian 10+, Fedora…).

**Real time**: in "Sound card and quality", choose the input (your sound card) and the output (speakers or headphones).
Everything else works like on the Pi: sensitivity, protection, built-in phono preamp, anti-distortion, dynamics of the
side, card order, FLAC / WAV recording (saved in `Music/vinylclean`). Latency is about 60 to 100 ms. Two different
cards for the input and the output: the clock drift is not corrected on the desktop version (small dropouts possible);
use the same card for both when you can.

**Clean a file**: choose a WAV or FLAC file, the output format (WAV or FLAC), then **Clean**. The current settings are
used (sensitivity, protection, phono preamp, anti-distortion; the file is treated as one side from its start). When it is
done, listen to **Original / Cleaned** at the same position, then **Download**. A 20-minute side takes about a minute.
Everything stays on your computer.

The page opens in an app window (Edge on Windows, Chromium / Chrome on Linux), or in your default browser. Closing the
window quits the program (unless a recording is in progress). Options: `--browser` (default browser), `--lan` (open the
page from a phone on the same network), `--clean IN OUT` (clean a file without the window).

The desktop program uses exactly the same DSP code (`core/`); a WAV cleaned on Windows is bit-identical to the same file
cleaned by `vc-file` on a PC. Sound input/output: [miniaudio](https://miniaud.io) (WASAPI on Windows, PulseAudio /
PipeWire / ALSA on Linux). Source code in `desktop/`; `desktop/build.sh` builds both files from Linux. The Raspberry Pi
version is not affected.

---

## Troubleshooting

| Problem | Solution |
|---|---|
| The page does not open | `systemctl status vinylclean-live`; try the numeric address (`hostname -I`) |
| I still hear clicks | check "Cleaned" on the page; protection "None", then raise the sensitivity (4 or 5) |
| "The input is clipping" | lower the input gain of the sound card |
| `Device or resource busy` | another program is using the sound card: stop it |
| "dropouts" keep increasing | go back to 48 kHz; check `vcgencmd get_throttled` (0x0) and the power supply |
| Strange behaviour with the two-core processing | add `--threads 1` to `VC_OPTS` in `/etc/default/vinylclean`, then `sudo systemctl restart vinylclean-live` (everything on one core, same sound) |
| Notes are damaged | lower the sensitivity; protection "Maximum" |
| Recording stops by itself | read the message in the Recording card (disk full, rate change…) |
| Very bright, thin sound, no bass | the turntable is connected directly: Phono preamp → **Built-in** |
| Dull sound, huge bass | you have a phono preamp: Phono preamp → **External** |
| Service log | `journalctl -u vinylclean-live -n 50` |

---

## How it works (summary)

**Declick**: autoregressive (AR) model of order 24 of the music, re-estimated every 10.7 ms. Two detectors (the
matched filter, and the forward + backward error), plus a band pre-detector that keeps large clicks out of the model
estimation. Local dynamic threshold, isolation test, protection of attacks and periodic sounds (3 levels), two
passes, then **LSAR** interpolation (higher order for long clicks).

**Decrackle**: the same engine with a "crackle" profile: repairs ≤ 0.5 ms, soft Wiener-type repair (only the part
that exceeds the expected interpolation error is removed), capped amplitude.

**Anti-distortion**: groove velocity recovered through the exact inverse RIAA curve; ×2 oversampling (Kaiser-window
half-band filter, with a second filter giving the exact, perfectly aligned derivative); tracing and tracking-error
terms computed and removed (first-order inverse); back to the base rate then RIAA curve.

**Real time**: everything is allocated at startup. In the audio thread: no allocation, no lock, no system call
other than ALSA. Denormals flushed to zero, fixed latency identical in the 3 listening modes, 10 ms crossfade on mode
changes. The web page runs in a low-priority thread and passes its settings to the audio thread through atomic
variables.

---

## Project structure

```
core/       DSP: ar, lsar, declick, decrackle (+ chain), antidist (anti-distortion), riaa.h, presets.h
tools/      vc_live.cpp, web_server.*, sha1.h, loudness.h, sidemeter.h, drift.h, levels.h, recorder.h, vc_file.cpp, vc_bench.cpp, wav.h, offline.h
web/        index.html (control page, English / French)
desktop/    Windows / Linux version (vc_desktop.cpp, http_server.*, extra.html, build.sh, packaging/)
tests/      test_declick.cpp, test_decrackle.cpp, test_loudness.cpp, test_recorder.cpp, test_riaa.cpp, test_antidist.cpp, test_side.cpp, test_drift.cpp, page_test_server.cpp, synth.h
scripts/    install.sh, kiosk-install.sh, systemd services, default options
cmake/      cross-compilation (PC → Pi), optional
```

Build on any Linux PC (ALSA and libFLAC development packages needed for `vc-live`):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cd build && ctest
```

## Sources

- S. J. Godsill, P. J. W. Rayner, *Digital Audio Restoration*, Springer, 1998.
- S. V. Vaseghi, P. J. W. Rayner, impulse detection with an AR model, matched filter and LSAR, IEE Proc., 1990.
- A. J. E. M. Janssen, R. N. J. Veldhuis, L. B. Vries, "Adaptive interpolation of discrete-time signals that can be modeled as autoregressive processes", IEEE TASSP, 1986.
- A. Özdoğru, "Comparison of Autoregressive Model Based Perceptible Click Detection Algorithms", POSTER 2022, CTU Prague.
- F. Rund, V. Vencovský, M. Semanský, "An Evaluation of Click Detection Algorithms Against the Results of Listening Tests", JAES 69(7/8), 2021.
- A. Özdoğru, F. Rund, K. Fliegel, "Performance evaluation of perceptible impulsive noise detection methods based on auditory models", EURASIP JASMP, 2025.
- P. Rajmic, J. Klimek, "Removing crackle from an LP record via wavelet analysis", DAFx 2004 (approach tested, not kept).
- J. P. Bello et al., "A Tutorial on Onset Detection in Music Signals", IEEE TSAP 13(5), 2005.
- N. Wiener, optimal filtering; per-band Wiener-type spectral gain.
- R. Bristow-Johnson, "Audio EQ Cookbook" (pre-detector and multiband filters).
- IEC 60098 (RIAA playback curve: 3180, 318 and 75 µs; groove radii 146.05 and 60.325 mm); R. Storn, K. Price, "Differential Evolution", J. Global Optim. 11, 1997 (coefficient optimization).
- E. Löfgren, "On the non-linear distortion in the reproduction of phonograph records caused by angular deviation of the pickup arm", Akustische Zeitschrift, 1938.
- H. G. Baerwald, "Analytic treatment of tracking error and notes on optimal pick-up design", J. SMPE 37, 1941.
- J. K. Stevenson, "Pickup arm design", Wireless World, May 1966.
- M. S. Corrington, "Tracing distortion in phonograph records", RCA Review 12, 1951; J. G. Woodward, E. C. Fox, "Theory of tracing distortion", RCA Review, 1947.
- J. F. Kaiser, Kaiser window (oversampling half-band filter).
- ITU-R BS.1770-4 (K-weighting, integrated loudness of the side); EBU Tech 3341.
- Pleasurize Music Foundation, "DR" dynamic range measurement (published method; FFmpeg `drmeter` used to check).
- J. O. Smith, *Digital Audio Resampling* (bandlimited interpolation, used for the clock drift correction).
- P. Esquef, M. Karjalainen, V. Välimäki, "Detection of clicks in audio signals using warped linear prediction", 2002
  (tried: `vc-file --warp`; not better than the classic prediction on the test benches, so off by default).
- Xiph.Org, libFLAC (FLAC format, RFC 9639).
- D. Reid, miniaudio (desktop audio input/output, public domain / MIT-0).
- I. Fette, A. Melnikov, RFC 6455 "The WebSocket Protocol", 2011; FIPS 180-1 (SHA-1).
- ARM, FPCR register (AArch64): FZ bit for denormals.

## License

MIT — see `LICENSE`.
