/**
 * @file src/nvenc/nvenc_base.cpp
 * @brief Definitions for abstract platform-agnostic base of standalone NVENC encoder.
 */
// this include
#include "nvenc_base.h"

// standard includes
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <string_view>
#include <utility>
#include <vector>

// local includes
#include "nvenc_utils.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/utility.h"

namespace {

  using namespace NVENC_NAMESPACE;

  /**
   * @brief Determine whether an NVENC buffer format stores 10-bit samples.
   *
   * @param buffer_format NVENC input buffer format.
   * @return `true` for a 10-bit format, otherwise `false`.
   */
  bool buffer_is_10bit(NV_ENC_BUFFER_FORMAT buffer_format) {
    return buffer_format == NV_ENC_BUFFER_FORMAT_YUV420_10BIT || buffer_format == NV_ENC_BUFFER_FORMAT_YUV444_10BIT;
  }

  /**
   * @brief Determine whether an NVENC buffer format stores YUV 4:4:4 samples.
   *
   * @param buffer_format NVENC input buffer format.
   * @return `true` for a YUV 4:4:4 format, otherwise `false`.
   */
  bool buffer_is_yuv444(NV_ENC_BUFFER_FORMAT buffer_format) {
    return buffer_format == NV_ENC_BUFFER_FORMAT_AYUV ||
           buffer_format == NV_ENC_BUFFER_FORMAT_YUV444 ||
           buffer_format == NV_ENC_BUFFER_FORMAT_YUV444_10BIT;
  }

  /**
   * @brief Determine whether a codec GUID appears in the driver-provided list.
   *
   * @param encode_guids Driver-provided codec GUIDs.
   * @param encode_guid Codec GUID to locate.
   * @return `true` when the codec is supported, otherwise `false`.
   */
  bool contains_guid(const std::vector<GUID> &encode_guids, const GUID &encode_guid) {
    return std::ranges::any_of(encode_guids, [&](const GUID &guid) {
      return std::memcmp(&encode_guid, &guid, sizeof(GUID)) == 0;
    });
  }

  /**
   * @brief Get the display name for a Sunshine video format.
   *
   * @param video_format Sunshine video format identifier.
   * @return Display name including its trailing separator.
   */
  std::string_view video_format_name(int video_format) {
    switch (video_format) {
      case 0:
        return "H.264 ";
      case 1:
        return "HEVC ";
      case 2:
        return "AV1 ";
      default:
        return " ";
    }
  }

  /**
   * @brief Convert an NVENC status value to its symbolic name.
   *
   * @param status NVENC status value.
   * @return Symbolic name when known, otherwise its numeric value.
   */
  std::string nvenc_status_string(NVENCSTATUS status) {
    static constexpr auto names = std::to_array<std::pair<NVENCSTATUS, std::string_view>>({
      {NV_ENC_SUCCESS, "NV_ENC_SUCCESS"},
      {NV_ENC_ERR_NO_ENCODE_DEVICE, "NV_ENC_ERR_NO_ENCODE_DEVICE"},
      {NV_ENC_ERR_UNSUPPORTED_DEVICE, "NV_ENC_ERR_UNSUPPORTED_DEVICE"},
      {NV_ENC_ERR_INVALID_ENCODERDEVICE, "NV_ENC_ERR_INVALID_ENCODERDEVICE"},
      {NV_ENC_ERR_INVALID_DEVICE, "NV_ENC_ERR_INVALID_DEVICE"},
      {NV_ENC_ERR_DEVICE_NOT_EXIST, "NV_ENC_ERR_DEVICE_NOT_EXIST"},
      {NV_ENC_ERR_INVALID_PTR, "NV_ENC_ERR_INVALID_PTR"},
      {NV_ENC_ERR_INVALID_EVENT, "NV_ENC_ERR_INVALID_EVENT"},
      {NV_ENC_ERR_INVALID_PARAM, "NV_ENC_ERR_INVALID_PARAM"},
      {NV_ENC_ERR_INVALID_CALL, "NV_ENC_ERR_INVALID_CALL"},
      {NV_ENC_ERR_OUT_OF_MEMORY, "NV_ENC_ERR_OUT_OF_MEMORY"},
      {NV_ENC_ERR_ENCODER_NOT_INITIALIZED, "NV_ENC_ERR_ENCODER_NOT_INITIALIZED"},
      {NV_ENC_ERR_UNSUPPORTED_PARAM, "NV_ENC_ERR_UNSUPPORTED_PARAM"},
      {NV_ENC_ERR_LOCK_BUSY, "NV_ENC_ERR_LOCK_BUSY"},
      {NV_ENC_ERR_NOT_ENOUGH_BUFFER, "NV_ENC_ERR_NOT_ENOUGH_BUFFER"},
      {NV_ENC_ERR_INVALID_VERSION, "NV_ENC_ERR_INVALID_VERSION"},
      {NV_ENC_ERR_MAP_FAILED, "NV_ENC_ERR_MAP_FAILED"},
      {NV_ENC_ERR_NEED_MORE_INPUT, "NV_ENC_ERR_NEED_MORE_INPUT"},
      {NV_ENC_ERR_ENCODER_BUSY, "NV_ENC_ERR_ENCODER_BUSY"},
      {NV_ENC_ERR_EVENT_NOT_REGISTERD, "NV_ENC_ERR_EVENT_NOT_REGISTERD"},
      {NV_ENC_ERR_GENERIC, "NV_ENC_ERR_GENERIC"},
      {NV_ENC_ERR_INCOMPATIBLE_CLIENT_KEY, "NV_ENC_ERR_INCOMPATIBLE_CLIENT_KEY"},
      {NV_ENC_ERR_UNIMPLEMENTED, "NV_ENC_ERR_UNIMPLEMENTED"},
      {NV_ENC_ERR_RESOURCE_REGISTER_FAILED, "NV_ENC_ERR_RESOURCE_REGISTER_FAILED"},
      {NV_ENC_ERR_RESOURCE_NOT_REGISTERED, "NV_ENC_ERR_RESOURCE_NOT_REGISTERED"},
      {NV_ENC_ERR_RESOURCE_NOT_MAPPED, "NV_ENC_ERR_RESOURCE_NOT_MAPPED"},
    });

    const auto item = std::ranges::find_if(names, [status](const auto &entry) {
      return entry.first == status;
    });
    return item == names.end() ? std::to_string(status) : std::string {item->second};
  }

