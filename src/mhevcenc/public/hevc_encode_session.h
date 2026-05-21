#pragma once

// c++ system headers -----------------------------------
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

// public project headers -------------------------------
#include "mnexus/public/types.h"

namespace mnexus { class IDevice; }

namespace mhevcenc {

/// Configuration for a `HevcEncodeSession`.
///
/// First iteration constraints (the only configuration the current
/// impl supports):
///   - Profile: Main (8-bit 4:2:0).
///   - GOP: `IDR + (gop_size - 1) x P` (no B-frames, no open GOP).
///     The very first picture is always an IDR; from then on the
///     session emits an IDR every `gop_size` `EncodePicture` calls.
///   - Rate control: constant QP (`qp`). No CBR / VBR yet.
///   - Single slice + single tile per picture.
struct EncoderConfig final {
  /// Coded picture dimensions (luma samples). Both must satisfy the
  /// driver's encode-input-picture granularity.
  uint32_t width  = 0;
  uint32_t height = 0;

  /// Constant QP for every picture. Range [0, 51]; lower is higher
  /// quality. Reasonable starting point for transcode-quality
  /// material is 28.
  int8_t   qp = 28;

  /// Number of pictures between IDRs (inclusive of the IDR itself).
  /// 30 matches GoPro's stock GOP structure.
  uint32_t gop_size = 30;

  /// H.265 level encoded into the generated SPS.
  mnexus::VideoH265Level level = mnexus::VideoH265Level::k4_1;
};

/// Per-picture encode output.
struct EncodedFrameData final {
  /// Annex B byte stream for this picture's access unit. Holds the
  /// slice NAL(s) with a leading start code (`0x00 0x00 0x00 0x01`)
  /// and nothing else. VPS / SPS / PPS are NOT included here -- the
  /// MP4 case (which is the primary consumer pattern) stores those
  /// once in the `hvcC` box rather than inline with every IDR.
  /// Callers that need an inline raw `.h265` stream MUST manually
  /// prepend `HevcEncodeSession::vps_sps_pps_bytes()` to the first
  /// IDR (and re-prepend at each IDR if they want random-access
  /// resilience).
  std::vector<uint8_t> au_bytes;

  /// True if this picture is an IRAP / IDR. False for P pictures.
  bool is_irap = false;

  /// PicOrderCntVal of this picture. 0 for IDR; previous + 1 for P.
  int32_t poc = 0;

  /// 1-indexed encode order. Useful for diagnostics.
  uint32_t encode_index = 0;
};

/// Owns all the mnexus state needed to encode one HEVC stream:
/// the Vulkan video session, the encode session parameters (with
/// generated VPS / SPS / PPS), the DPB texture, the output bitstream
/// buffer (large enough to absorb a couple of frames' worth of
/// variable output sizes), the POC counter, and the DPB slot
/// allocator.
class HevcEncodeSession final {
public:
  /// Builds the encode pipeline against `device` from `config`.
  /// Returns `nullptr` on capability check failure (profile / format
  /// / level not supported by the device's encode queue), missing
  /// Vulkan Video encode extensions, or allocation failure.
  static std::unique_ptr<HevcEncodeSession> Create(
    mnexus::IDevice* device, EncoderConfig const& config);

  ~HevcEncodeSession();
  HevcEncodeSession(HevcEncodeSession const&) = delete;
  HevcEncodeSession& operator=(HevcEncodeSession const&) = delete;

  EncoderConfig const& config() const;

  /// Generated VPS + SPS + PPS bytes as a single Annex B blob (each
  /// NAL prefixed with `0x00 0x00 0x00 0x01`). Bytes for the muxer
  /// to build the MP4 `hvcC` box, or to prepend to a raw `.h265`
  /// stream before its first IDR.
  std::vector<uint8_t> const& vps_sps_pps_bytes() const;

  /// Encodes one picture from `src_picture` into a fresh Annex B AU.
  /// Caller's responsibility:
  ///   - `src_picture` MUST have `kVideoEncodeSrc` usage and contain
  ///     uncompressed YCbCr data in the format the session was
  ///     created for (Main 8-bit -> `G8_B8R8_2PLANE_420_UNORM`).
  ///   - The texture's layout MUST be `kVideoEncodeSrc` at the time
  ///     the encode CL submitted by this call begins executing.
  ///   - `src_array_layer` selects which array layer of the texture
  ///     to consume (0 for non-array textures).
  /// Returns `std::nullopt` on submission / feedback-query failure.
  /// On success the returned bytes are the slice NAL(s) only (Annex
  /// B with leading start code); parameter-set NALs live in
  /// `vps_sps_pps_bytes()`.
  std::optional<EncodedFrameData> EncodePicture(
    mnexus::TextureHandle src_picture, uint32_t src_array_layer);

  /// Number of pictures successfully encoded since `Create`. Equal
  /// to the `encode_index` of the most recent successful
  /// `EncodePicture` return.
  uint32_t encoded_picture_count() const;

private:
  HevcEncodeSession();

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace mhevcenc
