#include "mhevcenc/public/hevc_encode_session.h"

// c++ system headers -----------------------------------
#include <cstring>
#include <utility>
#include <vector>

// public project headers -------------------------------
#include "mnexus/public/mnexus.h"

#include "mbase/public/assert.h"
#include "mbase/public/log.h"

namespace mhevcenc {

namespace {

constexpr mnexus::VideoH265Profile kProfile     = mnexus::VideoH265Profile::kMain;
constexpr mnexus::VideoBitDepth    kBitDepth    = mnexus::VideoBitDepth::k8;
constexpr uint32_t                 kMaxDpbSlots = 2;
constexpr uint32_t                 kMaxActiveRefs = 1;

uint64_t AlignUp(uint64_t v, uint64_t align) {
  return (align == 0) ? v : (((v + align - 1) / align) * align);
}

/// Generous upper bound on bytes per access unit: raw 4:2:0 size
/// (1.5 x luma). Even worst-case all-intra encode at low QP fits
/// comfortably; for inter-predicted P-frames the real output is much
/// smaller.
uint32_t EstimateMaxAuBytes(uint32_t coded_width, uint32_t coded_height) {
  return coded_width * coded_height * 3u / 2u;
}

} // anonymous namespace

struct HevcEncodeSession::Impl final {
  mnexus::IDevice*                     device                          = nullptr;
  EncoderConfig                        config{};
  std::vector<uint8_t>                 vps_sps_pps_bytes;
  mnexus::VideoSessionHandle           video_session_handle;
  mnexus::VideoSessionParametersHandle video_session_parameters_handle;
  mnexus::TextureHandle                dpb_texture_handle;
  mnexus::BufferHandle                 bitstream_buffer_handle;
  uint32_t                             bitstream_buffer_size           = 0;
  mnexus::QueueId                      encode_queue;

  /// Bumped at every `SubmitPicture`. Drives GOP scheduling
  /// (frame_idx % gop_size == 0 -> IDR) so the schedule stays correct
  /// even when the caller hasn't yet drained the previous submission.
  uint32_t                             submitted_count                 = 0;
  /// Bumped at every successful `WaitAndReceive`. Reflects how many
  /// AUs have actually landed back in caller hands. Surfaced via the
  /// `encoded_picture_count()` accessor.
  uint32_t                             encoded_picture_count           = 0;

  /// Whether the DPB texture's array layers have been transitioned out
  /// of `kUndefined` into `kVideoEncodeDpb`. The transition is a one-shot
  /// emitted by the first `SubmitPicture` call; after that the layout is
  /// stable for the rest of the session.
  bool                                 dpb_layout_initialized          = false;

  /// Slot index + POC of the most recent submitted picture (NOT
  /// merely the most recent completed). Updated at `SubmitPicture`
  /// time because the GPU encode queue executes submissions FIFO --
  /// when we submit the next P, the prev frame's encode-queue work is
  /// guaranteed to complete before this frame's encode reads from
  /// `prev_setup_slot`, even if its CPU-side bytes haven't been
  /// drained yet. `prev_setup_slot` stays at the sentinel 0xFF until
  /// the first submission.
  uint8_t                              prev_setup_slot                 = 0xFF;
  int32_t                              prev_poc                        = 0;

  /// State of the at-most-one in-flight encode submission. Set by
  /// `SubmitPicture`, consumed by `WaitAndReceive`. The pipeline
  /// depth is 1 because the bitstream buffer is a single non-ring
  /// allocation; ringing it would let multiple submissions overlap.
  struct Pending final {
    mnexus::IntraQueueSubmissionId submission_id;
    bool     is_idr      = false;
    int32_t  poc         = 0;
    uint32_t encode_index = 0;  // 1-indexed, matches `submitted_count` after increment
  };
  std::optional<Pending>               pending;