  GUID quality_preset_guid_from_number(unsigned number) {
    if (number > 7) {
      number = 7;
    }

    switch (number) {
      case 1:
      default:
        return NV_ENC_PRESET_P1_GUID;

      case 2:
        return NV_ENC_PRESET_P2_GUID;

      case 3:
        return NV_ENC_PRESET_P3_GUID;

      case 4:
        return NV_ENC_PRESET_P4_GUID;

      case 5:
        return NV_ENC_PRESET_P5_GUID;

      case 6:
        return NV_ENC_PRESET_P6_GUID;

      case 7:
        return NV_ENC_PRESET_P7_GUID;
    }
  };

  bool equal_guids(const GUID &guid1, const GUID &guid2) {
    return std::memcmp(&guid1, &guid2, sizeof(GUID)) == 0;
  }

  auto quality_preset_string_from_guid(const GUID &guid) {
    if (equal_guids(guid, NV_ENC_PRESET_P1_GUID)) {
      return "P1";
    }
    if (equal_guids(guid, NV_ENC_PRESET_P2_GUID)) {
      return "P2";
    }
    if (equal_guids(guid, NV_ENC_PRESET_P3_GUID)) {
      return "P3";
    }
    if (equal_guids(guid, NV_ENC_PRESET_P4_GUID)) {
      return "P4";
    }
    if (equal_guids(guid, NV_ENC_PRESET_P5_GUID)) {
      return "P5";
    }
    if (equal_guids(guid, NV_ENC_PRESET_P6_GUID)) {
      return "P6";
    }
    if (equal_guids(guid, NV_ENC_PRESET_P7_GUID)) {
      return "P7";
    }
    return "Unknown";
  }

}  // namespace

namespace NVENC_NAMESPACE {

  nvenc_base::nvenc_base(NV_ENC_DEVICE_TYPE device_type):
      device_type(device_type) {
  }

  nvenc_base::~nvenc_base() {
    // Use destroy_encoder() instead
  }

  int nvenc_base::get_encoder_cap(const GUID &encode_guid, NV_ENC_CAPS cap) const {
    NV_ENC_CAPS_PARAM param = {.version = NV_ENC_CAPS_PARAM_VER};
    param.capsToQuery = cap;
    int value = 0;
    if (nvenc->nvEncGetEncodeCaps(encoder, encode_guid, &param, &value) == NV_ENC_SUCCESS) {
      return value;
    }
    return 0;
  }

  bool nvenc_base::validate_encoder_capabilities(const GUID &encode_guid, NV_ENC_BUFFER_FORMAT buffer_format) {
    const auto supported_width = get_encoder_cap(encode_guid, NV_ENC_CAPS_WIDTH_MAX);
    const auto supported_height = get_encoder_cap(encode_guid, NV_ENC_CAPS_HEIGHT_MAX);
    if (encoder_params.width > supported_width || encoder_params.height > supported_height) {
      BOOST_LOG(error) << "NvEnc: gpu max encode resolution " << supported_width << "x" << supported_height
                       << ", requested " << encoder_params.width << "x" << encoder_params.height;
      return false;
    }
    if (buffer_is_10bit(buffer_format) && !get_encoder_cap(encode_guid, NV_ENC_CAPS_SUPPORT_10BIT_ENCODE)) {
      BOOST_LOG(error) << "NvEnc: gpu doesn't support 10-bit encode";
      return false;
    }
    if (buffer_is_yuv444(buffer_format) && !get_encoder_cap(encode_guid, NV_ENC_CAPS_SUPPORT_YUV444_ENCODE)) {
      BOOST_LOG(error) << "NvEnc: gpu doesn't support YUV444 encode";
      return false;
    }
    encoder_params.rfi = get_encoder_cap(encode_guid, NV_ENC_CAPS_SUPPORT_REF_PIC_INVALIDATION);
    return true;
  }

  void nvenc_base::configure_split_frame(
    NV_ENC_INITIALIZE_PARAMS &init_params,
    const ::nvenc::nvenc_config &config,
    const video::config_t &client_config
  ) const {
#if NVENC_SDK_VERSION >= 1300
    if (client_config.videoFormat <= 0 || get_encoder_cap(init_params.encodeGUID, NV_ENC_CAPS_NUM_ENCODER_ENGINES) <= 1) {
      return;
    }

    using enum ::nvenc::nvenc_split_frame_encoding;
    if (config.split_frame_encoding == disabled) {
      init_params.splitEncodeMode = NV_ENC_SPLIT_DISABLE_MODE;
    } else if (config.split_frame_encoding == force_enabled) {
      init_params.splitEncodeMode = NV_ENC_SPLIT_AUTO_FORCED_MODE;
    } else {
      init_params.splitEncodeMode = NV_ENC_SPLIT_AUTO_MODE;
    }
#else
    if (config.split_frame_encoding == ::nvenc::nvenc_split_frame_encoding::force_enabled) {
      BOOST_LOG(warning) << "NvEnc: split-frame encoding requires NVENC API 13.0; ignoring forced mode";
    }
#endif
  }

