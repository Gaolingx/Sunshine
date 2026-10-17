/**
 * @file src/nvenc/nvenc_d3d11_native.h
 * @brief Declarations for native Direct3D11 NVENC encoder.
 */
#pragma once
#ifdef _WIN32
  // standard includes
  #include <comdef.h>
  #include <d3d11.h>
  #include <vector>

  // local includes
  #include "nvenc_d3d11.h"

namespace NVENC_NAMESPACE {

  /**
   * @brief Native Direct3D11 NVENC encoder.
   */
  class nvenc_d3d11_native final: public nvenc_d3d11 {
  public:
    /**
     * @param d3d_device Direct3D11 device used for encoding.
     * @param dll Shared NVENC driver module.
     */
    explicit nvenc_d3d11_native(ID3D11Device *d3d_device, ::nvenc::shared_dll dll);
    ~nvenc_d3d11_native() override;

    std::size_t get_input_texture_count() const override;
    ID3D11Texture2D *get_input_texture(std::size_t index) override;

  protected:
    bool copy_input_surface(std::size_t from, std::size_t to) override;

  private:
    bool create_and_register_input_buffer() override;

    const ID3D11DevicePtr d3d_device;
    std::vector<ID3D11Texture2DPtr> d3d_input_textures;
  };

}  // namespace NVENC_NAMESPACE
#endif
