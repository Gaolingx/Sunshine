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
| 02 | [02-协议与会话生命周期.md](./02-协议与会话生命周期.md) | GameStream HTTP / WebUI HTTP / RTSP 三大服务、配对与鉴权、`stream::session`、`broadcast_ctx`、一次完整的“连接→推流→断开”时序（§4）、**全部 8 条断开路径**（§5） | `src/nvhttp.cpp`、`src/confighttp.cpp`、`src/rtsp.cpp`、`src/stream.cpp` |
| 03 | [03-输入系统.md](./03-输入系统.md) | 手柄 / 键鼠 / 触摸 / 笔注入、力反馈回传、输入批处理与校验、**输入上下文生命周期与暂停-恢复**（§9）、测试钩子（§11） | `src/input.cpp`、`src/input.h` |
| 04 | [04-码率控制与QoS.md](./04-码率控制与QoS.md) | 客户端码率协商、FEC 预算回算、AVCodec / NVENC 码控参数、DSCP/qWAVE、帧内发包配速 | `src/video.cpp`、`src/nvenc/nvenc_base.cpp`、`src/platform/windows/misc.cpp` |
| 05 | [05-FEC-重传-丢帧与心跳.md](./05-FEC-重传-丢帧与心跳.md) | Reed-Solomon FEC（视频/音频）、重传与参考帧失效、丢帧与队列溢出、心跳与超时 | `src/stream.cpp`、`src/thread_safe.h` |
| 06 | [06-采集与编码.md](./06-采集与编码.md) | 屏幕采集平台层、视频编码器子系统（NVENC/AMF/QSV/VAAPI/VideoToolbox/软件）、音频采集与 Opus 编码 | `src/video.cpp`、`src/audio.cpp`、`src/platform/*` |
| 07 | [07-配置-鉴权与WebUI.md](./07-配置-鉴权与WebUI.md) | 配置体系与校验、加密策略、WebUI 路由与前端、配置热重载 | `src/config.cpp`、`src/confighttp.cpp`、`src/httpcommon.cpp` |
| 08 | [08-附录-协议常量与结构速查.md](./08-附录-协议常量与结构速查.md) | 控制报文类型表、二进制线格式结构体、常量与默认值速查、**关键符号参考表**（§11） | `src/stream.cpp`、`src/config.cpp` |

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

## 图例与阅读约定

### ASCII 图符号

本套文档中的所有示意图都是纯 ASCII / 半角字符，符号含义固定如下：

| 符号 | 含义 | 典型上下文 |
|:----:|------|-----------|
| `┌ ─ ┐ │ └ ┘ ├ ┤ ┬ ┴ ┼` | 方框：一个**进程内组件**、线程、函数或状态 | 模块框、收敛图 |
| `+` `-` 与竖线字符 | 方框的**纯 ASCII 等价写法**（少数篇章为避免制表符对不齐而使用），含义与上表一致 | `05`、`07` 的部分图 |
| `═══` / `══►` | **主要数据流**（高频路径） | 端到端流水线 |
| `───` / `─►` | 普通调用 / 消息（低频或控制路径） | 泳道、状态转移 |
| `▼` | 向下进入下一层 / 下一步 | 分层图、收敛图 |
| `▲` | 向上返回 / 反向通道 | 力反馈、终止包 |
| `◄──►` | 双向（请求-响应） | HTTP / RTSP 交互 |
| `│`（竖线列） | **时间轴**，同列自上而下即时间先后 | 泳道时序图 |
| `★` | 关键节点 / 最容易误解的位置（**务必细看附近文字**） | 时序图、收敛图 |
| `⇒` | 推导 / 等价关系（如"等价于调用 X"） | 说明性文字 |
| `·`（行首） | 上一项的**子动作**，不代表新步骤 | 展开的调用树 |
| `(…)` | 括号内为**补充说明**，非流程的一部分 | 全篇 |
| `文件:行` | 源码定位（`src/stream.cpp:1164`）；可点击跳转 | 全篇 |
| `⚠️` | 易错点 / 反直觉行为 | 全篇 |
| `✓` / `✗` | 成立 / 不成立 | 对照表 |

