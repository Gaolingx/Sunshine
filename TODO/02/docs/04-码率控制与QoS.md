# 04 · 码率控制与 QoS

本篇回答三件事：
1. **码率是谁定的？客户端报上来的数字经过了哪些加工？**
2. **这个数字最终变成了哪些编码器参数？**
3. **发出网卡时怎么保证不打突发、怎么让路由器优先转发？**

> 图例见 [README · 图例与阅读约定](./README.md#图例与阅读约定)。

---

## 1. 码率数字的完整旅程

```
客户端 UI 里的滑块/自定义码率
   │
   │  RTSP ANNOUNCE 的 SDP 属性
   │    x-nv-vqos[0].bw.maximumBitrateKbps   ← 客户端声称的“可用带宽上限”
   │    x-ml-video.configuredBitrateKbps     ← 客户端想要的目标码率（默认 "0"）
   ▼
rtsp.cpp  cmd_announce()
   ├─ config.monitor.bitrate = maximumBitrateKbps
   ├─ 若 configuredBitrateKbps != 0 → 回算（见 §1.1）
   └─ 覆盖 config.monitor.bitrate = 回算结果
   ▼
video::config_t::bitrate （bps 语义上是 kbps）
   ▼
video.cpp  encode_session 创建时
   ├─ bitrate = (max_bitrate > 0 ? min(bitrate, config::video.max_bitrate) : bitrate) * 1000
   ├─ BOOST_LOG(info) << "Streaming bitrate is " << bitrate
   └─ 写入 AVCodecContext：rc_max_rate / bit_rate （/ rc_min_rate / rc_buffer_size）
   ▼
【NVENC 路径】nvenc_base::configure_rate_control()
   └─ rcParams.averageBitRate = client_config.bitrate * 1000
      rcParams.vbvBufferSize  = bitrate*1000 / framerate   (如驱动支持)
   ▼
【AVCodec 路径】avcodec_open2()
   └─ ctx->bit_rate / rc_max_rate / rc_min_rate / rc_buffer_size
   ▼
编码器实际输出 ≈ 上述值
```

### 1.1 `configuredBitrateKbps` 的回算（`src/rtsp.cpp:1257` 起）

客户端如果给了 `configuredBitrateKbps`，Sunshine 假设这个值**已经包含**了 FEC 开销和音频开销，需要把它**还原成纯视频目标码率**：

```cpp
if (fec_percentage <= 80) {
  configuredBitrateKbps /= 100.f / (100 - fec_percentage);   // 扣掉 FEC 冗余
}
// 扣掉音频占用
int audioBitrateAdjustment = (QUALITY ? 256 : 96) * channels;
configuredBitrateKbps -= std::min(audioBitrateAdjustment, configuredBitrateKbps / 5);
// 扣掉协议/头部开销的固定预留
configuredBitrateKbps -= std::min(500, configuredBitrateKbps / 10);
config.monitor.bitrate = (int) configuredBitrateKbps;
```

**为什么 `fec_percentage <= 80` 才回算？** 超过 80% 时 `100/(100-fec)` 的发散过大（比如 95% 会放大 20 倍），回算出来的数字已无意义，宁可保留原值让后面的编码器自己处理。

**音频占用估算**：

| 音频质量 | 每声道 kbps | 2 声道 | 6 声道 |
|---------|:----------:|:-----:|:-----:|
| 高（`HIGH_QUALITY`） | 256 | 512 | 1536 |
| 低 | 96 | 192 | 576 |

再取 `min(音频占用, 目标码率的 20%)`，避免小码率场景被音频吃光。

**示例**（2 声道低质量，`fec_percentage = 20`，客户端报 `configuredBitrateKbps = 20000`）：

```
20000 / (100/80)      = 16000
16000 - min(192, 3200) = 15808
15808 - min(500, 1580) = 15308   ← 最终编码器目标码率 (kbps)
```

### 1.2 服务端封顶

```cpp
// src/video.cpp:2234
auto bitrate = ((config::video.max_bitrate > 0)
                ? std::min(config.bitrate, config::video.max_bitrate)
                : config.bitrate) * 1000;
BOOST_LOG(info) << "Streaming bitrate is " << bitrate;
```

| 配置 | 位置 | 默认 | 说明 |
|------|------|:----:|------|
| `config::video.max_bitrate` | `src/config.h:234` | `0` | 服务端码率上限（kbps），`0` = 不限制 |

> ⚠️ **重要差异**：`config::video.max_bitrate` 只在 **AVCodec 路径**（`src/video.cpp`）生效；**NVENC 原生 SDK 路径**（`src/nvenc/nvenc_base.cpp::configure_rate_control`）直接使用 `client_config.bitrate`，**不受 `max_bitrate` 约束**。这是 fork 里一个需要注意的行为差异。

---

## 2. AVCodec 路径的码控参数

`src/video.cpp:2242-2265`：

```cpp
ctx->rc_max_rate = bitrate;
ctx->bit_rate    = bitrate;

if (encoder.flags & flag_e::CBR_WITH_VBR) {
  // Ensure rc_max_bitrate != bit_rate to force VBR mode
  ctx->bit_rate--;              // ★ 用“bit_rate 比 max 小 1”来把驱动骗进 VBR 模式
} else {
  ctx->rc_min_rate = bitrate;   // 真正的 CBR：min == max
}

if (encoder.flags & flag_e::RELAXED_COMPLIANCE) {
  ctx->strict_std_compliance = FF_COMPLIANCE_UNOFFICIAL;
}

if (!(encoder.flags & flag_e::NO_RC_BUF_LIMIT)) {
  if (!hardware && (ctx->slices > 1 || config.videoFormat == 1 /* HEVC */)) {
    // libx264 在 buffer 太小时画质会严重劣化；libx265 更常触发，因此软件 HEVC 一律放大 1.5 倍
    ctx->rc_buffer_size = bitrate / ((config.framerate * 10) / 15);
  } else {
    ctx->rc_buffer_size = bitrate / config.framerate;
#ifndef __APPLE__
    if (encoder.name == "nvenc" && config::video.nv_legacy.vbv_percentage_increase > 0) {
      ctx->rc_buffer_size += ctx->rc_buffer_size * config::video.nv_legacy.vbv_percentage_increase / 100;
    }
#endif
  }
}
```

### 2.1 `flag_e` 全表（`src/video.cpp:411`）

| 标志 | 位 | 含义 |
|------|:--:|------|
| `DEFAULT` | 0 | — |
| `PARALLEL_ENCODING` | 1<<1 | 允许并行编码（多 slice） |
| `H264_ONLY` | 1<<2 | 只支持 H.264 |
| `LIMITED_GOP_SIZE` | 1<<3 | GOP 受限（配合 IDR 请求） |
| `SINGLE_SLICE_ONLY` | 1<<4 | 单 slice |
| `CBR_WITH_VBR` | 1<<5 | **用 VBR 模拟 CBR**（见上） |
| `RELAXED_COMPLIANCE` | 1<<6 | 放宽 FFmpeg 标准合规检查 |
| `NO_RC_BUF_LIMIT` | 1<<7 | 不设 `rc_buffer_size` |
| `REF_FRAMES_INVALIDATION` | 1<<8 | 支持参考帧失效（RFI） |
| `ALWAYS_REPROBE` | 1<<9 | 每次重新探测 |
| `YUV444_SUPPORT` | 1<<10 | 支持 YUV 4:4:4 |
| `ASYNC_TEARDOWN` | 1<<11 | 异步销毁编码器（规避 NVENC 挂死） |
| `FIXED_GOP_SIZE` | 1<<12 | 固定 GOP 大小 |

**两个实际使用的组合**：

| 行号 | 组合 |
|:----:|------|
| `src/video.cpp:991` | `PARALLEL_ENCODING \| CBR_WITH_VBR \| RELAXED_COMPLIANCE \| NO_RC_BUF_LIMIT \| YUV444_SUPPORT` |
| `src/video.cpp:1313` | `LIMITED_GOP_SIZE \| PARALLEL_ENCODING \| NO_RC_BUF_LIMIT` |

`NO_RC_BUF_LIMIT` 的含义是：**不给编码器 VBV 缓冲上限**，允许一帧用掉更多字节以换取更低延迟（VBV 太小会导致编码器为了填满缓冲而降质/攒帧）。

### 2.2 `rc_mode` → FFmpeg 的 `rc` 选项

| `config::video.vk.rc_mode` | 含义 | 映射 | 默认 |
|:--------------------------:|------|------|:----:|
| 0 | 驱动默认 | — | |
| 1 | CQP | | |
| 2 | **CBR** | `NV_ENC_PARAMS_RC_CBR` | ★ 默认 |
| 3 | VBR | | |

映射点：`src/video.cpp` 的 790 / 817 / 854 行（分别对应不同 codec 的选项组装）。

---

## 3. NVENC 原生 SDK 路径

`src/nvenc/nvenc_base.cpp:306` `configure_rate_control()`：

```cpp
enc_config.gopLength                    = NVENC_INFINITE_GOPLENGTH;  // 不自动关键帧
enc_config.frameIntervalP               = 1;                         // 无 B 帧，零重排延迟
enc_config.rcParams.rateControlMode     = NV_ENC_PARAMS_RC_CBR;      // 恒定码率
enc_config.rcParams.zeroReorderDelay    = 1;                         // ★ 零重排延迟
enc_config.rcParams.enableLookahead     = 0;                         // ★ 关闭前瞻
if (config.low_delay_key_frame_scale > 0)
  enc_config.rcParams.lowDelayKeyFrameScale = config.low_delay_key_frame_scale;

// 两趟编码
multiPass = quarter_resolution → NV_ENC_TWO_PASS_QUARTER_RESOLUTION
          = full_resolution    → NV_ENC_TWO_PASS_FULL_RESOLUTION
          = 其他               → NV_ENC_MULTI_PASS_DISABLED

enc_config.rcParams.enableAQ = config.adaptive_quantization;
if (config.aq_strength > 0)
  enc_config.rcParams.aqStrength = min(config.aq_strength, 15);  // 该字段只有 4 bit

enc_config.rcParams.enableNonRefP = config.non_reference_p_frames;

if (config.temporal_adaptive_quantization) {
  if (get_encoder_cap(encode_guid, NV_ENC_CAPS_SUPPORT_TEMPORAL_AQ))
    enc_config.rcParams.enableTemporalAQ = 1;
  else
    BOOST_LOG(warning) << "NvEnc: gpu doesn't support temporal AQ";
}

enc_config.rcParams.averageBitRate = client_config.bitrate * 1000;

if (get_encoder_cap(encode_guid, NV_ENC_CAPS_SUPPORT_CUSTOM_VBV_BUF_SIZE)) {
  enc_config.rcParams.vbvBufferSize = client_config.bitrate * 1000 / client_config.framerate;
  if (config.vbv_percentage_increase > 0)
    enc_config.rcParams.vbvBufferSize += vbvBufferSize * config.vbv_percentage_increase / 100;
}
```

**关键点解读**：

| 参数 | 值 | 对延迟/画质的影响 |
|------|----|-----------------|
| `gopLength = INFINITE` | 无限 GOP | 只在客户端请求 IDR 时才出关键帧，避免周期性大帧造成带宽尖峰 |
| `frameIntervalP = 1` | 无 B 帧 | B 帧需要重排 → 引入帧延迟。串流场景必须为 1 |
| `zeroReorderDelay = 1` | 零重排延迟 | 同上 |
| `enableLookahead = 0` | 关前瞻 | 前瞻会缓存多帧，直接增加延迟 |
| `NV_ENC_PARAMS_RC_CBR` | CBR | 恒定码率，网络更可预测；相比 VBR 画质略差但延迟稳定 |
| `vbvBufferSize = bitrate / fps` | 约等于「一帧的字节数」 | 缓冲极小 → 极低延迟，但码率瞬时波动大 |
| `vbv_percentage_increase` | 默认 0 | 缓冲太小画质崩时可调大（牺牲一点延迟换画质） |

**默认帧的日志**（`src/nvenc/nvenc_base.cpp:773`）：

```
NvEnc: requested encoded frame size <bitrate/8/framerate> kB
```

这是 `平均码率 / 8 / 帧率`，即「平均每帧目标大小」，可作为调优参考。

---

## 4. 相关配置速查

| 配置项 | 位置 | 默认 | 说明 |
|--------|------|:----:|------|
| `video.qp` | `src/config.h:70` | — | 量化参数；**值越大压缩越狠、画质越差** |
| `video.max_bitrate` | `src/config.h:234` | `0` | 码率上限（kbps），0 = 不限（仅 AVCodec 路径生效） |
| `video.minimum_fps_target` | `src/config.h` | `0` | 最小帧率目标，用于 `max_frametime` 计算（见 §6） |
| `video.nvenc` | `nvenc_config` | — | `adaptive_quantization` / `aq_strength` / `two_pass` / `temporal_adaptive_quantization` / `non_reference_p_frames` / `low_delay_key_frame_scale` / `vbv_percentage_increase` |
| `video.nv_legacy.*` | `src/config.h:70+` | — | 走 AVCodec 里的 nvenc 封装时用（`vbv_percentage_increase` 等） |
| `video.vk.rc_mode` | `src/config.h:147` | `2` (CBR) | Vulkan/VAAPI 路径的码控模式 |
| `video.amd.*` | `src/config.h` | — | AMF：`amd_rc_h264/hevc/av1`、`amd_quality_*`、`amd_max_au_size`、`amd_coder` 等 |
| `video.qsv.*` | — | — | Intel QSV 相关 |
| `stream.fec_percentage` | `src/config.h:264` | `20` | FEC 百分比，范围 `[1, 255]` |
| `stream.packetsize` | `src/config.h:264` | `0` | 分片大小，范围 `[PACKETSIZE_MIN, PACKETSIZE_MAX]` |

---

## 5. QoS：qWAVE / DSCP 打标

`platf::enable_socket_qos(sock, address, port, type, dscp_tagging)` —— Windows 实现见 `src/platform/windows/misc.cpp:1650`。

```cpp
// 关掉 DSCP 就直接返回
if (!dscp_tagging) {
  BOOST_LOG(info) << "Windows doesn't support any concept of traffic priority without DSCP tagging";
  return nullptr;
}

// 懒加载 qwave.dll（std::call_once 保证只加载一次）
LoadLibraryExA("qwave.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
// 解析：QOSCreateHandle / QOSAddSocketToFlow / QOSRemoveSocketFromFlow

QOS_VERSION version { 1, 0 };

QOS_TRAFFIC_TYPE traffic_type =
    (type == qos_data_type_e::audio) ? QOSTrafficTypeVoice          // 4
                                     : QOSTrafficTypeAudioVideo;    // 5

// IPv4-mapped IPv6 需要先 connect() 再断开，才能让 qWAVE 认出来
if (需要 workaround) {
  sock.connect(...);   // using_connect_hack
  if (失败) BOOST_LOG(warning) << "qWAVE dual-stack workaround failed";
  else      BOOST_LOG(info)    << "Using qWAVE connect() workaround for QoS tagging";
}

QOSAddSocketToFlow(handle, sock, &addr, traffic_type, QOS_NON_ADAPTIVE_FLOW, &flow_id);
// 返回 qos_t（RAII，析构时 QOSRemoveSocketFromFlow）
```

**调用点**（`src/stream.cpp` 的 `videoThread` / `audioThread`）：

```cpp
session->video.qos = platf::enable_socket_qos(
    video_sock.native_handle(),
    session->video.peer.address(), session->video.peer.port(),
    platf::qos_data_type_e::video,
    session->config.videoQosType != 0);       // ← 来自 SDP 的 qosTrafficType
```

| 通道 | `qos_data_type_e` | Windows 优先级 | 来源 SDP 属性 | 默认值 |
|------|:-----------------:|---------------|--------------|:------:|
| 视频 | `video` | `QOSTrafficTypeAudioVideo` (5) | `x-nv-vqos[0].qosTrafficType` | 5 |
| 音频 | `audio` | `QOSTrafficTypeVoice` (4) | `x-nv-aqos[0].qosTrafficType` | 4 |

> **注意**：QoS 句柄在 **PING 握手成功后**才能创建——因为此时才知道客户端的真实端口。使用 RAII `qos_t`，会话销毁时自动 `QOSRemoveSocketFromFlow`。

---

## 6. 帧内发包配速（pacing）

即使码率算得再准，如果一帧的几百个分片在 0.1 ms 内全部灌进网卡，交换机/路由器会瞬间排队溢出——**突发就是延迟抖动的根源**。Sunshine 因此实现了「帧内配速」。

### 6.1 参数（`src/stream.cpp:1660` 起）

```cpp
// 以 1 Gbps 链路的 80% 为基准，算出「1 毫秒内能发多少包」
size_t ratecontrol_packets_in_1ms = 1'000'000'000 * 80 / 100 / 1000 / blocksize / 8;

// 单个 send_batch() 不超过 64 KB
//   原因①：Windows 上超过 64K 的批会绕过 SO_SNDBUF，落到 "Other I/O" 并等中断，性能不稳
size_t send_batch_size = 64 * 1024 / blocksize;
//   原因②：Linux 的 GSO 一次最多 64 个包
send_batch_size = std::min<size_t>(64, send_batch_size);

// 不要忽略上一帧最后一个「速率控制组」
auto ratecontrol_frame_start = std::max(ratecontrol_next_frame_start, std::chrono::steady_clock::now());

size_t ratecontrol_frame_packets_sent = 0;
size_t ratecontrol_group_packets_sent = 0;
```

其中 `blocksize = packetsize + MAX_RTP_HEADER_SIZE(16)`。

### 6.2 发送循环

```cpp
for (x = 0; x < shards.size(); ++x) {
  ...
  if (x - next_shard_to_send + 1 >= send_batch_size || x + 1 == shards.size()) {
    // 需要开始新的「1ms 速率控制组」的两种情况：
    //   ① 本组已发满 ratecontrol_packets_in_1ms
    //   ② 这是本帧的第一个批（要先替上一帧的尾批把时间对上）
    if (ratecontrol_group_packets_sent >= ratecontrol_packets_in_1ms ||
        ratecontrol_frame_packets_sent == 0) {

      auto due = ratecontrol_frame_start + 1ms * ratecontrol_frame_packets_sent / ratecontrol_packets_in_1ms;
      auto now = std::chrono::steady_clock::now();
      if (now < due) timer->sleep_for(due - now);   // 高精度定时器睡眠

      ratecontrol_group_packets_sent = 0;
    }

    // 批量发送
    batch_info.block_offset = next_shard_to_send;
    batch_info.block_count  = x - next_shard_to_send + 1;
    if (!platf::send_batch(batch_info)) {
      BOOST_LOG(verbose) << "Falling back to unbatched send";
      for (...) platf::send(send_info);            // 平台不支持批量则逐包发
    }

    ratecontrol_group_packets_sent += current_batch_size;
    ratecontrol_frame_packets_sent += current_batch_size;
    next_shard_to_send = x + 1;
  }
}

// 记住本帧的“理论结束时刻”，供下一帧复用
ratecontrol_next_frame_start = ratecontrol_frame_start +
                               1ms * ratecontrol_frame_packets_sent / ratecontrol_packets_in_1ms;
```

### 6.3 配速模型图解

```
时间(ms) →   0      1      2      3      4
            │      │      │      │      │
第 N 帧:    ████████████░░░░░░░            ← 帧 N 分片（假设 2.5ms 发完）
                  ↑
            组1 满 ratecontrol_packets_in_1ms → 睡眠到 due
第 N+1 帧:                     ██████████  ← ratecontrol_next_frame_start 承接上帧余量
                              ↑
                        不会“抢跑”，也不会“空转”
```

**三点设计意图**：

| 设计 | 目的 |
|------|------|
| `ratecontrol_frame_start = max(next_frame_start, now)` | 上一帧没发完（`next_frame_start > now`）→ 立刻接着发，不空转；上一帧提前发完 → 等 `next_frame_start` 再开新帧，抑制突发 |
| 「本帧第一个批」强制触发组重置 | 让上一帧的尾批与新帧的首批之间有时间间隔，避免两帧之间出现 2× 速率 |
| 批量大小同时受 64 KB 和 64 包限制 | 绕过 Windows `SO_SNDBUF` 语义问题和 Linux GSO 上限 |

> **注意**：`videoBroadcastThread` 需要 `platf::create_high_precision_timer()`，失败则整个视频广播线程无法工作——因为 `sleep_for` 的精度直接决定配速是否有效。

### 6.4 音频为什么不需要配速？

音频是固定节奏：每 `packetDuration`（默认 5 ms）产生一批 6 个 UDP 包（4 数据 + 2 校验），速率天然平稳且远低于链路容量，因此 `audioBroadcastThread` 里没有配速逻辑。

---

## 7. 码率相关的其它环节

| 环节 | 位置 | 说明 |
|------|------|------|
| 客户端带宽上限 | SDP `x-nv-vqos[0].bw.maximumBitrateKbps` | 直接作为 `config.monitor.bitrate` |
| 帧率一致性 | `cmd_announce` | `framerateX100` 与 `framerate` 差 > ±1% 则丢弃 |
| 编码格式可用性 | `cmd_announce` | 请求 HEVC/AV1 但被禁用 → 400 |
| 帧内分片数 | `video.cpp` FEC 计算 | 见 [05 · FEC](./05-FEC-重传-丢帧与心跳.md) |
| RTP 时间戳 | `videoBroadcastThread` | 90 kHz 时钟，`timestamp = round(frame_timestamp - video_epoch)` |
| 强制 IDR | 控制通道 `IDX_REQUEST_IDR_FRAME` | 客户端丢包时请求，会造成瞬时带宽尖峰 |
| 丢失统计 | 控制通道 `IDX_LOSS_STATS` | ⚠️ **仅记录日志，不做任何码率自适应** |

---

## 8. 小结

| 问题 | 答案 |
|------|------|
| 码率是谁定的？ | 客户端；Sunshine 只做 FEC/音频/头部开销的**回算**与服务端封顶 |
| 默认码控模式？ | CBR（`vk.rc_mode = 2`），NVENC 走 `NV_ENC_PARAMS_RC_CBR` |
| 为什么用 CBR 不用 VBR？ | 延迟可预测；瞬时峰值受 VBV 约束 |
| `max_bitrate` 生效范围？ | 仅 AVCodec 路径（NVENC 原生 SDK 不读它） |
| 怎么降延迟？ | 无限 GOP + 无 B 帧 + 零重排 + 关前瞻 + 极小 VBV |
| 怎么避免网络突发？ | 帧内配速（1 Gbps 的 80% 为基准，1 ms 一个速率控制组） |
| 怎么让网络优先转发？ | Windows 用 qWAVE 打 DSCP；音频 = Voice，视频 = AudioVideo |

下一篇：[05 · FEC、重传、丢帧与心跳](./05-FEC-重传-丢帧与心跳.md)