  void nvenc_base::configure_rate_control(
    NV_ENC_CONFIG &enc_config,
    const ::nvenc::nvenc_config &config,
    const video::config_t &client_config,
    const GUID &encode_guid
  ) {
    enc_config.gopLength = NVENC_INFINITE_GOPLENGTH;
    enc_config.frameIntervalP = 1;
    enc_config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    enc_config.rcParams.zeroReorderDelay = 1;
    enc_config.rcParams.enableLookahead = 0;
    // Leave the driver default in place, it is derived from the tuning info
    if (config.low_delay_key_frame_scale > 0) {
      enc_config.rcParams.lowDelayKeyFrameScale = config.low_delay_key_frame_scale;
    }

    using enum ::nvenc::nvenc_two_pass;
    if (config.two_pass == quarter_resolution) {
      enc_config.rcParams.multiPass = NV_ENC_TWO_PASS_QUARTER_RESOLUTION;
    } else if (config.two_pass == full_resolution) {
      enc_config.rcParams.multiPass = NV_ENC_TWO_PASS_FULL_RESOLUTION;
    } else {
      enc_config.rcParams.multiPass = NV_ENC_MULTI_PASS_DISABLED;
    }

    enc_config.rcParams.enableAQ = config.adaptive_quantization;
    if (config.aq_strength > 0) {
      // The field stores four bits, higher values are truncated by the driver
      enc_config.rcParams.aqStrength = std::min(config.aq_strength, 15);
    }
    enc_config.rcParams.enableNonRefP = config.non_reference_p_frames;
    // The target bitrate and the VBV window must be based on the same value, otherwise a configured
    // max_bitrate ceiling would cap one of them but not the other.
    const auto bitrate_kbps = ::nvenc::effective_bitrate_kbps(client_config.bitrate, config::video.max_bitrate);
    enc_config.rcParams.averageBitRate = bitrate_kbps * 1000;
    if (get_encoder_cap(encode_guid, NV_ENC_CAPS_SUPPORT_CUSTOM_VBV_BUF_SIZE)) {
      const auto buffer_size = ::nvenc::vbv_buffer_size_bits(config, client_config.framerate, bitrate_kbps);
      enc_config.rcParams.vbvBufferSize = static_cast<uint32_t>(std::min<std::uint64_t>(buffer_size, UINT32_MAX));
    }

    configure_lookahead(enc_config, config, encode_guid, client_config.videoFormat);
  }

  void nvenc_base::configure_lookahead(
    NV_ENC_CONFIG &enc_config,
    const ::nvenc::nvenc_config &config,
    const GUID &encode_guid,
    int video_format
  ) {
    if (config.lookahead_depth <= 0) {
      if (config.lookahead_level > 0) {
        BOOST_LOG(warning) << "NvEnc: lookahead level has no effect without a lookahead depth";
      }
      return;
    }

    if (!get_encoder_cap(encode_guid, NV_ENC_CAPS_SUPPORT_LOOKAHEAD)) {
      BOOST_LOG(warning) << "NvEnc: gpu doesn't support lookahead";
      return;
    }

    // The SDK limits the depth to 31 minus the number of B-frames, and Sunshine always encodes without B-frames
    const auto depth = std::min(config.lookahead_depth, ::nvenc::max_lookahead_depth);

    enc_config.rcParams.enableLookahead = 1;
    enc_config.rcParams.lookaheadDepth = static_cast<uint16_t>(depth);
    encoder_params.lookahead_depth = depth;
    // Adaptive I-frame insertion would break the infinite GOP and produce unpredictable bitrate spikes
    enc_config.rcParams.disableIadapt = 1;
    // Adaptive B-frame insertion is meaningless without B-frames, disable it to keep the output order fixed
    enc_config.rcParams.disableBadapt = 1;

#if NVENC_SDK_VERSION >= 1300
    if (config.lookahead_level > 0) {
      if (video_format == 0) {
        // H.264 rejects a non-zero lookahead level, which would fail the whole NVENC probe
        BOOST_LOG(warning) << "NvEnc: the H.264 encoder does not support a lookahead level, ignoring it";
      } else {
        enc_config.rcParams.lookaheadLevel = static_cast<NV_ENC_LOOKAHEAD_LEVEL>(config.lookahead_level);
      }
    }
#else
    if (config.lookahead_level > 0) {
      BOOST_LOG(warning) << "NvEnc: lookahead level requires NVENC API 13.0; ignoring it";
    }
#endif

    if (depth < config.lookahead_depth) {
      BOOST_LOG(warning) << "NvEnc: lookahead depth "sv << config.lookahead_depth
                         << " is above the supported maximum, using "sv << depth;
    }
  }

  void nvenc_base::configure_reference_frames(
    std::uint32_t &ref_frames_option,
    NV_ENC_NUM_REF_FRAMES &list0_option,
    std::uint32_t default_count,
    int requested_count,
    const GUID &encode_guid
  ) {
    ref_frames_option = requested_count > 0 ? static_cast<std::uint32_t>(requested_count) : default_count;
    if (ref_frames_option > 0U && !get_encoder_cap(encode_guid, NV_ENC_CAPS_SUPPORT_MULTIPLE_REF_FRAMES)) {
      ref_frames_option = 1;
      encoder_params.rfi = false;
    }
    encoder_params.ref_frames_in_dpb = ref_frames_option;
    // Limit each frame to one reference while keeping a larger DPB for RFI fallback.
    list0_option = NV_ENC_NUM_REF_FRAMES_1;
  }

  template<typename FormatConfig>
  void nvenc_base::configure_h264_hevc_metadata(
    FormatConfig &format_config,
    const video::config_t &client_config,
    const nvenc_colorspace_t &colorspace,
    NV_ENC_BUFFER_FORMAT buffer_format,
    const GUID &encode_guid
  ) {
    const auto configure_vui = [&](auto &vui) {
      vui.videoSignalTypePresentFlag = 1;
      vui.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
      vui.videoFullRangeFlag = colorspace.full_range;
      vui.colourDescriptionPresentFlag = 1;
      vui.colourPrimaries = colorspace.primaries;
      vui.transferCharacteristics = colorspace.tranfer_function;
      vui.colourMatrix = colorspace.matrix;
      vui.chromaSampleLocationFlag = buffer_is_yuv444(buffer_format) ? 0 : 1;
      vui.chromaSampleLocationTop = 0;
      vui.chromaSampleLocationBot = 0;
      vui.bitstreamRestrictionFlag = 1;
    };

    if constexpr (requires { format_config.h264VUIParameters; }) {
      configure_vui(format_config.h264VUIParameters);
    } else {
      configure_vui(format_config.hevcVUIParameters);
    }

    if (client_config.enableIntraRefresh != 1) {
      return;
    }
    if (!get_encoder_cap(encode_guid, NV_ENC_CAPS_SUPPORT_INTRA_REFRESH)) {
      BOOST_LOG(error) << "NvEnc: Client asked for intra-refresh but the encoder does not support intra-refresh";
      return;
    }
    format_config.enableIntraRefresh = 1;
    format_config.intraRefreshPeriod = 300;
    format_config.intraRefreshCnt = 299;
    if constexpr (requires { format_config.outputRecoveryPointSEI; }) {
      format_config.outputRecoveryPointSEI = 1;
    }
#if NVENC_SDK_VERSION >= 1200
    if (get_encoder_cap(encode_guid, NV_ENC_CAPS_SINGLE_SLICE_INTRA_REFRESH)) {
      format_config.singleSliceIntraRefresh = 1;
    } else {
      BOOST_LOG(warning) << "NvEnc: Single Slice Intra Refresh not supported";
    }
#endif
  }

