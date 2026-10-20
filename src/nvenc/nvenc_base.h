/**
 * @file src/nvenc/nvenc_base.h
 * @brief Declarations for abstract platform-agnostic base of standalone NVENC encoder.
 */
#pragma once

// standard includes
#include <cstdint>
#include <utility>
#include <vector>

// local includes
#include "nvenc_colorspace.h"
#include "nvenc_config.h"
#include "nvenc_encoded_frame.h"
#include "nvenc_encoder.h"
#include "nvenc_sdk.h"
#include "src/logging.h"
#include "src/video.h"
#include "src/video_colorspace.h"

/**
 * @brief Standalone NVENC encoder
 */
namespace NVENC_NAMESPACE {

  /**
   * @brief Abstract platform-agnostic base of standalone NVENC encoder.
   *        Derived classes perform platform-specific operations.
   */
  // Virtual inheritance is required because platform implementations also inherit their SDK-neutral interface.
  class nvenc_base: public virtual ::nvenc::nvenc_encoder {  // NOSONAR(cpp:S1011)
  public:
    /**
     * @param device_type Underlying device type used by derived class.
     */
    explicit nvenc_base(NV_ENC_DEVICE_TYPE device_type);
    ~nvenc_base() override;

    nvenc_base(const nvenc_base &) = delete;
    nvenc_base &operator=(const nvenc_base &) = delete;

    /**
     * @brief Create the encoder.
     * @param config NVENC encoder configuration.
     * @param client_config Stream configuration requested by the client.
     * @param colorspace YUV colorspace.
     * @param buffer_format Platform-agnostic input surface format.
     * @return `true` on success, `false` on error
     */
    bool create_encoder(
      const ::nvenc::nvenc_config &config,
      const video::config_t &client_config,
      const video::sunshine_colorspace_t &colorspace,
      platf::pix_fmt_e buffer_format
    ) override;

    /**
     * @brief Destroy the encoder.
     *        Derived classes classes call it in the destructor.
     */
    void destroy_encoder() override;

    /**
     * @brief Encode the next frame using platform-specific input surface.
     * @param frame_index Frame index that uniquely identifies the frame.
     *        Afterwards serves as parameter for `invalidate_ref_frames()`.
     *        No restrictions on the first frame index, but later frame indexes must be subsequent.
     * @param force_idr Whether to encode frame as forced IDR.
     * @param repeat_last_frame Whether the input surface still holds the previously submitted frame.
     *        The driver may keep reading that surface, so the frame has to be copied into a fresh
     *        surface before it can be submitted again.
     * @param out_frames List that receives the frame that became available during this call.
     *        Stays empty while the rate control lookahead window is being filled.
     * @return `true` on success, `false` on error.
     */
    bool encode_frame(uint64_t frame_index, bool force_idr, bool repeat_last_frame, std::vector<::nvenc::nvenc_encoded_frame> &out_frames) override;

    /**
     * @brief Signal the end of the input stream and collect the frames still buffered internally.
     * @param out_frames List that receives every frame that became available during this call.
     * @return `true` on success, `false` on error.
     */
    bool flush(std::vector<::nvenc::nvenc_encoded_frame> &out_frames) override;

    /**
     * @brief Perform reference frame invalidation (RFI) procedure.
     * @param first_frame First frame index of the invalidation range.
     * @param last_frame Last frame index of the invalidation range.
     * @return `true` on success, `false` on error.
     *         After error next frame must be encoded with `force_idr = true`.
     */
    bool invalidate_ref_frames(uint64_t first_frame, uint64_t last_frame) override;

    /**
     * @brief Get the rate control lookahead depth the driver was initialized with.
     * @return Effective lookahead depth, zero when lookahead is disabled or unsupported.
     */
    int lookahead_depth() const override;

  protected:
    /**
     * @brief Required. Used for loading NvEnc library and setting `nvenc` variable with `NvEncodeAPICreateInstance()`.
     *        Called during `create_encoder()` if `nvenc` variable is not initialized.
     * @return `true` on success, `false` on error
     */
    virtual bool init_library() = 0;

