/**
 * @file src/nvenc/nvenc_config.h
 * @brief Declarations for NVENC encoder configuration.
 */
#pragma once

// standard includes
#include <algorithm>
#include <cstdint>

namespace nvenc {

  /**
   * @brief Largest H.264 reference picture list L0 accepted by the NVENC SDK.
   *
   * `NV_ENC_CONFIG_H264::numRefL0` tops out at `NV_ENC_NUM_REF_FRAMES_7`.
   */
  constexpr std::uint32_t max_ref_list_h264 = 7;

  /**
   * @brief Largest HEVC reference picture list L0 accepted by the NVENC SDK.
   *
   * `NV_ENC_CONFIG_HEVC::numRefL0` tops out at `NV_ENC_NUM_REF_FRAMES_7`.
   */
  constexpr std::uint32_t max_ref_list_hevc = 7;

  /**
   * @brief Largest AV1 forward reference count accepted by the NVENC SDK.
   *
   * `NV_ENC_CONFIG_AV1::numFwdRefs` only addresses Last, Last2, Last3 and Golden.
   */
  constexpr std::uint32_t max_ref_list_av1 = 4;

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

    // Allocate more bitrate to flat regions since they're visually more perceptible, uses CUDA cores
    bool adaptive_quantization = false;  ///< Enable adaptive quantization for NVENC.

    // Shift bitrate towards regions that stay perceptually important across frames, uses CUDA cores
    bool temporal_adaptive_quantization = false;  ///< Enable temporal adaptive quantization for NVENC.

    // AQ strength from 1 to 15, zero uses the driver default
    int aq_strength = 0;  ///< Adaptive quantization strength.

    // Insert automatically chosen non-reference P-frames, saves bitrate on static content
    bool non_reference_p_frames = false;  ///< Enable non-reference P-frames for NVENC.

    // Replace P-frames with B-frames that only reference past frames, improves compression without adding latency
    bool unidirectional_b_frames = false;  ///< Enable unidirectional B-frames for NVENC.

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
   * @brief Resolve the DPB size written to the codec configuration.
   *
   * The DPB size is an input to the encoder, so it has to be set explicitly to guarantee the
   * repair window reference frame invalidation relies on.
   *
   * @param requested_count Reference frame count requested by the client, zero for the default.
   * @param default_count DPB size used when the client requests none.
   * @return DPB size to write, always at least one frame.
   */
  inline std::uint32_t dpb_size(int requested_count, std::uint32_t default_count) {
    return requested_count > 0 ? static_cast<std::uint32_t>(requested_count) : std::max(default_count, std::uint32_t {1});
  }

  /**
   * @brief Decide whether P-frames can be replaced with unidirectional B-frames.
   *
   * Unidirectional B-frames predict from two past frames, so they need at least two reference
   * frames in the DPB. They keep the decode order equal to the display order, which is why they
   * do not add latency.
   *
   * The NVENC SDK currently exposes this feature for the HEVC encoder only.
   *
   * @param requested Whether the configuration enables the feature.
   * @param supported Whether the GPU reports support for the feature.
   * @param ref_frames_in_dpb Reference frames available in the DPB.
   * @return True when the encoder can use unidirectional B-frames.
   */
  inline bool use_unidirectional_b_frames(bool requested, bool supported, std::uint32_t ref_frames_in_dpb) {
    return requested && supported && ref_frames_in_dpb >= 2;
  }

}  // namespace nvenc