  ~Impl() {
    if (device == nullptr) return;
    if (video_session_parameters_handle.IsValid()) device->DestroyVideoSessionParameters(video_session_parameters_handle);
    if (video_session_handle.IsValid())            device->DestroyVideoSession(video_session_handle);
    if (bitstream_buffer_handle.IsValid())         device->DestroyBuffer(bitstream_buffer_handle);
    if (dpb_texture_handle.IsValid())              device->DestroyTexture(dpb_texture_handle);
  }
};

std::unique_ptr<HevcEncodeSession> HevcEncodeSession::Create(
  mnexus::IDevice* device, EncoderConfig const& config
) {
  if (device == nullptr) {
    MBASE_LOG_ERROR("HevcEncodeSession::Create: device is null.");
    return nullptr;
  }
  if (config.width == 0 || config.height == 0) {
    MBASE_LOG_ERROR("HevcEncodeSession::Create: invalid dimensions ({}x{}).", config.width, config.height);
    return nullptr;
  }
  if (config.qp < 0 || config.qp > 51) {
    MBASE_LOG_ERROR("HevcEncodeSession::Create: qp {} out of spec range [0, 51].", config.qp);
    return nullptr;
  }
  if (config.gop_size == 0) {
    MBASE_LOG_ERROR("HevcEncodeSession::Create: gop_size must be >= 1.");
    return nullptr;
  }

  // Query encode capabilities.
  mnexus::VideoEncodeH265Capabilities caps{};
  if (!device->QueryVideoEncodeH265Capabilities(kProfile, kBitDepth, caps)) {
    MBASE_LOG_ERROR("HevcEncodeSession::Create: H.265 encode Main+8bit not supported by device.");
    return nullptr;
  }
  if (caps.picture_format == mnexus::Format::kUndefined) {
    MBASE_LOG_ERROR("HevcEncodeSession::Create: driver did not report a usable encode picture format.");
    return nullptr;
  }
  MBASE_LOG_INFO("HevcEncodeSession: caps max_level={}, picture_format={}, max_p_l0_ref={}",
    static_cast<int32_t>(caps.max_level),
    static_cast<int32_t>(caps.picture_format),
    static_cast<int32_t>(caps.max_p_picture_l0_reference_count));

  // Queue selection.
  mnexus::QueueSelection queue_selection;
  device->GetQueueSelection(queue_selection);
  if (!queue_selection.dedicated_video_encode.has_value()) {
    MBASE_LOG_ERROR("HevcEncodeSession::Create: no dedicated_video_encode queue available.");
    return nullptr;
  }

  // Session.
  mnexus::VideoSessionEncodeH265Desc session_desc{};
  session_desc.profile                       = kProfile;
  session_desc.bit_depth                     = kBitDepth;
  session_desc.picture_format                = caps.picture_format;
  session_desc.max_coded_extent              = mnexus::Extent2d{ config.width, config.height };
  session_desc.max_dpb_slots                 = kMaxDpbSlots;
  session_desc.max_active_reference_pictures = kMaxActiveRefs;
  session_desc.max_level                     = config.level;
  mnexus::VideoSessionHandle const session_handle = device->CreateVideoSessionEncodeH265(session_desc);
  if (!session_handle.IsValid()) {
    MBASE_LOG_ERROR("HevcEncodeSession::Create: CreateVideoSessionEncodeH265 failed.");
    return nullptr;
  }

  // Parameters.
  mnexus::VideoSessionParametersEncodeH265Desc params_desc{};
  params_desc.session              = session_handle;
  params_desc.level                = config.level;
  params_desc.coded_width          = config.width;
  params_desc.coded_height         = config.height;
  params_desc.num_ref_frames       = 1;
  params_desc.max_num_reorder_pics = 0;
  params_desc.quality_level        = config.quality_level;
  mnexus::VideoSessionParametersHandle const params_handle =
    device->CreateVideoSessionParametersEncodeH265(params_desc);
  if (!params_handle.IsValid()) {
    MBASE_LOG_ERROR("HevcEncodeSession::Create: CreateVideoSessionParametersEncodeH265 failed.");
    device->DestroyVideoSession(session_handle);
    return nullptr;
  }

  // Read back driver-generated VPS+SPS+PPS bytes (two-call pattern).
  std::vector<uint8_t> vps_sps_pps_bytes;
  {
    uint64_t needed = 0;
    if (!device->GetEncodedVideoSessionParametersBytes(params_handle, &needed, nullptr) || needed == 0) {
      MBASE_LOG_ERROR("HevcEncodeSession::Create: GetEncodedVideoSessionParametersBytes (size) failed.");
      device->DestroyVideoSessionParameters(params_handle);
      device->DestroyVideoSession(session_handle);
      return nullptr;
    }
    vps_sps_pps_bytes.resize(static_cast<size_t>(needed));
    if (!device->GetEncodedVideoSessionParametersBytes(params_handle, &needed, vps_sps_pps_bytes.data())) {
      MBASE_LOG_ERROR("HevcEncodeSession::Create: GetEncodedVideoSessionParametersBytes (data) failed.");
      device->DestroyVideoSessionParameters(params_handle);
      device->DestroyVideoSession(session_handle);
      return nullptr;
    }
    vps_sps_pps_bytes.resize(static_cast<size_t>(needed));
  }
  MBASE_LOG_INFO("HevcEncodeSession: generated VPS+SPS+PPS bytes = {}", vps_sps_pps_bytes.size());

  // DPB texture (2-layer 2D array of the encode picture format).
  mnexus::TextureDesc dpb_desc{};
  dpb_desc.dimension         = mnexus::TextureDimension::k2DArray;
  dpb_desc.format            = caps.picture_format;
  dpb_desc.usage             = mnexus::TextureUsageFlagBits::kVideoEncodeDpb;
  dpb_desc.width             = config.width;
  dpb_desc.height            = config.height;
  dpb_desc.depth             = 1;
  dpb_desc.mip_level_count   = 1;
  dpb_desc.array_layer_count = kMaxDpbSlots;
  mnexus::TextureHandle const dpb_handle = device->CreateTexture(dpb_desc);
  if (!dpb_handle.IsValid()) {
    MBASE_LOG_ERROR("HevcEncodeSession::Create: CreateTexture (DPB) failed.");
    device->DestroyVideoSessionParameters(params_handle);
    device->DestroyVideoSession(session_handle);
    return nullptr;
  }

  // Bitstream output buffer (mappable so the byte-count readback path can
  // memcpy directly after waiting on the encode submission).
  uint32_t const buffer_size = static_cast<uint32_t>(AlignUp(
    EstimateMaxAuBytes(config.width, config.height),
    caps.common.min_bitstream_buffer_size_alignment
  ));
  mnexus::BufferDesc buf_desc{};
  buf_desc.usage         = mnexus::BufferUsageFlagBits::kVideoEncodeDst
                         | mnexus::BufferUsageFlagBits::kMappable;
  buf_desc.size_in_bytes = buffer_size;
  mnexus::BufferHandle const buf_handle = device->CreateBuffer(buf_desc);
  if (!buf_handle.IsValid()) {
    MBASE_LOG_ERROR("HevcEncodeSession::Create: CreateBuffer (bitstream) failed.");
    device->DestroyTexture(dpb_handle);
    device->DestroyVideoSessionParameters(params_handle);
    device->DestroyVideoSession(session_handle);
    return nullptr;
  }

  auto session = std::unique_ptr<HevcEncodeSession>(new HevcEncodeSession());
  Impl& s = *session->impl_;
  s.device                          = device;
  s.config                          = config;
  s.vps_sps_pps_bytes               = std::move(vps_sps_pps_bytes);
  s.video_session_handle            = session_handle;
  s.video_session_parameters_handle = params_handle;
  s.dpb_texture_handle              = dpb_handle;
  s.bitstream_buffer_handle         = buf_handle;
  s.bitstream_buffer_size           = buffer_size;
  s.encode_queue                    = queue_selection.dedicated_video_encode.value();

  MBASE_LOG_INFO("HevcEncodeSession created: {}x{}, qp={}, gop={}, bitstream_buffer={}B",
    config.width, config.height, config.qp, config.gop_size, buffer_size);
  return session;
}

HevcEncodeSession::HevcEncodeSession() : impl_(std::make_unique<Impl>()) {}
HevcEncodeSession::~HevcEncodeSession() = default;

EncoderConfig const& HevcEncodeSession::config() const {
  return impl_->config;
}

std::vector<uint8_t> const& HevcEncodeSession::vps_sps_pps_bytes() const {
  return impl_->vps_sps_pps_bytes;
}

uint32_t HevcEncodeSession::encoded_picture_count() const {
  return impl_->encoded_picture_count;
}

std::optional<EncodedFrameData> HevcEncodeSession::EncodePicture(
  mnexus::TextureHandle src_picture, uint32_t src_array_layer
) {
  auto token = SubmitPicture(src_picture, src_array_layer);
  if (!token.has_value()) return std::nullopt;
  return WaitAndReceive(*token);
}

std::optional<HevcEncodeSession::SubmissionToken> HevcEncodeSession::SubmitPicture(
  mnexus::TextureHandle src_picture, uint32_t src_array_layer
) {
  Impl& s = *impl_;
  if (s.pending.has_value()) {
    MBASE_LOG_ERROR("HevcEncodeSession::SubmitPicture: an outstanding submission exists; call WaitAndReceive first.");
    return std::nullopt;
  }

  // GOP scheduling: every gop_size-th picture (counting from 0) is an IDR.
  // Driven by submitted_count so the schedule stays correct under the
  // async path where the caller may not have drained the previous AU.
  uint32_t const frame_idx = s.submitted_count;
  bool     const is_idr    = (frame_idx % s.config.gop_size) == 0;
  int32_t  const poc       = is_idr ? 0 : (s.prev_poc + 1);

  // DPB slot allocation:
  //   IDR  -> slot 0 (overwrites prior content; subsequent P refs read it).
  //   P    -> ping-pong between 1 and 0 starting at slot 1 for the first P.
  // The current frame's setup slot must differ from the slot of its reference.
  uint8_t  const setup_slot = is_idr
    ? static_cast<uint8_t>(0)
    : static_cast<uint8_t>((s.prev_setup_slot == 0) ? 1 : 0);

  // ----- Build the bound-reference-slots array (setup + active refs) -----
  std::vector<mnexus::VideoReferenceSlotInfo> bound_slots;
  std::vector<mnexus::VideoReferenceSlotInfo> active_refs;
  bound_slots.reserve(2);
  bound_slots.push_back(mnexus::VideoReferenceSlotInfo{
    .slot_index        = -1,                         // -1 = reconstructed picture
    .picture           = s.dpb_texture_handle,
    .array_layer       = setup_slot,
    .pic_order_cnt_val = poc,
  });
  if (!is_idr) {
    mnexus::VideoReferenceSlotInfo const ref{
      .slot_index        = static_cast<int32_t>(s.prev_setup_slot),
      .picture           = s.dpb_texture_handle,
      .array_layer       = s.prev_setup_slot,
      .pic_order_cnt_val = s.prev_poc,
    };
    bound_slots.push_back(ref);
    active_refs.push_back(ref);
  }

  // ----- Build the encode picture info -----
  mnexus::EncodeVideoH265PictureInfo pic_info{};
  pic_info.picture_type             = is_idr
    ? mnexus::VideoEncodeH265PictureType::kIdr
    : mnexus::VideoEncodeH265PictureType::kP;
  pic_info.pic_order_cnt_val        = poc;
  pic_info.temporal_id              = 0;
  pic_info.constant_qp              = s.config.qp;
  pic_info.pps_pic_parameter_set_id = 0;
  for (auto& v : pic_info.list0_dpb_slot_indices) v = 0xFF;
  pic_info.list0_dpb_slot_count     = 0;
  if (!is_idr) {
    pic_info.list0_dpb_slot_indices[0] = s.prev_setup_slot;
    pic_info.list0_dpb_slot_count      = 1;
  }

  // ----- Record + submit the encode CL on the dedicated encode queue -----
  mnexus::ICommandList* cl = s.device->CreateCommandList(
    mnexus::CommandListDesc{ .queue_family_index = s.encode_queue.queue_family_index });
  {
    MNEXUS_SCOPED_DEBUG_REGION(cl, "HevcEncodePicture");

    if (!s.dpb_layout_initialized) {
      // One-shot: bring both DPB array layers out of kUndefined into
      // kVideoEncodeDpb. We never transition out for the rest of the
      // session, so a single barrier covers all subsequent encodes.
      cl->TextureBarrier(
        s.dpb_texture_handle,
        mnexus::TextureSubresourceRange{
          .aspect_mask       = mnexus::TextureAspectFlagBits::kPlane0
                             | mnexus::TextureAspectFlagBits::kPlane1,
          .base_mip_level    = 0,
          .mip_level_count   = 1,
          .base_array_layer  = 0,
          .array_layer_count = kMaxDpbSlots,
        },
        mnexus::ResourceBarrierStageFlagBits::kVideoEncode,
        mnexus::ResourceBarrierState::kVideoEncodeDpb
      );
      s.dpb_layout_initialized = true;
    }

    mnexus::BeginVideoCodingDesc begin_desc{};
    begin_desc.session              = s.video_session_handle;
    begin_desc.parameters           = s.video_session_parameters_handle;
    begin_desc.bound_reference_slots = mnexus::container::ArrayProxy<mnexus::VideoReferenceSlotInfo const>{
      bound_slots.data(), static_cast<uint32_t>(bound_slots.size())
    };
    cl->BeginVideoCoding(begin_desc);

    mnexus::EncodeVideoH265Desc encode_desc{};
    encode_desc.dst_buffer        = s.bitstream_buffer_handle;
    encode_desc.dst_buffer_offset = 0;
    encode_desc.dst_buffer_range  = s.bitstream_buffer_size;
    encode_desc.src_picture       = src_picture;
    encode_desc.src_array_layer   = src_array_layer;
    encode_desc.setup_reference   = mnexus::VideoReferenceSlotInfo{
      .slot_index        = static_cast<int32_t>(setup_slot),
      .picture           = s.dpb_texture_handle,
      .array_layer       = setup_slot,
      .pic_order_cnt_val = poc,
    };
    encode_desc.active_references = mnexus::container::ArrayProxy<mnexus::VideoReferenceSlotInfo const>{
      active_refs.data(), static_cast<uint32_t>(active_refs.size())
    };
    encode_desc.picture_info      = pic_info;
    cl->EncodeVideoH265(encode_desc);

    cl->EndVideoCoding();
  }
  cl->End();

  mnexus::IntraQueueSubmissionId const id = s.device->QueueSubmitCommandList(s.encode_queue, cl);

  // Update prev-picture state immediately so the next SubmitPicture
  // (which may arrive before the WaitAndReceive for this one) picks
  // the right setup slot + POC delta. The GPU encode queue's FIFO
  // guarantees this submission's setup write completes before the next
  // submission's read of that DPB slot.
  s.prev_setup_slot = setup_slot;
  s.prev_poc        = poc;
  ++s.submitted_count;
  s.pending = Impl::Pending{
    .submission_id = id,
    .is_idr        = is_idr,
    .poc           = poc,
    .encode_index  = s.submitted_count,
  };
  return id;
}

std::optional<EncodedFrameData> HevcEncodeSession::WaitAndReceive(SubmissionToken token) {
  Impl& s = *impl_;
  if (!s.pending.has_value()) {
    MBASE_LOG_ERROR("HevcEncodeSession::WaitAndReceive: no outstanding submission.");
    return std::nullopt;
  }
  if (s.pending->submission_id != token) {
    MBASE_LOG_ERROR("HevcEncodeSession::WaitAndReceive: token mismatch.");
    return std::nullopt;
  }
  Impl::Pending const p = *s.pending;
  s.pending.reset();

  s.device->QueueWaitIdle(s.encode_queue, p.submission_id);

  // Bytes-written readback (drains the feedback query inline since we
  // just QueueWaitIdle'd on the submission, so the result is ready).
  uint64_t bytes_written = 0;
  if (!s.device->GetLastEncodedBytesWritten(s.video_session_handle, &bytes_written)) {
    MBASE_LOG_ERROR("HevcEncodeSession::WaitAndReceive: GetLastEncodedBytesWritten failed.");
    return std::nullopt;
  }
  if (bytes_written == 0 || bytes_written > s.bitstream_buffer_size) {
    MBASE_LOG_ERROR("HevcEncodeSession::WaitAndReceive: implausible bytes_written = {} (buffer size {}).",
      bytes_written, s.bitstream_buffer_size);
    return std::nullopt;
  }

  // Copy the encoded bytes out of the mappable bitstream buffer.
  std::vector<uint8_t> au_bytes(static_cast<size_t>(bytes_written));
  s.device->QueueReadBuffer(
    s.encode_queue, s.bitstream_buffer_handle,
    /*buffer_offset=*/0,
    au_bytes.data(),
    static_cast<uint32_t>(bytes_written)
  );

  s.encoded_picture_count = p.encode_index;

  EncodedFrameData result;
  result.au_bytes     = std::move(au_bytes);
  result.is_irap      = p.is_idr;
  result.poc          = p.poc;
  result.encode_index = p.encode_index;
  return result;
}

} // namespace mhevcenc