    /**
     * @brief Required. Used for creating outside-facing input surfaces,
     *        registering each surface with `nvenc->nvEncRegisterResource()` and storing the
     *        registration handles in `slots`.
     *        Called during `create_encoder()`, must create one surface per `slots` entry.
     * @return `true` on success, `false` on error
     */
    virtual bool create_and_register_input_buffer() = 0;

    /**
     * @brief Pick the slot that the next captured frame is converted into and submitted from.
     *
     * @return `true` when a free slot was selected, `false` when the driver stopped producing output.
     */
    bool acquire_input_surface();

    /**
     * @brief Optional. Override if you must perform additional operations on the registered input surface in the beginning of `encode_frame()`.
     *        Typically used for interop copy.
     * @return `true` on success, `false` on error
     */
    virtual bool synchronize_input_buffer() {
      return true;
    }

    /**
     * @brief Required. Duplicate one input surface into another.
     *
     * @param from Index of the surface that holds the frame to duplicate.
     * @param to Index of the surface that receives the copy.
     * @return `true` on success, `false` on error.
     */
    virtual bool copy_input_surface(std::size_t from, std::size_t to) = 0;

    /**
     * @brief Check whether an NVENC API status represents failure.
     *
     * @param status Native status code returned by the platform API.
     * @return True when the status is an NVENC error code.
     */
    bool nvenc_failed(NVENCSTATUS status);

    const NV_ENC_DEVICE_TYPE device_type;  ///< NVENC device backend used by this encoder instance.

    void *encoder = nullptr;  ///< Opaque NVENC encoder session handle returned by the driver.

    struct {
      uint32_t width = 0;
      uint32_t height = 0;
      NV_ENC_BUFFER_FORMAT buffer_format = NV_ENC_BUFFER_FORMAT_UNDEFINED;
      uint32_t ref_frames_in_dpb = 0;
      int lookahead_depth = 0;
      bool rfi = false;
    } encoder_params;  ///< Current encoder dimensions, pixel format, and reference-frame settings.

    std::string last_nvenc_error_string;  ///< Last NVENC error string.

    // Derived classes set these variables
    void *device = nullptr;  ///< Platform-specific handle of encoding device.
                             ///< Should be set in constructor or `init_library()`.
    std::shared_ptr<NV_ENCODE_API_FUNCTION_LIST> nvenc;  ///< Function pointers list produced by `NvEncodeAPICreateInstance()`.
                                                         ///< Should be set in `init_library()`.

    /**
     * @brief One input surface and its place in the submission ring.
     *
     * The driver owns a slot from `nvEncEncodePicture()` until its bitstream is retrieved, which
     * is exactly the range between `reap_pos` and `submit_pos`.
     */
    struct input_slot_t {
      NV_ENC_REGISTERED_PTR registered = nullptr;  ///< Surface registered with `NvEncRegisterResource()`.
      NV_ENC_INPUT_PTR mapped = nullptr;  ///< Mapping of the submitted picture, cleared when it is retrieved.
      std::uint64_t frame_index = 0;  ///< Frame index of the submitted picture, used to verify retrieval order.
    };

    std::vector<input_slot_t> slots;  ///< Input surfaces and their in-flight state, in ring order.
    std::vector<NV_ENC_OUTPUT_PTR> bitstreams;  ///< Bitstream buffers paired with `slots` by index.

    std::size_t submit_pos = 0;  ///< Index of the next slot handed out for conversion and submission.
    std::size_t reap_pos = 0;  ///< Index of the oldest submitted picture whose bitstream was not retrieved yet.
    std::size_t current_input_surface = 0;  ///< Slot handed out by `acquire_input_surface()` and not submitted yet.
    std::size_t pending = 0;  ///< Number of submitted pictures whose bitstream was not retrieved yet.

    /**
     * @brief Number of pictures held by the driver.
     * @return Number of submitted but not yet retrieved slots.
     */
    std::size_t pending_count() const {
      return pending;
    }

    /**
     * @brief Advance a ring index by one slot.
     * @param index Current ring index.
     * @return Next ring index.
     */
    std::size_t next_slot(std::size_t index) const {
      return (index + 1) % slots.size();
    }

