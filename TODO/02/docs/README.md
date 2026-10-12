# Sunshine 项目说明书（分层功能文档）

> **定位**：本目录是面向**使用者 / 二次开发者 / 排障人员**的项目说明书。
> 全文以 **功能层面（layer）** 为单位横向切开整个项目：从 `main.cpp` 进程启动 → 三大 HTTP/RTSP 服务 → 会话与推流编排 → 采集与编码 → 平台抽象（Windows / Linux / macOS）。
> 每一篇都可以独立查阅，编号越小越靠上（越接近进程入口）。

---

## 📚 文档索引

| 编号 | 文档 | 覆盖的功能层 | 主要源文件 |
|:----:|------|-------------|-----------|
| 00 | [README.md](./README.md) | 阅读指南、全景图、术语表 | — |
| 01 | [01-总览与启动流程.md](./01-总览与启动流程.md) | 仓库结构、`main.cpp` 启动时序、L0–L4 分层架构、端口表 | `src/main.cpp`、`src/globals.h`、`src/network.cpp` |
| 02 | [02-协议与会话生命周期.md](./02-协议与会话生命周期.md) | GameStream HTTP / WebUI HTTP / RTSP 三大服务、配对与鉴权、`stream::session`、`broadcast_ctx`、一次完整的“连接→推流→断开” | `src/nvhttp.cpp`、`src/confighttp.cpp`、`src/rtsp.cpp`、`src/stream.cpp` |
| 03 | [03-输入系统.md](./03-输入系统.md) | 手柄 / 键鼠 / 触摸 / 笔注入、力反馈回传、输入批处理与校验、暂停-恢复保持 | `src/input.cpp`、`src/input.h` |
| 04 | [04-码率控制与QoS.md](./04-码率控制与QoS.md) | 客户端码率协商、FEC 预算回算、AVCodec / NVENC 码控参数、DSCP/qWAVE、帧内发包配速 | `src/video.cpp`、`src/nvenc/nvenc_base.cpp`、`src/platform/windows/misc.cpp` |
| 05 | [05-FEC-重传-丢帧与心跳.md](./05-FEC-重传-丢帧与心跳.md) | Reed-Solomon FEC（视频/音频）、重传与参考帧失效、丢帧与队列溢出、心跳与超时 | `src/stream.cpp`、`src/thread_safe.h` |
| 06 | [06-采集与编码.md](./06-采集与编码.md) | 屏幕采集平台层、视频编码器子系统（NVENC/AMF/QSV/VAAPI/VideoToolbox/软件）、音频采集与 Opus 编码 | `src/video.cpp`、`src/audio.cpp`、`src/platform/*` |
| 07 | [07-配置-鉴权与WebUI.md](./07-配置-鉴权与WebUI.md) | 配置体系与校验、加密策略、WebUI 路由与前端、配置热重载 | `src/config.cpp`、`src/confighttp.cpp`、`src/httpcommon.cpp` |
| 08 | [08-附录-协议常量与结构速查.md](./08-附录-协议常量与结构速查.md) | 控制报文类型表、二进制线格式结构体、常量与默认值速查 | `src/stream.cpp`、`src/config.cpp` |

---

## 🗺️ 全景图（一页看懂）

```
                        ┌───────────────────────────────────────────────┐
   Moonlight 客户端      │              Sunshine (Host)                  │
   ─────────────────     │                                               │
   HTTPS 47989 (GameStream) ──► L1-a  nvhttp         : serverinfo/applist/launch/resume/cancel/pair
   HTTP  47990 (WebUI)      ──► L1-b  confighttp     : WebUI 页面 + /api/* JSON
   RTSP  48010 (setup)      ──► L1-c  rtsp_stream    : OPTIONS/DESCRIBE/ANNOUNCE/SETUP/PLAY
                                  │
                                  ▼
                          L2  stream::session  ── broadcast_ctx_t ── 4 线程
                              (alloc / start / stop / join)     │
                                  │                             ├─ recv_thread     (UDP PING / 打洞)
                                  │                             ├─ control_thread  (ENet 控制通道 + 加密)
                                  │                             ├─ video_thread    (采集 + 编码 + FEC + 配速)
                                  │                             └─ audio_thread    (音频采集 + Opus + FEC)
                                  ▼
                          L3  video::capture / audio::capture
                              (编码器选择、码控、编码循环)
                                  │
                                  ▼
                          L4  platf::*  （Windows DXGI/WGC+WASAPI / Linux KMS+X11+VAAPI / macOS AVFoundation）
```

**数据方向总览**

| 通道 | 传输 | 端口偏移 | 可靠性 | 加密 |
|------|------|:--------:|--------|------|
| 控制（输入/反馈/心跳） | ENet over UDP | `+10` | 可靠有序（`ENET_PACKET_FLAG_RELIABLE`） | 可选 AES-GCM（控制协议 13） |
| 视频 | 原始 UDP + RTP 头 + FEC | `+9` | 不可靠（靠 FEC + 客户端请求 IDR） | 可选 AES-GCM |
| 音频 | 原始 UDP + RTP 头 + FEC | `+11` | 不可靠（4+2 RS 固定冗余） | 固定 AES-CBC |
| 会话建立 | RTSP over TCP | `+21` | 可靠 | 可选 AES-GCM（`rtspenc://`） |

---

## 🧭 术语表

| 术语 | 含义 |
|------|------|
| **rikey** | RTSP 阶段协商出的 16 字节对称密钥，用于派生控制/视频/音频三路加密密钥 |
| **avRiKeyId** | 音频 AES-CBC 的起始 IV，取自 `launch_session.iv` 的前 4 字节 |
| **av_ping_payload** | 客户端用于在 UDP 通道上“认领”自己会话的 16 个十六进制字符（`RAND_bytes(8)` 的 hex） |
| **control_connect_data** | 控制通道会话配对的 32 位随机数，通过 `X-SS-Connect-Data` 下发 |
| **ping_timeout** | 心跳超时（默认 10 s），同时用于 ENet 控制通道与 UDP 首次 PING |
| **IDX_*** | 控制通道消息索引常量，见 [08-附录](./08-附录-协议常量与结构速查.md) |
| **FEC** | Forward Error Correction，前景纠错；视频按客户端 `fecPercentage` 加校验分片，音频固定 4+2 |
| **IDR** | Instantaneous Decoder Refresh，关键帧；客户端丢包后通过控制通道请求重发 |
| **RFI** | Reference Frame Invalidation，参考帧失效；比重发 IDR 更“轻”的恢复手段 |
| **qWAVE** | Windows 的 QoS 服务（`qwave.dll`），用于给音频/视频流打 DSCP 优先级 |
| **pacing** | 把一帧的所有分片均匀摊到 1 ms 的“速率控制组”里发出，避免突发挤爆网卡缓冲 |

---

## 🔧 相关配套资料

- 编译与环境搭建：仓库根 [README.md](../../../README.md)、[docs/](../../../docs/)
- 历史调优讨论：`TODO/01/NVENC超低延迟调优作用与场景.md`、`TODO/01/Rustdesk 和 Sunshine 延迟 带宽差异.md`
- 客户端协议实现（C 语言参考实现，随仓库携带）：`third-party/moonlight-common-c/`

> **文档约定**：文中形如 `src/stream.cpp:1164` 的引用表示“文件:起始行”，便于对照源码；所有数值默认值均来自 `src/config.cpp` 的实际解析代码。