  void nvenc_base::configure_h264(
    NV_ENC_CONFIG &enc_config,
    const ::nvenc::nvenc_config &config,
    const video::config_t &client_config,
    const nvenc_colorspace_t &colorspace,
    NV_ENC_BUFFER_FORMAT buffer_format,
    const GUID &encode_guid
  ) {
    enc_config.profileGUID = buffer_is_yuv444(buffer_format) ? NV_ENC_H264_PROFILE_HIGH_444_GUID : NV_ENC_H264_PROFILE_HIGH_GUID;
    auto &format_config = enc_config.encodeCodecConfig.h264Config;
    format_config.repeatSPSPPS = 1;
    format_config.idrPeriod = NVENC_INFINITE_GOPLENGTH;
    format_config.sliceMode = 3;
    format_config.sliceModeData = client_config.slicesPerFrame;
    if (buffer_is_yuv444(buffer_format)) {
      format_config.chromaFormatIDC = 3;
    }
    format_config.enableFillerDataInsertion = config.insert_filler_data;
    format_config.entropyCodingMode = config.h264_cavlc || !get_encoder_cap(encode_guid, NV_ENC_CAPS_SUPPORT_CABAC) ?
                                        NV_ENC_H264_ENTROPY_CODING_MODE_CAVLC :
                                        NV_ENC_H264_ENTROPY_CODING_MODE_CABAC;
    configure_reference_frames(format_config.maxNumRefFrames, format_config.numRefL0, 5, client_config.numRefFrames, encode_guid);

    if (config.enable_min_qp) {
      enc_config.rcParams.enableMinQP = 1;
      enc_config.rcParams.minQP.qpInterP = config.min_qp_h264;
      enc_config.rcParams.minQP.qpIntra = config.min_qp_h264;
      enc_config.rcParams.minQP.qpInterB = config.min_qp_h264;
    }
    if (config.enable_max_qp) {
      enc_config.rcParams.enableMaxQP = 1;
      enc_config.rcParams.maxQP.qpInterP = config.max_qp_h264;
      enc_config.rcParams.maxQP.qpIntra = config.max_qp_h264;
      enc_config.rcParams.maxQP.qpInterB = config.max_qp_h264;
    }

    configure_h264_hevc_metadata(format_config, client_config, colorspace, buffer_format, encode_guid);
  }

  void nvenc_base::configure_hevc(
    NV_ENC_CONFIG &enc_config,
    const ::nvenc::nvenc_config &config,
    const video::config_t &client_config,
    const nvenc_colorspace_t &colorspace,
    NV_ENC_BUFFER_FORMAT buffer_format,
    const GUID &encode_guid
  ) {
    auto &format_config = enc_config.encodeCodecConfig.hevcConfig;
    format_config.repeatSPSPPS = 1;
    format_config.idrPeriod = NVENC_INFINITE_GOPLENGTH;
    format_config.sliceMode = 3;
    format_config.sliceModeData = client_config.slicesPerFrame;
    if (buffer_is_yuv444(buffer_format)) {
      format_config.chromaFormatIDC = 3;
    }
    format_config.enableFillerDataInsertion = config.insert_filler_data;
    if (buffer_is_10bit(buffer_format)) {
#if NVENC_SDK_VERSION >= 1300
      format_config.inputBitDepth = NV_ENC_BIT_DEPTH_10;
      format_config.outputBitDepth = NV_ENC_BIT_DEPTH_10;
#else
      format_config.pixelBitDepthMinus8 = 2;
#endif
    }
    configure_reference_frames(format_config.maxNumRefFramesInDPB, format_config.numRefL0, 5, client_config.numRefFrames, encode_guid);

    if (config.enable_min_qp) {
      enc_config.rcParams.enableMinQP = 1;
      enc_config.rcParams.minQP.qpInterP = config.min_qp_hevc;
      enc_config.rcParams.minQP.qpIntra = config.min_qp_hevc;
      enc_config.rcParams.minQP.qpInterB = config.min_qp_hevc;
    }
    if (config.enable_max_qp) {
      enc_config.rcParams.enableMaxQP = 1;
      enc_config.rcParams.maxQP.qpInterP = config.max_qp_hevc;
      enc_config.rcParams.maxQP.qpIntra = config.max_qp_hevc;
      enc_config.rcParams.maxQP.qpInterB = config.max_qp_hevc;
    }

    configure_h264_hevc_metadata(format_config, client_config, colorspace, buffer_format, encode_guid);
  }

#if NVENC_SDK_VERSION >= 1200
  void nvenc_base::configure_av1(
    NV_ENC_CONFIG &enc_config,
    const ::nvenc::nvenc_config &config,
    const video::config_t &client_config,
    const nvenc_colorspace_t &colorspace,
    NV_ENC_BUFFER_FORMAT buffer_format,
    const GUID &encode_guid
  ) {
    auto &format_config = enc_config.encodeCodecConfig.av1Config;
    format_config.repeatSeqHdr = 1;
    format_config.idrPeriod = NVENC_INFINITE_GOPLENGTH;
    if (buffer_is_yuv444(buffer_format)) {
      format_config.chromaFormatIDC = 3;
    }
    format_config.enableBitstreamPadding = config.insert_filler_data;
    if (buffer_is_10bit(buffer_format)) {
  #if NVENC_SDK_VERSION >= 1300
      format_config.inputBitDepth = NV_ENC_BIT_DEPTH_10;
      format_config.outputBitDepth = NV_ENC_BIT_DEPTH_10;
  #else
      format_config.inputPixelBitDepthMinus8 = 2;
      format_config.pixelBitDepthMinus8 = 2;
  #endif
    }
    format_config.colorPrimaries = colorspace.primaries;
    format_config.transferCharacteristics = colorspace.tranfer_function;
    format_config.matrixCoefficients = colorspace.matrix;
    format_config.colorRange = colorspace.full_range;
    format_config.chromaSamplePosition = buffer_is_yuv444(buffer_format) ? 0 : 1;
    configure_reference_frames(format_config.maxNumRefFramesInDPB, format_config.numFwdRefs, 8, client_config.numRefFrames, encode_guid);

    if (config.enable_min_qp) {
      enc_config.rcParams.enableMinQP = 1;
      enc_config.rcParams.minQP.qpInterP = config.min_qp_av1;
      enc_config.rcParams.minQP.qpIntra = config.min_qp_av1;
      enc_config.rcParams.minQP.qpInterB = config.min_qp_av1;
    }
    if (config.enable_max_qp) {
      enc_config.rcParams.enableMaxQP = 1;
      enc_config.rcParams.maxQP.qpInterP = config.max_qp_av1;
      enc_config.rcParams.maxQP.qpIntra = config.max_qp_av1;
      enc_config.rcParams.maxQP.qpInterB = config.max_qp_av1;
    }
    if (client_config.slicesPerFrame > 1) {
      // NVENC supports power-of-two tile counts, biased toward rows.
      format_config.numTileRows = std::pow(2, std::ceil(std::log2(client_config.slicesPerFrame) / 2));
      format_config.numTileColumns = std::pow(2, std::floor(std::log2(client_config.slicesPerFrame) / 2));
    }
  }
#endif

