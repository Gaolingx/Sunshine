/**
 * @file src/nvenc/nvenc_d3d11_interface.h
 * @brief Declarations for the SDK-neutral Direct3D11 NVENC interface.
 */
#pragma once
#ifdef _WIN32

  // standard includes
  #include <cstddef>

  // lib includes
  #include <d3d11.h>

  // local includes
  #include "nvenc_encoder.h"

namespace nvenc {

  /**
   * @brief SDK-neutral Direct3D11 NVENC encoder interface.
   */
  // Virtual inheritance is required because concrete encoders also inherit their SDK-specific implementation.
  class nvenc_d3d11_interface: public virtual nvenc_encoder {  // NOSONAR(cpp:S1011)
  public:
    /**
     * @brief Destroy the SDK-neutral Direct3D11 encoder interface.
     */
    ~nvenc_d3d11_interface() override = default;

    /**
     * @brief Number of input textures the encoder rotates through.
     *
     * @return Number of textures returned by `get_input_texture()`.
     */
    virtual std::size_t get_input_texture_count() const = 0;

    /**
     * @brief Get the texture of one input surface.
     *
     * @param index Input surface index, smaller than `get_input_texture_count()`.
     * @return Input surface texture.
     */
    virtual ID3D11Texture2D *get_input_texture(std::size_t index) = 0;

    /**
     * @brief Reserve the next input surface and return the texture that must be converted into it.
     *
     * The returned texture stays owned by the encoder until the frame it receives has been encoded.
     *
     * @return Texture to convert the next frame into, or `nullptr` when no surface is available.
     */
    virtual ID3D11Texture2D *acquire_input_texture() = 0;
  };

}  // namespace nvenc
#endif
