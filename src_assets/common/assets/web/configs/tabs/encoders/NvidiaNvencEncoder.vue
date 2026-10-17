<script setup>
import { ref } from 'vue'
import Checkbox from "../../../Checkbox.vue";

const props = defineProps([
  'platform',
  'config',
])

const config = ref(props.config)
</script>

<template>
  <div id="nvidia-nvenc-encoder" class="config-page">
    <!-- Performance preset -->
    <div class="mb-3">
      <label for="nvenc_preset" class="form-label">{{ $t('config.nvenc_preset') }}</label>
      <select id="nvenc_preset" class="form-select" v-model="config.nvenc_preset">
        <option value="1">P1 {{ $t('config.nvenc_preset_1') }}</option>
        <option value="2">P2</option>
        <option value="3">P3</option>
        <option value="4">P4</option>
        <option value="5">P5</option>
        <option value="6">P6</option>
        <option value="7">P7 {{ $t('config.nvenc_preset_7') }}</option>
      </select>
      <div class="form-text">{{ $t('config.nvenc_preset_desc') }}</div>
    </div>

    <!-- Split frame encoding -->
    <div class="mb-3" v-if="platform === 'windows'">
      <label for="nvenc_split_encode" class="form-label">{{ $t('config.nvenc_split_encode') }}</label>
      <select id="nvenc_split_encode" class="form-select" v-model="config.nvenc_split_encode">
        <option value="disabled">{{ $t('_common.disabled') }}</option>
        <option value="driver_decides">{{ $t('config.nvenc_split_encode_driver_decides_def') }}</option>
        <option value="enabled">{{ $t('_common.enabled') }}</option>
      </select>
      <div class="form-text">{{ $t('config.nvenc_split_encode_desc') }}</div>
    </div>

    <!-- Two-pass mode -->
    <div class="mb-3">
      <label for="nvenc_twopass" class="form-label">{{ $t('config.nvenc_twopass') }}</label>
      <select id="nvenc_twopass" class="form-select" v-model="config.nvenc_twopass">
        <option value="disabled">{{ $t('config.nvenc_twopass_disabled') }}</option>
        <option value="quarter_res">{{ $t('config.nvenc_twopass_quarter_res') }}</option>
        <option value="full_res">{{ $t('config.nvenc_twopass_full_res') }}</option>
      </select>
      <div class="form-text">{{ $t('config.nvenc_twopass_desc') }}</div>
    </div>

    <!-- Spatial AQ -->
    <Checkbox class="mb-3"
              id="nvenc_spatial_aq"
              locale-prefix="config"
              v-model="config.nvenc_spatial_aq"
              default="false"
    ></Checkbox>

    <!-- Temporal AQ -->
    <Checkbox class="mb-3"
              id="nvenc_temporal_aq"
              locale-prefix="config"
              v-model="config.nvenc_temporal_aq"
              default="false"
    ></Checkbox>

    <!-- Adaptive quantization strength -->
    <div class="mb-3">
      <label for="nvenc_aq_strength" class="form-label">{{ $t('config.nvenc_aq_strength') }}</label>
      <input type="number" min="0" max="15" class="form-control" id="nvenc_aq_strength" placeholder="0"
             v-model="config.nvenc_aq_strength" />
      <div class="form-text">{{ $t('config.nvenc_aq_strength_desc') }}</div>
    </div>

    <!-- VBV/HRD buffer size in milliseconds -->
    <div class="mb-3">
      <label for="nvenc_vbv_ms" class="form-label">{{ $t('config.nvenc_vbv_ms') }}</label>
      <input type="number" min="0" max="1000" class="form-control" id="nvenc_vbv_ms" placeholder="0"
             v-model="config.nvenc_vbv_ms" />
      <div class="form-text">
        {{ $t('config.nvenc_vbv_ms_desc') }}<br>
        <br>
        <a href="https://en.wikipedia.org/wiki/Video_buffering_verifier">VBV/HRD</a>
      </div>
    </div>

    <!-- Rate control lookahead depth -->
    <div class="mb-3">
      <label for="nvenc_lookahead_depth" class="form-label">{{ $t('config.nvenc_lookahead_depth') }}</label>
      <input type="number" min="0" max="16" class="form-control" id="nvenc_lookahead_depth" placeholder="0"
             v-model="config.nvenc_lookahead_depth" />
      <div class="form-text">{{ $t('config.nvenc_lookahead_depth_desc') }}</div>
    </div>

    <!-- Advanced compression options -->
    <div class="mb-3 accordion">
      <div class="accordion-item">
        <h2 class="accordion-header">
          <button class="accordion-button collapsed" type="button" data-bs-toggle="collapse"
                  data-bs-target="#panelsStayOpen-collapseTwo">
            {{ $t('config.nvenc_compression') }}
          </button>
        </h2>
        <div id="panelsStayOpen-collapseTwo" class="accordion-collapse collapse">
          <div class="accordion-body">
            <!-- Low delay key frame scale -->
            <div class="mb-3">
              <label for="nvenc_ldkfs" class="form-label">{{ $t('config.nvenc_ldkfs') }}</label>
              <input type="number" min="0" max="10" class="form-control" id="nvenc_ldkfs" placeholder="0"
                     v-model="config.nvenc_ldkfs" />
              <div class="form-text">{{ $t('config.nvenc_ldkfs_desc') }}</div>
            </div>

            <!-- Non-reference P-frames -->
            <Checkbox class="mb-3"
                      id="nvenc_nonref_p"
                      locale-prefix="config"
                      v-model="config.nvenc_nonref_p"
                      default="false"
            ></Checkbox>

            <!-- Weighted prediction -->
            <Checkbox class="mb-3"
                      id="nvenc_weighted_prediction"
                      locale-prefix="config"
                      v-model="config.nvenc_weighted_prediction"
                      default="false"
            ></Checkbox>
          </div>
        </div>
      </div>
    </div>

    <!-- Quantization parameter limits -->
    <div class="mb-3 accordion">
      <div class="accordion-item">
        <h2 class="accordion-header">
          <button class="accordion-button collapsed" type="button" data-bs-toggle="collapse"
                  data-bs-target="#panelsStayOpen-collapseThree">
            {{ $t('config.nvenc_qp_limits') }}
          </button>
        </h2>
        <div id="panelsStayOpen-collapseThree" class="accordion-collapse collapse">
          <div class="accordion-body">
            <!-- Minimum QP limit -->
            <Checkbox class="mb-3"
                      id="nvenc_enable_min_qp"
                      locale-prefix="config"
                      v-model="config.nvenc_enable_min_qp"
                      default="false"
            ></Checkbox>

            <div class="mb-3">
              <label for="nvenc_min_qp_h264" class="form-label">{{ $t('config.nvenc_min_qp_h264') }}</label>
              <input type="number" min="0" max="51" class="form-control" id="nvenc_min_qp_h264" placeholder="19"
                     v-model="config.nvenc_min_qp_h264" />
              <div class="form-text">{{ $t('config.nvenc_min_qp_h264_desc') }}</div>
            </div>

            <div class="mb-3">
              <label for="nvenc_min_qp_hevc" class="form-label">{{ $t('config.nvenc_min_qp_hevc') }}</label>
              <input type="number" min="0" max="51" class="form-control" id="nvenc_min_qp_hevc" placeholder="23"
                     v-model="config.nvenc_min_qp_hevc" />
              <div class="form-text">{{ $t('config.nvenc_min_qp_hevc_desc') }}</div>
            </div>

            <div class="mb-3">
              <label for="nvenc_min_qp_av1" class="form-label">{{ $t('config.nvenc_min_qp_av1') }}</label>
              <input type="number" min="0" max="255" class="form-control" id="nvenc_min_qp_av1" placeholder="23"
                     v-model="config.nvenc_min_qp_av1" />
              <div class="form-text">{{ $t('config.nvenc_min_qp_av1_desc') }}</div>
            </div>

            <!-- Maximum QP limit -->
            <Checkbox class="mb-3"
                      id="nvenc_enable_max_qp"
                      locale-prefix="config"
                      v-model="config.nvenc_enable_max_qp"
                      default="false"
            ></Checkbox>

            <div class="mb-3">
              <label for="nvenc_max_qp_h264" class="form-label">{{ $t('config.nvenc_max_qp_h264') }}</label>
              <input type="number" min="0" max="51" class="form-control" id="nvenc_max_qp_h264" placeholder="51"
                     v-model="config.nvenc_max_qp_h264" />
              <div class="form-text">{{ $t('config.nvenc_max_qp_h264_desc') }}</div>
            </div>

            <div class="mb-3">
              <label for="nvenc_max_qp_hevc" class="form-label">{{ $t('config.nvenc_max_qp_hevc') }}</label>
              <input type="number" min="0" max="51" class="form-control" id="nvenc_max_qp_hevc" placeholder="51"
                     v-model="config.nvenc_max_qp_hevc" />
              <div class="form-text">{{ $t('config.nvenc_max_qp_hevc_desc') }}</div>
            </div>

            <div class="mb-3">
              <label for="nvenc_max_qp_av1" class="form-label">{{ $t('config.nvenc_max_qp_av1') }}</label>
              <input type="number" min="0" max="255" class="form-control" id="nvenc_max_qp_av1" placeholder="255"
                     v-model="config.nvenc_max_qp_av1" />
              <div class="form-text">{{ $t('config.nvenc_max_qp_av1_desc') }}</div>
            </div>
          </div>
        </div>
      </div>
    </div>

    <!-- Miscellaneous options -->
    <div class="mb-3 accordion">
      <div class="accordion-item">
        <h2 class="accordion-header">
          <button class="accordion-button" type="button" data-bs-toggle="collapse"
                  data-bs-target="#panelsStayOpen-collapseOne">
            {{ $t('config.misc') }}
          </button>
        </h2>
        <div id="panelsStayOpen-collapseOne" class="accordion-collapse collapse show"
             aria-labelledby="panelsStayOpen-headingOne">
          <div class="accordion-body">
            <!-- NVENC Realtime HAGS priority -->
            <Checkbox v-if="platform === 'windows'"
                      class="mb-3"
                      id="nvenc_realtime_hags"
                      locale-prefix="config"
                      v-model="config.nvenc_realtime_hags"
                      default="true"
            >
              <br>
              <br>
              <a href="https://devblogs.microsoft.com/directx/hardware-accelerated-gpu-scheduling/">HAGS</a>
            </Checkbox>

            <!-- Prefer lower encoding latency over power savings -->
            <Checkbox v-if="platform === 'windows'"
                      class="mb-3"
                      id="nvenc_latency_over_power"
                      locale-prefix="config"
                      v-model="config.nvenc_latency_over_power"
                      default="true"
            ></Checkbox>

            <!-- Present OpenGL/Vulkan on top of DXGI -->
            <Checkbox v-if="platform === 'windows'"
                      class="mb-3"
                      id="nvenc_opengl_vulkan_on_dxgi"
                      locale-prefix="config"
                      v-model="config.nvenc_opengl_vulkan_on_dxgi"
                      default="true"
            ></Checkbox>

            <!-- NVENC H264 CAVLC -->
            <Checkbox class="mb-3"
                      id="nvenc_h264_cavlc"
                      locale-prefix="config"
                      v-model="config.nvenc_h264_cavlc"
                      default="false"
            ></Checkbox>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<style scoped>

</style>