### 编号约定

| 记法 | 含义 |
|------|------|
| `L0` … `L4` | 项目分层（见 [01 §3](./01-总览与启动流程.md)），数字越大越靠近操作系统 |
| `阶段 ①` … `阶段 ⑥` | 一次客户端会话的六个阶段（见 [02 §4.1](./02-协议与会话生命周期.md)） |
| `平面 A / B / C` | 分别为 TCP 控制面、UDP 数据面、稳态推流（见 [02 §4.2](./02-协议与会话生命周期.md)） |
| `P1` … `P8` | 8 条断开路径（见 [02 §5.2](./02-协议与会话生命周期.md)） |
| `IDX_*` | 控制通道消息的**数组下标**常量（线上值是另一回事，见 [08 §2.1](./08-附录-协议常量与结构速查.md)） |
| `0x0305` 之类 | 线上**字节值**，除非注明均为**大端** |
| `+9 / +10 / +11` | 端口偏移（相对 `sunshine.port`，默认 47989，见 [08 §1](./08-附录-协议常量与结构速查.md)） |

### 阅读顺序建议

```
排障（"连不上 / 黑屏 / 卡顿"）
  01 §2 启动时序 ──► 02 §4 连接→推流→断开 ──► 02 §5 断开路径 ──► 05 心跳与 FEC

开发（"改这里会影响什么"）
  08 §11 关键符号参考表 ──► 找符号所在篇 ──► 用 08 §10 反查表确认关联文件

速查（"这个字段是什么"）
  08 §1~§9 ──► 直接对照源码
```

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
| **保留输入（retained input）** | 以客户端证书（或 `unique_id`）为键、跨会话存活的输入上下文；使“暂停-恢复”时虚拟手柄不重建，见 [03 §9](./03-输入系统.md) |
| **rebind_input** | 复用保留输入上下文时，把它的邮箱句柄与 `platf::rebind_gamepad` 接到**新会话**上，见 [03 §9.4](./03-输入系统.md) |
| **收尸（reap）** | `controlBroadcastThread` 对 `state == STOPPING` 的会话执行 `erase` + `enet_peer_disconnect_now` + `controlEnd.raise`，见 [02 §5.3](./02-协议与会话生命周期.md) |
| **唯一收敛点** | 8 条断开路径最终都调用 `session::stop()`；它只做 CAS + `raise`，不做 join，见 [02 §5.1](./02-协议与会话生命周期.md) |
| **tap（`SS_HDR_METADATA`）** | HDR 元数据经 `IDX_HDR_MODE` 下发的载荷，见 [06](./06-采集与编码.md) |
| **整队清空** | `queue_t` 溢出策略为 `drop_oldest` 时，`raise()` 发现队列满会 `_queue.clear()` 而不是丢一个，见 [05 §5](./05-FEC-重传-丢帧与心跳.md) |

---

## 🔧 相关配套资料

- 编译与环境搭建：仓库根 [README.md](../../../README.md)、[docs/](../../../docs/)
- 历史调优讨论：`TODO/01/NVENC超低延迟调优作用与场景.md`、`TODO/01/Rustdesk 和 Sunshine 延迟 带宽差异.md`
- 客户端协议实现（C 语言参考实现，随仓库携带）：`third-party/moonlight-common-c/`

> **文档约定**：文中形如 `src/stream.cpp:1164` 的引用表示“文件:起始行”，便于对照源码；所有数值默认值均来自 `src/config.cpp` 的实际解析代码。
>
> **关于行号**：源码行号基于本 fork 的当前工作树（`Gaolingx/Sunshine`）实测。上游 `LizardByte/Sunshine` 的 `master` 若已变动，符号名仍可用于定位（见 [08 §11 关键符号参考表](./08-附录-协议常量与结构速查.md)）。
>
> **图例**：所有示意图的符号含义见上文 [图例与阅读约定](#图例与阅读约定)。