  private:
    /**
     * @brief Retrieve the oldest submitted picture and free its slot.
     *
     * Blocks until the driver finished the picture. Only called when the caller knows that a
     * picture became available, so the blocking lock normally returns immediately.
     *
     * @param out_frames List that receives the retrieved frame.
     * @return `true` on success, `false` on error.
     */
    bool reap_one(std::vector<::nvenc::nvenc_encoded_frame> &out_frames);

    /**
     * @brief Query one encoder capability.
     *
     * @param encode_guid Codec GUID to query.
     * @param cap Capability identifier.
     * @return Capability value, or zero when the query fails.
     */
    int get_encoder_cap(const GUID &encode_guid, NV_ENC_CAPS cap) const;

    /**
     * @brief Validate the requested input format and dimensions against encoder capabilities.
     *
     * @param encode_guid Selected codec GUID.
     * @param buffer_format Selected NVENC input format.
     * @return `true` when the request is supported, otherwise `false`.
     */
    bool validate_encoder_capabilities(const GUID &encode_guid, NV_ENC_BUFFER_FORMAT buffer_format);

    /**
     * @brief Configure split-frame encoding for the selected SDK.
     *
     * @param init_params Encoder initialization parameters to update.
     * @param config NVENC encoder configuration.
     * @param client_config Stream configuration requested by the client.
     */
    void configure_split_frame(
      NV_ENC_INITIALIZE_PARAMS &init_params,
      const ::nvenc::nvenc_config &config,
      const video::config_t &client_config
    ) const;

    /**
     * @brief Configure rate control and VBV options.
     *
     * @param enc_config Encoder configuration to update.
     * @param config NVENC encoder configuration.
     * @param client_config Stream configuration requested by the client.
     * @param encode_guid Selected codec GUID.
     */
    void configure_rate_control(
      NV_ENC_CONFIG &enc_config,
      const ::nvenc::nvenc_config &config,
      const video::config_t &client_config,
      const GUID &encode_guid
    );

    /**
     * @brief Configure the rate control lookahead window.
     *
     * @param enc_config Encoder configuration to update.
     * @param config NVENC encoder configuration.
     * @param encode_guid Selected codec GUID.
     */
    void configure_lookahead(
      NV_ENC_CONFIG &enc_config,
      const ::nvenc::nvenc_config &config,
      const GUID &encode_guid
    );

    /**
     * @brief Configure the requested reference-frame count.
     *
     * @param ref_frames_option Codec-specific reference-frame option.
     * @param list0_option Codec-specific list-zero option.
     * @param default_count Default reference-frame count.
     * @param requested_count Client-requested reference-frame count.
     * @param encode_guid Selected codec GUID.
     */
    void configure_reference_frames(
      std::uint32_t &ref_frames_option,
      NV_ENC_NUM_REF_FRAMES &list0_option,
      std::uint32_t default_count,
      int requested_count,
      const GUID &encode_guid
    );

    /**
     * @brief Configure VUI metadata and intra-refresh options shared by H.264 and HEVC.
     *
     * @tparam FormatConfig Codec-specific NVENC configuration type.
     * @param format_config Codec-specific encoder configuration to update.
     * @param client_config Stream configuration requested by the client.
     * @param colorspace NVENC colorspace metadata.
     * @param buffer_format Selected NVENC input format.
     * @param encode_guid Selected codec GUID.
     */
    template<typename FormatConfig>
    void configure_h264_hevc_metadata(
      FormatConfig &format_config,
      const video::config_t &client_config,
      const nvenc_colorspace_t &colorspace,
      NV_ENC_BUFFER_FORMAT buffer_format,
      const GUID &encode_guid
    );

    /**
     * @brief Configure H.264 codec options.
     *
     * @param enc_config Encoder configuration to update.
     * @param config NVENC encoder configuration.
     * @param client_config Stream configuration requested by the client.
     * @param colorspace NVENC colorspace metadata.
     * @param buffer_format Selected NVENC input format.
     * @param encode_guid Selected codec GUID.
     */
    void configure_h264(
      NV_ENC_CONFIG &enc_config,
      const ::nvenc::nvenc_config &config,
      const video::config_t &client_config,
      const nvenc_colorspace_t &colorspace,
      NV_ENC_BUFFER_FORMAT buffer_format,
      const GUID &encode_guid
    );

