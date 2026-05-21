#include "mhevcenc/public/hevc_encode_session.h"

// public project headers -------------------------------
#include "mnexus/public/mnexus.h"

#include "mbase/public/assert.h"
#include "mbase/public/log.h"

namespace mhevcenc {

struct HevcEncodeSession::Impl final {
  EncoderConfig        config;
  std::vector<uint8_t> vps_sps_pps_bytes;
  uint32_t             encoded_picture_count = 0;
};

std::unique_ptr<HevcEncodeSession> HevcEncodeSession::Create(
  mnexus::IDevice* device, EncoderConfig const& config
) {
  if (device == nullptr) {
    MBASE_LOG_ERROR("HevcEncodeSession::Create: device is null.");
    return nullptr;
  }
  if (config.width == 0 || config.height == 0) {
    MBASE_LOG_ERROR("HevcEncodeSession::Create: invalid dimensions ({}x{}).",
      config.width, config.height);
    return nullptr;
  }
  if (config.qp < 0 || config.qp > 51) {
    MBASE_LOG_ERROR("HevcEncodeSession::Create: qp {} out of spec range [0, 51].",
      config.qp);
    return nullptr;
  }
  if (config.gop_size == 0) {
    MBASE_LOG_ERROR("HevcEncodeSession::Create: gop_size must be >= 1.");
    return nullptr;
  }

  // TODO: Full implementation pending the mnexus Vulkan-backend encode
  // path. Wires up: capability query, video session create, parameters
  // create + VPS/SPS/PPS readback, DPB texture, output bitstream
  // buffer, POC counter, GOP scheduler.
  MBASE_LOG_ERROR("HevcEncodeSession::Create is not yet implemented.");
  return nullptr;
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
  mnexus::TextureHandle /*src_picture*/, uint32_t /*src_array_layer*/
) {
  MBASE_LOG_ERROR("HevcEncodeSession::EncodePicture is not yet implemented.");
  return std::nullopt;
}

} // namespace mhevcenc
