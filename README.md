# Disco Boy

[![Release](https://img.shields.io/github/v/release/Utility-Muffin-Research-Kitchen/DiscoBoy?label=release&color=7FB069&labelColor=0F160E)](https://github.com/Utility-Muffin-Research-Kitchen/DiscoBoy/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/Utility-Muffin-Research-Kitchen/DiscoBoy/total?color=7FB069&labelColor=0F160E)](https://github.com/Utility-Muffin-Research-Kitchen/DiscoBoy/releases)
[![License: MIT](https://img.shields.io/github/license/Utility-Muffin-Research-Kitchen/DiscoBoy?color=7FB069&labelColor=0F160E)](LICENSE)
![Platform: Miniloong Pocket 1](https://img.shields.io/badge/platform-Miniloong%20Pocket%201-7FB069?labelColor=0F160E)

A music player for [Leaf](https://github.com/Utility-Muffin-Research-Kitchen), the
custom firmware for the Miniloong Pocket 1. Native [Catastrophe](https://github.com/Utility-Muffin-Research-Kitchen)
app, packaged as a Leaf `.pak`. Named after the Frank Zappa track, to match the
Leaf / UMRK naming DNA.

## What it does

Browses your music by **Artists**, **Albums**, or **Folders**, with cover art, a
focused now-playing screen, and a play queue that follows whatever you played from.
The MLP1's mono speaker is weak, so Disco Boy follows Leaf's audio routing and plays
straight to a Bluetooth headset when one is connected.

## Install

Disco Boy is a standalone app, not bundled with Leaf. Grab the latest `.pak` from the
[releases page](https://github.com/Utility-Muffin-Research-Kitchen/DiscoBoy/releases) and
drop `DiscoBoy.pak` into the `Apps/mlp1/` folder on your Leaf SD card; it shows up in your
Apps list. Put music under `Music/` on the card (see [Music location](#music-location)).

Bluetooth audio is best with Wi-Fi off — the MLP1's RTL8723DS Wi-Fi/BT coexistence causes
occasional dropouts, not an app bug.

## Browsing

A tab bar across the top switches between three views (L1 / R1):

- **Artists** -> that artist's albums -> tracks.
- **Albums** -> tracks, with a cover-art header (album / artist / year / total time).
- **Folders** -> the real directory tree, for whatever is on the card.

Every list stays small and navigable at any library size. Left / Right jump by
letter, and the now-playing track pins to the top of the list it belongs to. Play a
track and the **queue** becomes the list you played it from, so next / previous /
shuffle / auto-advance all follow that album or folder.

Cover art is resolved per track: a sidecar `cover.png` / `folder.jpg` (matched
case-insensitively) if present, otherwise the file's own **embedded art** (ID3 APIC,
FLAC PICTURE, MP4 cover), extracted and cached. List thumbnails are downscaled; the
now-playing and album-header art are full resolution.

## Audio

The common formats decode through small vendored single-header decoders under
`third_party/`: **WAV** (dr_wav), **MP3** (dr_mp3), **FLAC** (dr_flac), and **OGG
Vorbis** (stb_vorbis). Everything else - **M4A / AAC / ALAC, Opus, WMA, AIFF, APE,
WavPack, ...** - decodes through **FFmpeg**, which the device already ships (the
LoongOS Kodi build). The app links against tiny SONAME stub libraries and binds to
the device's real `libav*` at runtime, so no FFmpeg binaries are shipped in the pak.
Tags, duration, and embedded art for those formats also come from FFmpeg.

Output is **libasound (ALSA) direct**, not SDL audio: a playback thread streams
decoded S16 frames to a pcm. The device follows Leaf's live `audio_output`:
`BLUETOOTH` opens the BlueALSA `bluealsa` pcm (A2DP straight to the headset), and
anything else opens ALSA `default` (PulseAudio -> speaker). This mirrors how the
RetroArch runner routes game audio, and Disco Boy re-routes live when you plug in a
jack or (dis)connect Bluetooth.

## Controls

| Button | Action |
|---|---|
| Up / Down | move in the list |
| Left / Right | jump by letter |
| A | open (artist / album / folder) or play a track |
| B | back up one level |
| X | play / pause |
| Y | toggle the now-playing screen |
| L1 / R1 | switch tab |
| L2 / R2 | hold to seek (-/+) |
| SELECT | full-screen cover art (press again, or B, to close) |
| MENU | quit |

On the now-playing screen the transport is a full row - skip-prev / rewind /
play-pause / forward / skip-next, with shuffle and repeat below; there L1 / R1 are
previous / next track and L2 / R2 hold to seek. The now-playing screen has no hint
bar of its own: you reach it with Y from the list, so Y returns there, and the
transport is on screen.

SELECT opens a full-screen view of the current cover art with no other chrome -
a "sleeve" view. SELECT again (or B) returns to where you were; playback controls
(X, L1 / R1, L2 / R2, volume) stay live while it's up.

## Music location

Tracks are read (recursively) from `$MUSIC_PATH`, else `$SDCARD_PATH/Music`, else
`./Music`.

The first launch reads tags + duration from every file (the slow part); the results
are cached in a small binary file under `.discoboy/` on the card, keyed by path +
mtime + size. Later launches serve unchanged files straight from the cache (no decoder
open) and only re-read what's new or changed, so startup is near-instant at any library
size. A steady-state launch writes nothing; a read-only card just falls back to a full
scan. Before tags are in, the Artists view falls back to the grandparent folder name
(the `Artist/` dir in an `Artist/Album/track` layout) so it reads sensibly immediately.

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

The FFmpeg fallback uses vendored FFmpeg 4.4 public headers
(`third_party/ffmpeg/include`) plus small SONAME stub libraries
(`third_party/ffmpeg/stub`) generated only to satisfy the linker; the device
provides the real `libav*` at runtime. `scripts/make-record-placeholder.py`
regenerates the vinyl no-art placeholder.

## Credits

Common-format decoding uses the public-domain single-header decoders dr_wav /
dr_mp3 / dr_flac (mackron) and stb_vorbis (Sean Barrett). Other formats decode via
FFmpeg (LGPL), dynamically linked to the copy already on the device. Transport
glyphs are a subset of Material Icons (Apache License 2.0, Google). See
`pak/res/media-icons.ttf`.