    /**
     * @brief Configure HEVC codec options.
     *
     * @param enc_config Encoder configuration to update.
     * @param config NVENC encoder configuration.
     * @param client_config Stream configuration requested by the client.
     * @param colorspace NVENC colorspace metadata.
     * @param buffer_format Selected NVENC input format.
     * @param encode_guid Selected codec GUID.
     */
    void configure_hevc(
      NV_ENC_CONFIG &enc_config,
      const ::nvenc::nvenc_config &config,
      const video::config_t &client_config,
      const nvenc_colorspace_t &colorspace,
      NV_ENC_BUFFER_FORMAT buffer_format,
      const GUID &encode_guid
    );

#if NVENC_SDK_VERSION >= 1200
    /**
     * @brief Configure AV1 codec options.
     *
     * @param enc_config Encoder configuration to update.
     * @param config NVENC encoder configuration.
     * @param client_config Stream configuration requested by the client.
     * @param colorspace NVENC colorspace metadata.
     * @param buffer_format Selected NVENC input format.
     * @param encode_guid Selected codec GUID.
     */
    void configure_av1(
      NV_ENC_CONFIG &enc_config,
      const ::nvenc::nvenc_config &config,
      const video::config_t &client_config,
      const nvenc_colorspace_t &colorspace,
      NV_ENC_BUFFER_FORMAT buffer_format,
      const GUID &encode_guid
    );
#endif

    /**
     * @brief Configure codec-specific encoder options.
     *
     * @param enc_config Encoder configuration to update.
     * @param config NVENC encoder configuration.
     * @param client_config Stream configuration requested by the client.
     * @param colorspace NVENC colorspace metadata.
     * @param buffer_format Selected NVENC input format.
     * @param encode_guid Selected codec GUID.
     */
    void configure_codec(
      NV_ENC_CONFIG &enc_config,
      const ::nvenc::nvenc_config &config,
      const video::config_t &client_config,
      const nvenc_colorspace_t &colorspace,
      NV_ENC_BUFFER_FORMAT buffer_format,
      const GUID &encode_guid
    );

    /**
     * @brief Initialize the encoder and its registered input and output resources.
     *
     * @param init_params Completed encoder initialization parameters.
     * @return `true` on success, otherwise `false`.
     */
    bool initialize_encoder_resources(NV_ENC_INITIALIZE_PARAMS &init_params);

    /**
     * @brief Log the selected encoder configuration.
     *
     * @param init_params Encoder initialization parameters.
     * @param enc_config Encoder configuration.
     * @param config NVENC encoder configuration.
     * @param client_config Stream configuration requested by the client.
     * @param buffer_format Selected NVENC input format.
     */
    void log_created_encoder(
      const NV_ENC_INITIALIZE_PARAMS &init_params,
      const NV_ENC_CONFIG &enc_config,
      const ::nvenc::nvenc_config &config,
      const video::config_t &client_config,
      NV_ENC_BUFFER_FORMAT buffer_format
    ) const;

    struct {
      uint64_t last_encoded_frame_index = 0;  ///< Index of the most recently completed frame, used for RFI.
      std::uint64_t completed_frame_count = 0;  ///< Number of frames whose bitstream has been retrieved.
      std::uint32_t submitted_frame_count = 0;  ///< Number of pictures submitted to the driver, used as `frameIdx`.
      bool rfi_needs_confirmation = false;  ///< Whether the next retrieved frame must be flagged as post-RFI.
      std::pair<uint64_t, uint64_t> last_rfi_range;  ///< Last fulfilled RFI range.
      logging::min_max_avg_periodic_logger<double> frame_size_logger = {debug, "NvEnc: encoded frame sizes in kB", ""};  ///< Frame size statistics.
    } encoder_state;
  };

}  // namespace NVENC_NAMESPACE
