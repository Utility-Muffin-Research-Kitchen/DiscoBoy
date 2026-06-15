# Disco Boy

A music player for [Leaf](https://github.com/Utility-Muffin-Research-Kitchen), the
custom firmware for the Miniloong Pocket 1. Native [Catastrophe](https://github.com/Utility-Muffin-Research-Kitchen)
app, packaged as a Leaf `.pak`. Named after the Frank Zappa track, to match the
Leaf / UMRK naming DNA.

## What it does

Browses a music folder, lists the tracks, and plays them with a now-playing bar
(track, status, elapsed / total, auto-advance to the next track). The MLP1's mono
speaker is weak - so Disco Boy follows Leaf's audio routing and plays straight to a
Bluetooth headset when one is connected.

## Audio

Decoding uses single-header decoders vendored under `third_party/`. **WAV works
today** (dr_wav). mp3/flac/ogg are next (dr_mp3 / dr_flac / stb_vorbis behind the
same `disco_audio_*` interface, dispatched by file extension).

Output is **libasound (ALSA) direct**, not SDL audio: a playback thread streams
decoded S16 frames to a pcm. The device is picked from Leaf's `JAWAKA_AUDIO_OUTPUT`
- `BLUETOOTH` opens the BlueALSA `bluealsa` pcm (A2DP straight to the headset), and
anything else opens ALSA `default` (PulseAudio -> speaker). This mirrors how the
RetroArch runner routes game audio, so headphones that work in games work here too.
The device is chosen once at launch, so re-open the app if you connect a headset
while it is already running.

## Controls
| Button | Action |
|---|---|
| Up / Down | move in the track list |
| A | play the highlighted track |
| X | play / pause |
| L1 / R1 | previous / next track |
| B | quit |

## Music location
Tracks are read (recursively) from `$MUSIC_PATH`, else `$SDCARD_PATH/Music`, else
`./Music`. WAV, MP3, FLAC and OGG (Vorbis) decode; `.opus`/`.m4a` are listed but not
yet decoded. Per-folder album art is a sidecar `cover.png` / `folder.jpg`.

## Build
Cross-compiled for the MLP1 in the shared toolchain container, with `DiscoBoy`,
`Catastrophe`, and `Jawaka` as sibling checkouts:

```sh
make package-platform PLATFORM=mlp1
# -> build/mlp1/package/DiscoBoy.pak
```

`make mlp1` just builds the binary (`ports/mlp1/pak/bin/discoboy`); `package-mlp1`
assembles the staged pak from `pak/` + the binary. Leaf wires app packaging through
its own root `make package-platform` / `make stage-app APP=DiscoBoy DEVICE=mlp1`.

## Credits
Decoding uses the public-domain single-header decoders dr_wav / dr_mp3 / dr_flac
(mackron) and stb_vorbis (Sean Barrett). Transport glyphs are a subset of Material
Icons (Apache License 2.0, Google). See `pak/res/media-icons.ttf`.
