/**
 * @file src/nvenc/nvenc_base.h
 * @brief Declarations for abstract platform-agnostic base of standalone NVENC encoder.
 */
#pragma once

// standard includes
#include <cstdint>
#include <deque>
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
     * @param out_frames List that receives every frame that became available during this call.
     *        May stay empty while the rate control lookahead window is being filled.
     * @return `true` on success, `false` on error.
     */
    bool encode_frame(uint64_t frame_index, bool force_idr, std::vector<::nvenc::nvenc_encoded_frame> &out_frames) override;

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

  protected:
    /**
     * @brief Required. Used for loading NvEnc library and setting `nvenc` variable with `NvEncodeAPICreateInstance()`.
     *        Called during `create_encoder()` if `nvenc` variable is not initialized.
     * @return `true` on success, `false` on error
     */
    virtual bool init_library() = 0;

    /**
     * @brief Required. Used for creating outside-facing input surfaces,
     *        registering each surface with `nvenc->nvEncRegisterResource()` and appending the registration
     *        handles to `registered_input_buffers`.
     *        Called during `create_encoder()`, must create `input_surface_count` surfaces.
     * @return `true` on success, `false` on error
     */
    virtual bool create_and_register_input_buffer() = 0;

    /**
     * @brief Pick the input surface that the next captured frame is converted into and submitted from.
     *
     * Derived classes call this before writing into their platform-specific surface storage.
     * Surfaces are used as a ring buffer because the driver keeps lookahead frames alive until
     * their bitstream has been retrieved.
     *
     * @return `true` when a free surface was selected, `false` on error.
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
     * @brief Optional. Override when one input surface can be duplicated into another.
     *
     * The video pipeline repeats the last frame when capture delivers slower than the encoder runs,
     * but the driver keeps reading the surface of the previous frame while the lookahead window
     * is filled, so the content has to be copied into a fresh surface.
     *
     * @param from Index of the surface that holds the frame to duplicate.
     * @param to Index of the surface that receives the copy.
     * @return `true` on success, `false` on error or when duplication is unsupported.
     */
    virtual bool copy_input_surface(std::size_t from, std::size_t to) {
      return false;
    }

    /**
     * @brief Optional. Override if you want to create encoder in async mode.
     *        In this case must also set `async_event_handle` variable.
     * @param timeout_ms Wait timeout in milliseconds
     * @return `true` on success, `false` on timeout or error
     */
    virtual bool wait_for_async_event(uint32_t timeout_ms) {
      return false;
    }

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
      bool rfi = false;
    } encoder_params;  ///< Current encoder dimensions, pixel format, and reference-frame settings.

    std::string last_nvenc_error_string;  ///< Last NVENC error string.

    // Derived classes set these variables
    void *device = nullptr;  ///< Platform-specific handle of encoding device.
                             ///< Should be set in constructor or `init_library()`.
    std::shared_ptr<NV_ENCODE_API_FUNCTION_LIST> nvenc;  ///< Function pointers list produced by `NvEncodeAPICreateInstance()`.
                                                         ///< Should be set in `init_library()`.
    std::vector<NV_ENC_REGISTERED_PTR> registered_input_buffers;  ///< Platform-specific input surfaces registered with `NvEncRegisterResource()`.
                                                                  ///< Should be filled in `create_and_register_input_buffer()`.
    std::size_t input_surface_count = 1;  ///< Number of input surfaces the encoder rotates through.
    std::size_t current_input_surface = 0;  ///< Index of the input surface that is currently being filled and submitted.
    bool input_surface_filled = false;  ///< Whether the current input surface received a converted frame since the last submission.
    void *async_event_handle = nullptr;  ///< (optional) Platform-specific handle of event object event.
                                         ///< Can be set in constructor or `init_library()`, must override `wait_for_async_event()`.

  private:
    /**
     * @brief One submitted picture that the driver may not have finished encoding yet.
     */
    struct submitted_frame {
      std::uint64_t frame_index = 0;  ///< Frame index of the submitted picture.
      bool idr = false;  ///< Whether the picture was submitted as a forced IDR frame.
      NV_ENC_INPUT_PTR mapped_input_buffer = nullptr;  ///< Mapped input resource, unmapped when the bitstream is retrieved.
    };

    /**
     * @brief Retrieve the bitstreams of pictures the driver finished encoding.
     *
     * @param ready_count Number of finished pictures, the oldest submissions are retrieved first.
     * @param out_frames List that receives the retrieved frames, in submission order.
     * @param wait_for_completion_event Whether to wait for the async completion event first.
     * @return `true` on success, `false` on error.
     */
    bool reap_output_frames(std::size_t ready_count, std::vector<::nvenc::nvenc_encoded_frame> &out_frames, bool wait_for_completion_event = true);

    /**
     * @brief Retrieve and unlock a single pending bitstream.
     *
     * @param bitstream Bitstream buffer to retrieve.
     * @param frame Receives the encoded payload and frame metadata.
     * @return `true` on success, `false` on error.
     */
    bool retrieve_bitstream(NV_ENC_OUTPUT_PTR bitstream, ::nvenc::nvenc_encoded_frame &frame);

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
     * @param video_format Requested video format, `0` is H.264, `1` is HEVC and `2` is AV1.
     */
    void configure_lookahead(
      NV_ENC_CONFIG &enc_config,
      const ::nvenc::nvenc_config &config,
      const GUID &encode_guid,
      int video_format
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

    std::vector<NV_ENC_OUTPUT_PTR> output_bitstreams;  ///< Bitstream buffers used for the pictures that are currently in flight.

    std::size_t next_input_surface = 0;  ///< Index of the input surface that will be submitted next.
    std::deque<std::size_t> in_flight_input_surfaces;  ///< Input surfaces that the driver may still read from.
    std::deque<NV_ENC_OUTPUT_PTR> free_output_bitstreams;  ///< Bitstream buffers that can be reused right away.
    std::deque<NV_ENC_OUTPUT_PTR> pending_output_bitstreams;  ///< Bitstream buffers of pictures that are not ready yet.
    std::deque<submitted_frame> submitted_frames;  ///< Metadata of the submitted pictures, in submission order.

    struct {
      uint64_t last_encoded_frame_index = 0;  ///< Index of the most recently completed frame, used for RFI.
      std::uint64_t completed_frame_count = 0;  ///< Number of frames whose bitstream has been retrieved.
      bool rfi_needs_confirmation = false;  ///< Whether the next retrieved frame must be flagged as post-RFI.
      std::pair<uint64_t, uint64_t> last_rfi_range;  ///< Last fulfilled RFI range.
      logging::min_max_avg_periodic_logger<double> frame_size_logger = {debug, "NvEnc: encoded frame sizes in kB", ""};  ///< Frame size statistics.
    } encoder_state;
  };

}  // namespace NVENC_NAMESPACE
