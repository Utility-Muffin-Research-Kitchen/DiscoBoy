# Vendored FFmpeg interface (not FFmpeg itself)

Disco Boy decodes the formats the single-header decoders don't cover (M4A/AAC/ALAC,
Opus, WMA, AIFF, ...) through **FFmpeg, which already ships on the device** (the
LoongOS Kodi build, `libav*` 4.4 / avcodec 58). This directory holds only what's
needed to *compile and link* against that runtime copy — no FFmpeg code or binaries:

- `include/` — the FFmpeg 4.4 **public headers** actually reached by `disco_ff.c`
  (the transitive closure: libavformat/avcodec/avutil/swresample), plus a generated
  `libavutil/avconfig.h` for aarch64 little-endian. FFmpeg headers are LGPL-2.1+.
- `stub/` — tiny `libav*.so.N` **link stubs** with the correct SONAMEs, generated to
  satisfy the linker. They contain empty symbols, no implementation; at runtime the
  binary binds to the device's real `libav*`.

The app links FFmpeg dynamically and ships none of it, so Disco Boy stays MIT while
FFmpeg stays under its own license on the device.

Regenerate: extract the four libs' headers from the matching FFmpeg release, recompute
the closure with the cross `gcc -M`, and build the stubs with
`-shared -nostdlib -Wl,-soname,libX.so.N` exporting the symbols `disco_ff.c` uses.
