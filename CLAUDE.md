# mhevcenc

Vulkan Video HEVC (H.265) encoder built on mnexus. Owns the encode
session lifetime (Vulkan video session, video session parameters,
DPB texture, output bitstream buffer, POC counter, DPB slot
allocator) and exposes a simple "feed an uncompressed YCbCr picture
in, get an Annex B access unit out" interface. Companion to
mhevcdec.

First iteration scope: Main profile (8-bit 4:2:0), constant-QP rate
control, fixed `IDR + (gop_size - 1) x P` GOP (no B-frames, single
forward reference per P), single slice / single tile per picture.
Sufficient for transcoding GoPro footage where the source GOP is
already in this shape.

## Language

All code comments MUST be written in English.

## Symbols

It is forbidden to use the full width forms of symbols that have
counterparts in ASCII. e.g. `()`, `:`, `,`, `0-9`.

## Coding style

- `m<short>` library naming.
- `src/<libname>/{public,private}` layout; public include path is
  reached as `#include "mhevcenc/public/<file>.h"`.
- TU header (`#include "<this file>.h"`) listed first in each
  `.cpp`, then C++ system headers, then external headers, then
  public project headers, then private project headers -- each
  group separated by a blank line.
- Slabs of related includes are grouped and labeled with the
  same `// public project headers --...` style markers used in
  the existing files.

## Build

CMake-based C++23 static library. Typically consumed as a
sibling-lib `add_subdirectory(... mhevcenc ...)` from a parent
project's `CMakeLists.txt`. The parent **MUST** add `mbase` and
`mnexus` first, with `MNEXUS_ENABLE_VIDEO_CODING=ON` so mnexus
declares its Vulkan-video API surface (encode and decode). mhevcenc
asserts both `mbase` and `mnexus` targets already exist at configure
time -- it does NOT pull them in, so the parent stays in control of
which version is used.

Target name: `mhevcenc`. Links `mbase` + `mnexus` PUBLIC.

Unlike `mhevcdec`, mhevcenc does NOT depend on `vidsynt`: the
encoder side generates VPS / SPS / PPS through mnexus (which
asks the driver to author them) rather than parsing existing
bitstream NALs.

When adding source files to `CMakeLists.txt`, list them in
alphabetical order within each `set(...)` block.

## Directory structure

```
src/mhevcenc/
  public/   <- API surface
    hevc_encode_session.h
  private/  <- implementation
    hevc_encode_session.cpp
```

## Public API

- `EncoderConfig` -- input parameters for `HevcEncodeSession::Create`.
  Carries the coded picture size, the constant QP, the GOP size
  (IDR cadence), and the H.265 level to encode into the SPS.
- `EncodedFrameData` -- per-picture encode output. Holds the Annex
  B byte stream for one access unit (the slice NAL(s) with leading
  start code; parameter-set NALs are NOT included -- consumers that
  want them inline must prepend `vps_sps_pps_bytes()` themselves,
  e.g. before the first IDR of a raw `.h265` file). Plus the IRAP
  flag, POC, and 1-indexed encode order.
- `HevcEncodeSession` -- owns all the mnexus state needed to encode
  one HEVC stream: the Vulkan video session + parameters (with
  generated VPS / SPS / PPS), the DPB texture, the output bitstream
  ring buffer, the POC counter, the DPB slot allocator. Caller
  passes `mnexus::IDevice*` (dependency-injection -- mhevcenc
  borrows, does not own). Two main entry points:
  - `vps_sps_pps_bytes()` -- the generated parameter-set NAL blob
    (Annex B). Bytes for the muxer to build the MP4 `hvcC` box,
    or for one-shot consumers that prepend them to a raw `.h265`
    stream.
  - `EncodePicture(src_picture, src_array_layer) -> std::optional<EncodedFrameData>`
    -- encodes one picture from the caller-provided input texture
    (`kVideoEncodeSrc` layout, format matching the session's
    profile) and returns the Annex B AU bytes. Returns
    `std::nullopt` on submission failure.

## Threading

- All public methods on `HevcEncodeSession` are expected to be
  called from a single owner thread (typically the consumer's
  main thread, mirroring `HevcDecodeSession`). Internally the
  session submits encode work on the `dedicated_video_encode`
  queue; that queue is not touched by anything else.

## Commit messages

Conventional-Commits-style with the lib as the scope, e.g.
`feat(mhevcenc): ...`, `fix(mhevcenc): ...`, `perf(mhevcenc): ...`,
`docs(mhevcenc): ...`, `refactor(mhevcenc): ...`.
