/**
 * @file src/nvenc/nvenc_config.h
 * @brief Declarations for NVENC encoder configuration.
 */
#pragma once

// standard includes
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace nvenc {

  /**
   * @brief Maximum lookahead depth accepted by the NVENC SDK with no B-frames.
   */
  constexpr int max_lookahead_depth = 16;

  /**
   * @brief Maximum rate control lookahead level defined by the NVENC SDK.
   */
  constexpr int max_lookahead_level = 3;

  /**
   * @brief Enumerates supported nVENC two pass options.
   */
  enum class nvenc_two_pass {
    disabled,  ///< Single pass, the fastest and no extra vram
    quarter_resolution,  ///< Larger motion vectors being caught, faster and uses less extra vram
    full_resolution,  ///< Better overall statistics, slower and uses more extra vram
  };

  /**
   * @brief Enumerates supported nVENC split frame encoding options.
   */
  enum class nvenc_split_frame_encoding {
    disabled,  ///< Disable
    driver_decides,  ///< Let driver decide
    force_enabled,  ///< Force-enable
  };

  /**
   * @brief NVENC encoder configuration.
   */
  struct nvenc_config {
    // Quality preset from 1 to 7, higher is slower
    int quality_preset = 1;  ///< Quality preset.

    // Use optional preliminary pass for better motion vectors, bitrate distribution and stricter VBV(HRD), uses CUDA cores
    nvenc_two_pass two_pass = nvenc_two_pass::quarter_resolution;  ///< Two pass.

    // Ratio of I-frame bits to P-frame bits in single frame VBV CBR mode, zero uses the driver default of the tuning info
    int low_delay_key_frame_scale = 0;  ///< Low delay key frame scale.

    // Improves fades compression, uses CUDA cores
    bool weighted_prediction = false;  ///< Enable weighted prediction for NVENC.

    // Allocate more bitrate to flat regions since they're visually more perceptible, uses CUDA cores
    bool adaptive_quantization = false;  ///< Enable adaptive quantization for NVENC.

    // AQ strength from 1 to 15, zero uses the driver default
    int aq_strength = 0;  ///< Adaptive quantization strength.

    // Insert automatically chosen non-reference P-frames, saves bitrate on static content
    bool non_reference_p_frames = false;  ///< Enable non-reference P-frames for NVENC.

    // Don't use QP below certain value, limits peak image quality to save bitrate
    bool enable_min_qp = false;  ///< Enable minimum QP limits for NVENC.

    // Min QP value for H.264 when enable_min_qp is selected
    int min_qp_h264 = 19;  ///< Min qp h264.

    // Min QP value for HEVC when enable_min_qp is selected
    int min_qp_hevc = 23;  ///< Min qp HEVC.

    // Min QP value for AV1 when enable_min_qp is selected
    int min_qp_av1 = 23;  ///< Min qp AV1.

    // Don't use QP above certain value, prevents frames the encoder cannot afford from turning into a blurry mess
    bool enable_max_qp = false;  ///< Enable maximum QP limits for NVENC.

    // Max QP value for H.264 when enable_max_qp is selected
    int max_qp_h264 = 51;  ///< Max qp h264.

    // Max QP value for HEVC when enable_max_qp is selected
    int max_qp_hevc = 51;  ///< Max qp HEVC.

    // Max QP value for AV1 when enable_max_qp is selected
    int max_qp_av1 = 255;  ///< Max qp AV1.

    // Use CAVLC entropy coding in H.264 instead of CABAC, not relevant and here for historical reasons
    bool h264_cavlc = false;  ///< Use CAVLC entropy coding for H.264.

    // Add filler data to encoded frames to stay at target bitrate, mainly for testing
    bool insert_filler_data = false;  ///< Insert filler data to maintain bitrate constraints.

    // Enable split-frame encoding if the gpu has multiple NVENC hardware clusters
    nvenc_split_frame_encoding split_frame_encoding = nvenc_split_frame_encoding::driver_decides;  ///< Split frame encoding.

    // Size of the VBV/HRD buffer in milliseconds of target bitrate, zero keeps the default single frame buffer
    int vbv_milliseconds = 0;  ///< Vbv buffer size in milliseconds.

    // Number of frames the rate control may look ahead, zero disables lookahead buffering
    int lookahead_depth = 0;  ///< Rate control lookahead depth in frames.

    // Analysis level of the rate control lookahead, zero keeps the level derived from the tuning info
    int lookahead_level = 0;  ///< Rate control lookahead analysis level.
  };

  /**
   * @brief Resolve the bitrate the encoder has to target.
   *
   * The client requests the bitrate, but the server may cap it with the `max_bitrate` option.
   * Every rate control value has to use the same resolved bitrate, otherwise the target bitrate
   * and the VBV/HRD window are derived from different rates.
   *
   * @param requested_kbps Bitrate requested by the client in kilobits per second.
   * @param ceiling_kbps Configured bitrate ceiling in kilobits per second, zero disables the ceiling.
   * @return Requested bitrate, or the ceiling when it is lower.
   */
  inline int effective_bitrate_kbps(int requested_kbps, int ceiling_kbps) {
    return ceiling_kbps > 0 ? std::min(requested_kbps, ceiling_kbps) : requested_kbps;
  }

  /**
   * @brief Compute the VBV/HRD buffer size in bits requested by the configuration.
   *
   * @param config NVENC encoder configuration.
   * @param framerate Encoded frames per second.
   * @param bitrate_kbps Target bitrate in kilobits per second.
   * @return Buffer size in bits, a single frame of target bitrate when no larger window is requested.
   */
  inline std::uint64_t vbv_buffer_size_bits(const nvenc_config &config, int framerate, int bitrate_kbps) {
    if (framerate <= 0 || bitrate_kbps <= 0) {
      return 0;
    }

    const auto bitrate = static_cast<std::uint64_t>(bitrate_kbps);
    const auto single_frame_size = bitrate * 1000 / static_cast<std::uint64_t>(framerate);

    if (config.vbv_milliseconds > 0) {
      const auto requested = bitrate * static_cast<std::uint64_t>(config.vbv_milliseconds);
      if (requested > single_frame_size) {
        return requested;
      }
    }

    return single_frame_size;
  }

  /**
   * @brief Compute how many NVENC input surfaces one lookahead depth requires to run.
   *
   * The driver holds `lookahead_depth` pictures in steady state, one more surface is being
   * converted, and one spare keeps the ring from running full.
   *
   * @param lookahead_depth Effective lookahead depth the driver was initialized with.
   * @return Number of input surfaces that must stay allocated and registered.
   */
  inline std::size_t input_surface_count(int lookahead_depth) {
    if (lookahead_depth <= 0) {
      return 1;
    }

    return static_cast<std::size_t>(lookahead_depth) + 2;
  }

}  // namespace nvenc