  void nvenc_base::configure_codec(
    NV_ENC_CONFIG &enc_config,
    const ::nvenc::nvenc_config &config,
    const video::config_t &client_config,
    const nvenc_colorspace_t &colorspace,
    NV_ENC_BUFFER_FORMAT buffer_format,
    const GUID &encode_guid
  ) {
    switch (client_config.videoFormat) {
      case 0:
        configure_h264(enc_config, config, client_config, colorspace, buffer_format, encode_guid);
        break;
      case 1:
        configure_hevc(enc_config, config, client_config, colorspace, buffer_format, encode_guid);
        break;
#if NVENC_SDK_VERSION >= 1200
      case 2:
        configure_av1(enc_config, config, client_config, colorspace, buffer_format, encode_guid);
        break;
#endif
    }
  }

  bool nvenc_base::initialize_encoder_resources(NV_ENC_INITIALIZE_PARAMS &init_params) {
    if (nvenc_failed(nvenc->nvEncInitializeEncoder(encoder, &init_params))) {
      BOOST_LOG(error) << "NvEnc: NvEncInitializeEncoder() failed: " << last_nvenc_error_string;
      return false;
    }

    // Bitstream buffers are paired with input slots by index; the platform layer fills the slots.
    const auto slot_count = ::nvenc::input_surface_count(encoder_params.lookahead_depth);
    slots.resize(slot_count);
    bitstreams.reserve(slot_count);
    for (std::size_t i = 0; i < slot_count; i++) {
      NV_ENC_CREATE_BITSTREAM_BUFFER create_bitstream_buffer = {.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER};
      if (nvenc_failed(nvenc->nvEncCreateBitstreamBuffer(encoder, &create_bitstream_buffer))) {
        BOOST_LOG(error) << "NvEnc: NvEncCreateBitstreamBuffer() failed: " << last_nvenc_error_string;
        return false;
      }
      bitstreams.push_back(create_bitstream_buffer.bitstreamBuffer);
    }

    return create_and_register_input_buffer();
  }

  void nvenc_base::log_created_encoder(
    const NV_ENC_INITIALIZE_PARAMS &init_params,
    const NV_ENC_CONFIG &enc_config,
    const ::nvenc::nvenc_config &config,
    const video::config_t &client_config,
    NV_ENC_BUFFER_FORMAT buffer_format
  ) const {
    std::string extra;
    if (buffer_is_yuv444(buffer_format)) {
      extra += " yuv444";
    }
    if (buffer_is_10bit(buffer_format)) {
      extra += " 10-bit";
    }
    if (enc_config.rcParams.multiPass != NV_ENC_MULTI_PASS_DISABLED) {
      extra += " two-pass";
    }
    if (enc_config.rcParams.vbvBufferSize != 0) {
      const auto bitrate_kbps = ::nvenc::effective_bitrate_kbps(client_config.bitrate, config::video.max_bitrate);
      extra += std::format(
        " vbv={}ms",
        static_cast<int>(std::lround(enc_config.rcParams.vbvBufferSize * 1000.0 / (bitrate_kbps * 1000.0)))
      );
    }
    if (enc_config.rcParams.enableLookahead) {
      extra += std::format(" lookahead={}", enc_config.rcParams.lookaheadDepth);
#if NVENC_SDK_VERSION >= 1300
      if (enc_config.rcParams.lookaheadLevel != NV_ENC_LOOKAHEAD_LEVEL_0) {
        extra += std::format("/{}", static_cast<int>(enc_config.rcParams.lookaheadLevel));
      }
#endif
    }
    if (config.low_delay_key_frame_scale > 0) {
      extra += std::format(" ldkfs={}", config.low_delay_key_frame_scale);
    }
    if (encoder_params.rfi) {
      extra += " rfi";
    }
    if (init_params.enableWeightedPrediction) {
      extra += " weighted-prediction";
    }
    if (enc_config.rcParams.enableAQ) {
      extra += " spatial-aq";
    }
    if (enc_config.rcParams.aqStrength != 0) {
      extra += std::format(" aq-strength={}", enc_config.rcParams.aqStrength);
    }
    if (enc_config.rcParams.enableNonRefP) {
      extra += " nonref-p";
    }
    if (enc_config.rcParams.enableMinQP) {
      extra += std::format(" qpmin={}", enc_config.rcParams.minQP.qpInterP);
    }
    if (enc_config.rcParams.enableMaxQP) {
      extra += std::format(" qpmax={}", enc_config.rcParams.maxQP.qpInterP);
    }
    if (config.insert_filler_data) {
      extra += " filler-data";
    }
#if NVENC_SDK_VERSION >= 1300
    if (client_config.videoFormat > 0 && get_encoder_cap(init_params.encodeGUID, NV_ENC_CAPS_NUM_ENCODER_ENGINES) > 1) {
      if (init_params.splitEncodeMode == NV_ENC_SPLIT_AUTO_MODE) {
        extra += " sfe-auto";
      } else if (init_params.splitEncodeMode == NV_ENC_SPLIT_AUTO_FORCED_MODE) {
        extra += " sfe";
      }
    }
#endif

    BOOST_LOG(info) << "NvEnc: created encoder v" << NVENC_SDK_VERSION << " "
                    << video_format_name(client_config.videoFormat)
                    << quality_preset_string_from_guid(init_params.presetGUID) << extra;
  }

