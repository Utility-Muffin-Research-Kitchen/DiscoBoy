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

Disco Boy is a standalone app, not bundled with Leaf's default payload. Install it
either way:

- **Pak Rat (on-device):** press **Menu**, open **Actions -> Pak Rat**, choose
  **Disco Boy**, and install over Wi-Fi. It shows up in your **Apps** tab when the
  install finishes.
- **Manually:** download `DiscoBoy.pak.zip` from the
  [latest release](https://github.com/Utility-Muffin-Research-Kitchen/DiscoBoy/releases/latest),
  unzip it, copy the `DiscoBoy.pak` folder into `Apps/mlp1/` on your SD card, and run
  **System -> Rescan Library**.

Put music under `Music/` on either SD card (see [Music location](#music-location)).

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
| Stick click | lock the screen (pocket mode); click again to unlock |
| MENU | quit |

**Pocket mode.** Click the analog stick to lock: a padlock flashes, then the screen
powers off and every button is ignored, so the player can ride in a pocket without
skipping tracks or pausing. Music (and auto-advance to the next track) keeps playing.
Click the stick again to wake the screen and unlock.

On the now-playing screen the transport is a full row - skip-prev / rewind /
play-pause / forward / skip-next, with shuffle and repeat below; there L1 / R1 are
previous / next track and L2 / R2 hold to seek. The now-playing screen has no hint
bar of its own: you reach it with Y from the list, so Y returns there, and the
transport is on screen.

SELECT opens a full-screen view of the current cover art with no other chrome -
a "sleeve" view. SELECT again (or B) returns to where you were; playback controls
(X, L1 / R1, L2 / R2, volume) stay live while it's up.

## Music location

On Leaf builds that publish both SD cards' music roots (ordered `$MUSIC_PATHS`, primary
card first), tracks are read recursively from each and merged; an absent secondary card
is skipped without hiding the primary library. On firmware that does not publish those
roots -- or on a direct launch -- Disco Boy falls back to `$MUSIC_PATH`, then
`$SDCARD_PATH/Music`, then `./Music`, and still opens with whatever it finds. Duplicate
roots and malformed colon lists are rejected so the same physical files cannot appear
twice merely because a card was configured twice.

Artists and Albums merge matching metadata across cards while retaining both physical
tracks. A compact `SD1` or `SD2` label appears only when otherwise-identical tracks need
disambiguation. Folders remain source-local: with both cards mounted, open `SD1` or
`SD2` first and then browse that card's real directory tree.

The first launch reads tags + duration from every file (the slow part); the results
are cached in a small binary file under `$USERDATA_PATH/DiscoBoy/`, keyed by path +
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
