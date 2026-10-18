/**
 * @file src/nvenc/nvenc_encoder.h
 * @brief Declarations for the SDK-neutral NVENC encoder interface.
 */
#pragma once

// standard includes
#include <cstdint>
#include <vector>

// local includes
#include "nvenc_config.h"
#include "nvenc_encoded_frame.h"

namespace platf {
  enum class pix_fmt_e;
}

namespace video {
  struct config_t;
  struct sunshine_colorspace_t;
}  // namespace video

namespace nvenc {

  /**
   * @brief SDK-neutral standalone NVENC encoder interface.
   */
  class nvenc_encoder {
  public:
    /**
     * @brief Destroy the SDK-neutral encoder interface.
     */
    virtual ~nvenc_encoder() = default;

    /**
     * @brief Create the encoder.
     *
     * @param config NVENC encoder configuration.
     * @param client_config Stream configuration requested by the client.
     * @param colorspace Sunshine colorspace metadata.
     * @param buffer_format Platform-agnostic input surface format.
     * @return `true` on success, `false` on error.
     */
    virtual bool create_encoder(
      const nvenc_config &config,
      const video::config_t &client_config,
      const video::sunshine_colorspace_t &colorspace,
      platf::pix_fmt_e buffer_format
    ) = 0;

    /**
     * @brief Destroy the encoder.
     */
    virtual void destroy_encoder() = 0;

    /**
     * @brief Encode the next frame using the platform-specific input surface.
     *
     * When rate control lookahead is enabled the encoder buffers input frames internally, so a
     * submission does not necessarily produce an output immediately. At most one frame becomes
     * available per call.
     *
     * @param frame_index Frame index that uniquely identifies the frame.
     * @param force_idr Whether to encode the frame as a forced IDR.
     * @param repeat_last_frame Whether the input surface still holds the previously submitted frame
     *        and must be duplicated into a fresh surface before being submitted again.
     * @param out_frames List that receives every frame that became available during this call.
     * @return `true` on success, `false` on error.
     */
    virtual bool encode_frame(std::uint64_t frame_index, bool force_idr, bool repeat_last_frame, std::vector<nvenc_encoded_frame> &out_frames) = 0;

    /**
     * @brief Signal the end of the input stream and collect every frame still buffered internally.
     *
     * @param out_frames List that receives every frame that became available during this call.
     * @return `true` on success, `false` on error.
     */
    virtual bool flush(std::vector<nvenc_encoded_frame> &out_frames) = 0;

    /**
     * @brief Invalidate reference frames in the requested range.
     *
     * @param first_frame First frame index of the invalidation range.
     * @param last_frame Last frame index of the invalidation range.
     * @return `true` on success, `false` on error.
     */
    virtual bool invalidate_ref_frames(std::uint64_t first_frame, std::uint64_t last_frame) = 0;

    /**
     * @brief Get the rate control lookahead depth the driver was initialized with.
     *
     * The value is the effective depth, so it is zero when lookahead is disabled or unsupported.
     *
     * @return Number of pictures the driver buffers before it emits one.
     */
    virtual int lookahead_depth() const = 0;
  };

}  // namespace nvenc