  bool nvenc_base::create_encoder(
    const ::nvenc::nvenc_config &config,
    const video::config_t &client_config,
    const video::sunshine_colorspace_t &sunshine_colorspace,
    platf::pix_fmt_e sunshine_buffer_format
  ) {
    if (!nvenc && !init_library()) {
      return false;
    }
    if (encoder) {
      destroy_encoder();
    }
    auto fail_guard = util::fail_guard([this] {
      destroy_encoder();
    });

    const auto colorspace = nvenc_colorspace_from_sunshine_colorspace(sunshine_colorspace);
    const auto buffer_format = nvenc_format_from_sunshine_format(sunshine_buffer_format);
    if (buffer_format == NV_ENC_BUFFER_FORMAT_UNDEFINED) {
      BOOST_LOG(error) << "NvEnc: unsupported input pixel format";
      return false;
    }
    encoder_params.width = client_config.width;
    encoder_params.height = client_config.height;
    encoder_params.buffer_format = buffer_format;
    encoder_params.rfi = true;

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS session_params = {.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER};
    session_params.device = device;
    session_params.deviceType = device_type;
    session_params.apiVersion = NVENCAPI_VERSION;
    if (nvenc_failed(nvenc->nvEncOpenEncodeSessionEx(&session_params, &encoder))) {
      BOOST_LOG(error) << "NvEnc: NvEncOpenEncodeSessionEx() failed: " << last_nvenc_error_string;
      return false;
    }

    std::uint32_t encode_guid_count = 0;
    if (nvenc_failed(nvenc->nvEncGetEncodeGUIDCount(encoder, &encode_guid_count))) {
      BOOST_LOG(error) << "NvEnc: NvEncGetEncodeGUIDCount() failed: " << last_nvenc_error_string;
      return false;
    }
    std::vector<GUID> encode_guids(encode_guid_count);
    if (nvenc_failed(nvenc->nvEncGetEncodeGUIDs(encoder, encode_guids.data(), static_cast<std::uint32_t>(encode_guids.size()), &encode_guid_count))) {
      BOOST_LOG(error) << "NvEnc: NvEncGetEncodeGUIDs() failed: " << last_nvenc_error_string;
      return false;
    }

    NV_ENC_INITIALIZE_PARAMS init_params = {.version = NV_ENC_INITIALIZE_PARAMS_VER};
    switch (client_config.videoFormat) {
      case 0:
        init_params.encodeGUID = NV_ENC_CODEC_H264_GUID;
        break;
      case 1:
        init_params.encodeGUID = NV_ENC_CODEC_HEVC_GUID;
        break;
#if NVENC_SDK_VERSION >= 1200
      case 2:
        init_params.encodeGUID = NV_ENC_CODEC_AV1_GUID;
        break;
#endif
      default:
        BOOST_LOG(error) << "NvEnc: unknown video format " << client_config.videoFormat;
        return false;
    }
    if (!contains_guid(encode_guids, init_params.encodeGUID)) {
      BOOST_LOG(error) << "NvEnc: encoding format is not supported by the gpu";
      return false;
    }
    if (!validate_encoder_capabilities(init_params.encodeGUID, buffer_format)) {
      return false;
    }

    init_params.presetGUID = quality_preset_guid_from_number(config.quality_preset);
    init_params.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    init_params.enablePTD = 1;
    // Synchronous encoding: each nvEncEncodePicture() call finishes one picture, which lets the
    // slot ring derive ownership and progress from the returned status alone.
    init_params.enableEncodeAsync = 0;
    init_params.enableWeightedPrediction = config.weighted_prediction &&
                                           get_encoder_cap(init_params.encodeGUID, NV_ENC_CAPS_SUPPORT_WEIGHTED_PREDICTION);
    init_params.encodeWidth = encoder_params.width;
    init_params.darWidth = encoder_params.width;
    init_params.encodeHeight = encoder_params.height;
    init_params.darHeight = encoder_params.height;
    const AVRational fps = video::framerate_to_rational(client_config);
    init_params.frameRateNum = fps.num;
    init_params.frameRateDen = fps.den;
    configure_split_frame(init_params, config, client_config);

    NV_ENC_PRESET_CONFIG preset_config = {
      .version = NV_ENC_PRESET_CONFIG_VER,
      .presetCfg = {.version = NV_ENC_CONFIG_VER},
    };
    if (nvenc_failed(nvenc->nvEncGetEncodePresetConfigEx(encoder, init_params.encodeGUID, init_params.presetGUID, init_params.tuningInfo, &preset_config))) {
      BOOST_LOG(error) << "NvEnc: NvEncGetEncodePresetConfigEx() failed: " << last_nvenc_error_string;
      return false;
    }

    NV_ENC_CONFIG enc_config = preset_config.presetCfg;
    enc_config.profileGUID = NV_ENC_CODEC_PROFILE_AUTOSELECT_GUID;
    configure_rate_control(enc_config, config, client_config, init_params.encodeGUID);
    configure_codec(enc_config, config, client_config, colorspace, buffer_format, init_params.encodeGUID);
    init_params.encodeConfig = &enc_config;

    // Frames inside the lookahead window are already encoded, so invalidation cannot repair them.
    if (encoder_params.lookahead_depth > 0) {
      encoder_params.rfi = false;
    }

    if (!initialize_encoder_resources(init_params)) {
      return false;
    }

    auto frame_size_format = stat_trackers::two_digits_after_decimal();
    const auto bitrate_kbps = ::nvenc::effective_bitrate_kbps(client_config.bitrate, config::video.max_bitrate);
    BOOST_LOG(debug) << "NvEnc: requested encoded frame size "
                     << frame_size_format % (bitrate_kbps / 8. / client_config.framerate) << " kB";
    log_created_encoder(init_params, enc_config, config, client_config, buffer_format);

    encoder_state = {};
    fail_guard.disable();
    return true;
  }

  void nvenc_base::destroy_encoder() {
    if (encoder) {
      // Pictures still buffered by the rate control lookahead keep their input resources mapped and
      // their bitstreams in use. Drain them first, otherwise the driver tears down objects it still
      // references and crashes.
      std::vector<::nvenc::nvenc_encoded_frame> discarded;
      flush(discarded);
    }

    for (auto &slot : slots) {
      if (slot.mapped && encoder) {
        if (nvenc_failed(nvenc->nvEncUnmapInputResource(encoder, slot.mapped))) {
          BOOST_LOG(error) << "NvEnc: NvEncUnmapInputResource() failed: " << last_nvenc_error_string;
        }
      }
      slot.mapped = nullptr;
    }

    for (auto bitstream : bitstreams) {
      if (bitstream && encoder) {
        if (nvenc_failed(nvenc->nvEncDestroyBitstreamBuffer(encoder, bitstream))) {
          BOOST_LOG(error) << "NvEnc: NvEncDestroyBitstreamBuffer() failed: " << last_nvenc_error_string;
        }
      }
    }
    bitstreams.clear();

    for (auto &slot : slots) {
      if (slot.registered && encoder) {
        if (nvenc_failed(nvenc->nvEncUnregisterResource(encoder, slot.registered))) {
          BOOST_LOG(error) << "NvEnc: NvEncUnregisterResource() failed: " << last_nvenc_error_string;
        }
      }
      slot.registered = nullptr;
    }
    slots.clear();

    if (encoder) {
      if (nvenc_failed(nvenc->nvEncDestroyEncoder(encoder))) {
        BOOST_LOG(error) << "NvEnc: NvEncDestroyEncoder() failed: " << last_nvenc_error_string;
      }
      encoder = nullptr;
    }

    submit_pos = 0;
    reap_pos = 0;
    current_input_surface = 0;
    pending = 0;
    encoder_state = {};
    encoder_params = {};
  }

  bool nvenc_base::acquire_input_surface() {
    if (!encoder) {
      return false;
    }

    // The ring only runs full when the driver stopped handing pictures back.
    if (pending_count() >= slots.size()) {
      BOOST_LOG(error) << "NvEnc: all " << slots.size() << " input surfaces are still owned by the driver";
      return false;
    }

    current_input_surface = submit_pos;
    return true;
  }

  bool nvenc_base::reap_one(std::vector<::nvenc::nvenc_encoded_frame> &out_frames) {
    auto &slot = slots[reap_pos];

    NV_ENC_LOCK_BITSTREAM lock_bitstream = {.version = NV_ENC_LOCK_BITSTREAM_VER};
    lock_bitstream.outputBitstream = bitstreams[reap_pos];
    // A non-blocking lock can return before the picture is encoded.
    lock_bitstream.doNotWait = 0;

    if (nvenc_failed(nvenc->nvEncLockBitstream(encoder, &lock_bitstream))) {
      BOOST_LOG(error) << "NvEnc: NvEncLockBitstream() failed: " << last_nvenc_error_string;
      return false;
    }

    ::nvenc::nvenc_encoded_frame encoded_frame;
    auto data_pointer = (uint8_t *) lock_bitstream.bitstreamBufferPtr;
    encoded_frame.data = {data_pointer, data_pointer + lock_bitstream.bitstreamSizeInBytes};
    encoded_frame.frame_index = lock_bitstream.outputTimeStamp;
    encoded_frame.idr = lock_bitstream.pictureType == NV_ENC_PIC_TYPE_IDR;
    encoded_frame.after_ref_frame_invalidation = encoder_state.rfi_needs_confirmation;
    encoder_state.rfi_needs_confirmation = false;

    if (encoded_frame.frame_index != slot.frame_index) {
      BOOST_LOG(warning) << "NvEnc: frame index mismatch " << slot.frame_index << " " << encoded_frame.frame_index;
    }

    encoder_state.last_encoded_frame_index = encoded_frame.frame_index;
    encoder_state.completed_frame_count++;
    encoder_state.frame_size_logger.collect_and_log(encoded_frame.data.size() / 1000.);

    if (encoded_frame.idr) {
      BOOST_LOG(debug) << "NvEnc: idr frame " << encoded_frame.frame_index;
    }

    if (nvenc_failed(nvenc->nvEncUnlockBitstream(encoder, lock_bitstream.outputBitstream))) {
      BOOST_LOG(error) << "NvEnc: NvEncUnlockBitstream() failed: " << last_nvenc_error_string;
    }

    if (slot.mapped && nvenc_failed(nvenc->nvEncUnmapInputResource(encoder, slot.mapped))) {
      BOOST_LOG(error) << "NvEnc: NvEncUnmapInputResource() failed: " << last_nvenc_error_string;
    }
    slot.mapped = nullptr;

    pending--;
    reap_pos = next_slot(reap_pos);
    out_frames.push_back(std::move(encoded_frame));
    return true;
  }

  bool nvenc_base::encode_frame(uint64_t frame_index, bool force_idr, bool repeat_last_frame, std::vector<::nvenc::nvenc_encoded_frame> &out_frames) {
    if (!encoder) {
      return false;
    }

    // The duplicated frame must not reuse a surface the driver still reads.
    if (repeat_last_frame && pending_count() > 0) {
      const auto previous_surface = current_input_surface;
      if (!acquire_input_surface()) {
        return false;
      }
      if (!copy_input_surface(previous_surface, current_input_surface)) {
        BOOST_LOG(error) << "NvEnc: failed to duplicate the repeated frame into a free input surface";
        return false;
      }
    }

    auto &slot = slots[current_input_surface];

    if (!synchronize_input_buffer()) {
      BOOST_LOG(error) << "NvEnc: failed to synchronize input buffer";
      return false;
    }

    NV_ENC_MAP_INPUT_RESOURCE mapped_input_buffer = {.version = NV_ENC_MAP_INPUT_RESOURCE_VER};
    mapped_input_buffer.registeredResource = slot.registered;

    if (nvenc_failed(nvenc->nvEncMapInputResource(encoder, &mapped_input_buffer))) {
      BOOST_LOG(error) << "NvEnc: NvEncMapInputResource() failed: " << last_nvenc_error_string;
      return false;
    }
    slot.mapped = mapped_input_buffer.mappedResource;

    NV_ENC_PIC_PARAMS pic_params = {.version = NV_ENC_PIC_PARAMS_VER};
    pic_params.inputWidth = encoder_params.width;
    pic_params.inputHeight = encoder_params.height;
    pic_params.encodePicFlags = force_idr ? NV_ENC_PIC_FLAG_FORCEIDR : 0;
    // The SDK requires a frame index that increases monotonically from zero over the session lifetime
    pic_params.frameIdx = encoder_state.submitted_frame_count;
    pic_params.inputTimeStamp = frame_index;
    pic_params.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    pic_params.inputBuffer = mapped_input_buffer.mappedResource;
    pic_params.bufferFmt = mapped_input_buffer.mappedBufferFmt;
    pic_params.outputBitstream = bitstreams[current_input_surface];

    const auto status = nvenc->nvEncEncodePicture(encoder, &pic_params);
    if (status != NV_ENC_SUCCESS && status != NV_ENC_ERR_NEED_MORE_INPUT) {
      nvenc_failed(status);
      BOOST_LOG(error) << "NvEnc: NvEncEncodePicture() failed: " << last_nvenc_error_string;
      if (nvenc_failed(nvenc->nvEncUnmapInputResource(encoder, mapped_input_buffer.mappedResource))) {
        BOOST_LOG(error) << "NvEnc: NvEncUnmapInputResource() failed: " << last_nvenc_error_string;
      }
      slot.mapped = nullptr;
      return false;
    }

    slot.frame_index = frame_index;
    encoder_state.submitted_frame_count++;
    submit_pos = next_slot(current_input_surface);
    pending++;

    if (status == NV_ENC_ERR_NEED_MORE_INPUT) {
      // The picture is buffered by the driver while the lookahead window fills up.
      return true;
    }

    // Without B-frames the oldest submitted picture is the one that finished.
    return reap_one(out_frames);
  }

  bool nvenc_base::flush(std::vector<::nvenc::nvenc_encoded_frame> &out_frames) {
    // Every outstanding picture owns a slot, so an empty ring already delivered everything.
    if (!encoder || pending_count() == 0) {
      return true;
    }

    NV_ENC_PIC_PARAMS pic_params = {.version = NV_ENC_PIC_PARAMS_VER};
    pic_params.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
    pic_params.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;

    const auto status = nvenc->nvEncEncodePicture(encoder, &pic_params);
    if (status != NV_ENC_SUCCESS && status != NV_ENC_ERR_NEED_MORE_INPUT) {
      if (nvenc_failed(status)) {
        BOOST_LOG(error) << "NvEnc: NvEncEncodePicture() failed on end of stream: " << last_nvenc_error_string;
        return false;
      }
    }

    // EOS encodes everything the lookahead still buffered, so the remaining locks complete at once.
    while (pending_count() > 0) {
      if (!reap_one(out_frames)) {
        return false;
      }
    }

    return true;
  }

  bool nvenc_base::invalidate_ref_frames(uint64_t first_frame, uint64_t last_frame) {
    if (!encoder || !encoder_params.rfi) {
      return false;
    }

    if (first_frame >= encoder_state.last_rfi_range.first && last_frame <= encoder_state.last_rfi_range.second) {
      BOOST_LOG(debug) << "NvEnc: rfi request " << first_frame << "-" << last_frame << " already done";
      return true;
    }

    if (last_frame < first_frame) {
      BOOST_LOG(error) << "NvEnc: invaid rfi request " << first_frame << "-" << last_frame << ", generating IDR";
      return false;
    }

    if (encoder_state.completed_frame_count == 0) {
      BOOST_LOG(debug) << "NvEnc: rfi request " << first_frame << "-" << last_frame << " before any frame was encoded, generating IDR";
      return false;
    }

    BOOST_LOG(debug) << "NvEnc: rfi request " << first_frame << "-" << last_frame << " expanding to last encoded frame " << encoder_state.last_encoded_frame_index;
    last_frame = encoder_state.last_encoded_frame_index;

    if (last_frame - first_frame + 1 >= encoder_params.ref_frames_in_dpb) {
      BOOST_LOG(debug) << "NvEnc: rfi request too large, generating IDR";
      return false;
    }

    for (auto i = first_frame; i <= last_frame; i++) {
      if (nvenc_failed(nvenc->nvEncInvalidateRefFrames(encoder, i))) {
        BOOST_LOG(error) << "NvEnc: NvEncInvalidateRefFrames() " << i << " failed: " << last_nvenc_error_string;
        return false;
      }
    }

    // Record the range and mark the request only once it was applied, otherwise the next frame would
    // be announced to the client as a recovery point without repairing the damaged references.
    encoder_state.last_rfi_range = {first_frame, last_frame};
    encoder_state.rfi_needs_confirmation = true;

    return true;
  }

  int nvenc_base::lookahead_depth() const {
    return encoder_params.lookahead_depth;
  }

  bool nvenc_base::nvenc_failed(NVENCSTATUS status) {
    last_nvenc_error_string.clear();
    if (status != NV_ENC_SUCCESS) {
      /* This API function gives broken strings more often than not
      if (nvenc && encoder) {
        last_nvenc_error_string = nvenc->nvEncGetLastErrorString(encoder);
        if (!last_nvenc_error_string.empty()) last_nvenc_error_string += " ";
      }
      */
      last_nvenc_error_string += nvenc_status_string(status);
      return true;
    }

    return false;
  }

}  // namespace NVENC_NAMESPACE
