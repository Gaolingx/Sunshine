# 09 - NVENC 编码参数详解与场景调优

> **定位**：本文只回答一个问题 —— **NVENC 那一堆 `nvenc_*` 配置键，每一个到底在干什么、能不能改、改了会怎样**。
> 覆盖三层内容：① 参数分类（硬编码 / 可配置 / 只在某一平台生效）；② 每个参数的**用途 + 改动后果**（延迟 / 画质 / 码率占用）；③ **避雷清单**与**场景推荐配置**（可直接粘贴进 `sunshine.conf`）。
>
> **与前序文档的分工**（避免重复阅读）：
> | 想知道什么 | 去哪看 |
> |-----------|--------|
> | 码率数字是怎么一层层算出来的、FEC 与音频怎么扣 | [04 §1](./04-码率控制与QoS.md#1-码率数字的完整旅程) |
> | `configure_rate_control()` 这个函数整体长什么样 | [04 §3](./04-码率控制与QoS.md#3-nvenc-原生-sdk-路径) |
> | 编码器子系统的结构（`encoder_t`、`probe_encoders()`、`nv_legacy`） | [06 §1](./06-采集与编码.md#1-编码器子系统) |
> | **每个旋钮的语义 + 改动后果 + 避雷 + 场景配方** | **本文** |
> | 配置怎么解析、校验失败会怎样、怎么热重载 | [07 §1](./07-配置-鉴权与WebUI.md#1-配置系统) |
> | 超发之后网络侧发生什么（队列溢出 / 整队清空 / 请求 IDR） | [05 §2](./05-FEC-重传-丢帧与心跳.md#4-重传sunshine-没有-arq)、[05 §5](./05-FEC-重传-丢帧与心跳.md#5-丢帧没有显式策略只有队列覆盖) |
>
> **行号基准**：本 fork `Gaolingx/Sunshine` 当前工作树（分支 `feature-add_nvenc_present`）。符号名才是稳定锚点，见 [08 §11](./08-附录-协议常量与结构速查.md#11-关键符号参考表)。

---

## 1. 生效边界：一个参数要过四道关

这是全篇最重要的一节。**绝大多数"我改了没反应"都死在这里**，而不是死在你填的数值上。

```
sunshine.conf 里的 nvenc_xxx
   │
   ├─ 关 1：配置解析与范围校验（config.cpp）
   │     越界 / 拼写错 / 未知取值 → 打一条 warning，静默用回默认值
   │     （例：nvenc_preset 允许 1~7，写 8 会被拒绝）
   │
   ├─ 关 2：编码器路径
   │     Windows  → 原生 NVENC SDK（src/nvenc/*），全功能
   │     Linux    → FFmpeg + ffnvcodec（src/video.cpp 选项表），只有部分键
   │
   ├─ 关 3：GPU 能力位（NV_ENC_CAPS_*）
   │     驱动不支持 → 要么打 warning 降级，要么**完全静默**（最坑）
   │
   └─ 关 4：客户端协商值（RTSP / 握手）
          bitrate、framerate、slicesPerFrame、numRefFrames、enableIntraRefresh
          这些**来自客户端**，Sunshine 配置里根本没有对应键
```

### 1.1 关 1：校验不过 = 回到默认值，且只在启动时打印一次

`apply_config()` 的 NVENC 段（[src/config.cpp:1635](../../../src/config.cpp#L1635)）对每个键做**独立的范围检查**。要点三条：

1. **不做交叉校验** —— 例如 `nvenc_min_qp_h264 = 40` 与 `nvenc_max_qp_h264 = 30` 会被双双接受（见 [§6 避雷第 3 条](#6-避雷清单)）。
2. **枚举值不合法会降级到默认**，不是报错退出：`nvenc_tuning` 值不认识 → `ultra_low_latency`；`nvenc_twopass` 不认识 → `quarter_resolution`；`nvenc_split_encode` 不认识 → `driver_decides`。
3. **越界值被丢弃而不是被夹紧** —— 写 `nvenc_aq_strength = 20` 不会变成 15，而是**整项失效回到 0（= 驱动默认）**。

### 1.2 关 2：Windows 与 Linux 不是同一套实现

| 键 | Windows 原生 SDK | Linux FFmpeg | 备注 |
|----|:----------------:|:------------:|------|
| `nvenc_preset` / `nvenc_tuning` / `nvenc_twopass` | ✓ | ✓ | 经 `nv_legacy` 镜像成 `preset` / `tune` / `multipass` |
| `nvenc_spatial_aq` / `nvenc_temporal_aq` / `nvenc_aq_strength` | ✓ | ✓ | `spatial-aq` / `temporal-aq` / `aq-strength`；⚠️ Linux 上该值范围是 1–15，默认 0 会越界导致建流失败 |
| `nvenc_nonref_p` / `nvenc_weighted_prediction` / `nvenc_ldkfs` | ✓ | ✓ | `nonref_p` / `weighted_pred` / `ldkfs` |
| `nvenc_vbv_increase` | ✓（**有**能力位检查） | ✓（**无**能力位检查） | 见 [§7.1](#71-放大机制一vbv-放大单帧窗口被撑大) |
| `nvenc_enable_min_qp` / `nvenc_min_qp_*` | ✓ | **✗ 完全不生效** | FFmpeg 选项表里没有 `qpmin` |
| `nvenc_enable_max_qp` / `nvenc_max_qp_*` | ✓ | **✗ 完全不生效** | FFmpeg 选项表里没有 `qpmax` |
| `nvenc_split_encode` | ✓ | **✗ 完全不生效** | 选项表里没有 split 相关项 |
| `nvenc_h264_cavlc` | ✓ | ✓ | `coder` |
| `nvenc_realtime_hags` / `nvenc_opengl_vulkan_on_dxgi` / `nvenc_latency_over_power` | ✓ | — | Windows 平台键，非编码参数 |

> ⚠️ **Linux 用户注意两点**：① `nvenc_enable_min_qp` / `nvenc_enable_max_qp` 这两组共 8 个键**能被解析、能通过校验、日志里也不报错，但一个字节都不会传到编码器**（纯静默无操作）；② `nvenc_aq_strength` 在 Linux 上必须在 **1–15** 之间，默认的 0 越界会让编码器直接起不来（[§4.2](#42-spatial_aq--aq_strength--temporal_aq自适应量化)）。

Windows 侧的码控全部集中在原生路径 [src/nvenc/nvenc_base.cpp:304](../../../src/nvenc/nvenc_base.cpp#L304) 起的 `configure_rate_control()`；Linux 侧的对应实现（含 VBV 放大）在 [src/video.cpp:2261](../../../src/video.cpp#L2261)。**同一个键在两条路径上的生效条件并不一致**，这是排查"Windows 有效、Linux 无效"类问题的起点。

### 1.3 关 3：能力位不满足时的表现

| 项 | 能力位 | Windows 原生 SDK | Linux FFmpeg |
|----|--------|-----------------|--------------|
| `nvenc_vbv_increase` | `NV_ENC_CAPS_SUPPORT_CUSTOM_VBV_BUF_SIZE` | ★ **整段 VBV 设置被跳过** → 键彻底无效，且日志里连 `vbv+N` 后缀都不打印 | 不做能力位检查，直接生效 |
| `nvenc_temporal_aq` | `NV_ENC_CAPS_SUPPORT_TEMPORAL_AQ` | 打一条 `gpu doesn't support temporal AQ` warning 后忽略 | **编码器初始化失败**（直接报错返回，不降级） |
| `nvenc_weighted_prediction` | `NV_ENC_CAPS_SUPPORT_WEIGHTED_PREDICTION` | 静默不设置 `enableWeightedPrediction` | **编码器初始化失败**（同上） |
| `nvenc_tuning = uhq`（H.264） | 语义不支持（非能力位） | 打 warning 回退 `hq` | **静默**把 `tune` 换掉 |

> ★ `nvenc_vbv_increase` 那一行是最贵的坑：它把"单帧 VBV"整块放在能力位判断里面，所以**老 GPU 上调这个键，看到的现象是"完全没变化"，而不是"画质变好了"**。

> ⚠️ **"静默失效"只是 Windows 的说法。** Linux 走 FFmpeg，对不满足的能力位是**直接报错返回**而不是降级，且 Sunshine 对 nvenc 不做重试 —— 所以 Linux 上这类键"要么完全生效、要么整个编码器起不来"。**排查顺序：先确认日志里编码器建起来了，再谈参数效果。**

### 1.4 关 4：来自客户端、配置里改不了的量

| 量 | 来源 | 被谁用 |
|----|------|--------|
| `bitrate` | 客户端协商（经 FEC / 音频 / 头部回算，见 [04 §1.1](./04-码率控制与QoS.md#11-configuredbitratekbps-的回算srcrtspcpp1257-起)） | `averageBitRate`、单帧 VBV 预算 |
| `framerate` | 客户端 | 单帧 VBV 预算、`frameRateNum/Den` |
| `slicesPerFrame` | 客户端 | `sliceModeData`；AV1 还据此推导 tile 行列数 |
| `numRefFrames` | 客户端 | DPB 大小（请求 >0 才用，否则用编码器默认） |
| `enableIntraRefresh` | 客户端 | 只有它要求且能力位支持时才开 intra-refresh |
| `videoFormat` | 客户端 | 决定走 H.264 / HEVC / AV1 分支，进而决定**哪些 QP 键被使用** |

---

## 2. 参数总表

23 个键。⚠️ 标记的是**本 fork 新增**（上游 `master` 没有），链接列指向逐项详解。

| 键 | 默认 | 取值范围 | 主要影响 | ⚠️ | 详解 |
|----|------|---------|---------|:--:|:----:|
| `nvenc_preset` | `1` | 1–7 | 编码速度 ↔ 压缩效率；**不改变码率** | | [§4.1](#41-preset--tuning--twopass编码速度与质量档) |
| `nvenc_tuning` | `ull` | `hq` / `ll` / `ull` / `uhq` | 延迟 ↔ 画质（编码器内部流水线策略） | ⚠️ | [§4.1](#41-preset--tuning--twopass编码速度与质量档) |
| `nvenc_twopass` | `quarter_res` | `disabled` / `quarter_res` / `full_res` | 画质 ↔ 占用 CUDA 核心 | | [§4.1](#41-preset--tuning--twopass编码速度与质量档) |
| `nvenc_spatial_aq` | `disabled` | bool | 低码率下帧内比特重分配（**分配方向文档自相矛盾**，见详解）；占用 CUDA 核心 | | [§4.2](#42-spatial_aq--aq_strength--temporal_aq自适应量化) |
| `nvenc_aq_strength` | `0` | 0–15 | AQ 强度；0 = 驱动默认 | ⚠️ | [§4.2](#42-spatial_aq--aq_strength--temporal_aq自适应量化) |
| `nvenc_temporal_aq` | `disabled` | bool | 运动场景画质 ↑；**与严格 CBR 冲突** | ⚠️ | [§4.2](#42-spatial_aq--aq_strength--temporal_aq自适应量化) |
| `nvenc_nonref_p` | `disabled` | bool | 静态内容省码率；**抗丢包 ↓** | ⚠️ | [§4.3](#43-nonref_p--weighted_prediction--ldkfs帧类型与参考行为) |
| `nvenc_weighted_prediction` | `disabled` | bool | 淡入淡出省码率；**仅 H.264**，且与 B 帧 / AQ / split encode 冲突 | ⚠️ | [§4.3](#43-nonref_p--weighted_prediction--ldkfs帧类型与参考行为) |
| `nvenc_ldkfs` | `0` | 0–10 | I/P 帧比特比；影响 keyframe 时的抖动 | ⚠️ | [§4.3](#43-nonref_p--weighted_prediction--ldkfs帧类型与参考行为) |
| `nvenc_vbv_increase` | `0` | 0–400（%） | 允许的**瞬时**码率放大倍数 | | [§4.4](#44-vbv_increase码率窗口) |
| `nvenc_enable_min_qp` | `disabled` | bool | 强制码率在 GOP 内摊平 | ⚠️ | [§4.5](#45-qp-上下限) |
| `nvenc_min_qp_h264` | `19` | 0–51 | 同上（H.264 标度） | ⚠️ | [§4.5](#45-qp-上下限) |
| `nvenc_min_qp_hevc` | `23` | 0–51 | 同上（HEVC 标度） | ⚠️ | [§4.5](#45-qp-上下限) |
| `nvenc_min_qp_av1` | `23` | 0–255 | 同上（AV1 8-bit 标度） | ⚠️ | [§4.5](#45-qp-上下限) |
| `nvenc_enable_max_qp` | `disabled` | bool | 难帧不糊；**会让编码器超发** | ⚠️ | [§4.5](#45-qp-上下限) |
| `nvenc_max_qp_h264` | `51` | 0–51 | 同上 | ⚠️ | [§4.5](#45-qp-上下限) |
| `nvenc_max_qp_hevc` | `51` | 0–51 | 同上 | ⚠️ | [§4.5](#45-qp-上下限) |
| `nvenc_max_qp_av1` | `255` | 0–255 | 同上 | ⚠️ | [§4.5](#45-qp-上下限) |
| `nvenc_split_encode` | `driver_decides` | `disabled` / `driver_decides` / `enabled` | 多 NVENC 单元并行 → **延迟 ↓**；**与 weighted_prediction 互斥** | | [§4.6](#46-split_encode--h264_cavlc格式与并行) |
| `nvenc_h264_cavlc` | `disabled` | bool | 关掉 CABAC，**纯损失** | | [§4.6](#46-split_encode--h264_cavlc格式与并行) |
| `nvenc_realtime_hags` | `enabled` | bool | Windows HAGS 下实时优先级 | | [§4.6](#46-split_encode--h264_cavlc格式与并行) |
| `nvenc_latency_over_power` | `enabled` | bool | 延迟优先于省电 | | [§4.6](#46-split_encode--h264_cavlc格式与并行) |
| `nvenc_opengl_vulkan_on_dxgi` | `enabled` | bool | 采集路径：把全屏 OpenGL/Vulkan 强制呈现到 DXGI 之上（系统级设置） | | [§4.6](#46-split_encode--h264_cavlc格式与并行) |

> ⚠️ **三个"默认值等于没开"的陷阱**：
> - `nvenc_max_qp_*` 的默认值（H.264/HEVC `51`、AV1 `255`）在各自标度下**已经是最差质量**，等于"不设上限"。打开 `nvenc_enable_max_qp` 而不同时下调数值，**行为与关闭完全一致**。
> - `nvenc_aq_strength = 0` 的含义是"用驱动的默认强度"，**不是"不用 AQ"**。要真正关掉 AQ 得把 `nvenc_spatial_aq` / `nvenc_temporal_aq` 设为 `disabled`。
> - `nvenc_split_encode = enabled` 与 `nvenc_weighted_prediction = enabled` 一起写，**两个都会失效**（SDK 明文互斥），而日志里两个开关都会照常打印。详见 [§6 避雷第 2 条](#6-避雷清单)。

---

## 3. Sunshine 故意锁死的项（`nvenc_config` 里没有对应键）

这些值写死在 [src/nvenc/nvenc_base.cpp](../../../src/nvenc/nvenc_base.cpp)，**配置文件改不了**。知道它们锁了什么，才能判断"哪些调优在 Sunshine 里根本做不了"。

| 锁死项 | 值 / 位置 | 为什么这样锁 |
|--------|-----------|-------------|
| `rateControlMode` | **`NV_ENC_PARAMS_RC_CBR`**（[:311](../../../src/nvenc/nvenc_base.cpp#L311)） | 直播串流必须码率可预测；VBR 的峰值会撞爆家庭网络 |
| `frameIntervalP` | **`1`（无 B 帧）**（[:310](../../../src/nvenc/nvenc_base.cpp#L310)） | 有 B 帧就有重排序缓冲 = 固定增加 ≥1 帧延迟，且和 `zeroReorderDelay` 自相矛盾 |
| `zeroReorderDelay` | **`1`**（[:312](../../../src/nvenc/nvenc_base.cpp#L312)） | 编码器发一帧收一帧，Sunshine 的推流管线是同步的 |
| `enableLookahead` | **`0`**（[:313](../../../src/nvenc/nvenc_base.cpp#L313)） | Lookahead 需要预看未来帧 → 必然引入延迟 |
| `gopLength` / `idrPeriod` | **`NVENC_INFINITE_GOPLENGTH`**（[:309](../../../src/nvenc/nvenc_base.cpp#L309)、H.264 [:428](../../../src/nvenc/nvenc_base.cpp#L428)、HEVC [:466](../../../src/nvenc/nvenc_base.cpp#L466)、AV1 [:510](../../../src/nvenc/nvenc_base.cpp#L510)） | **不做周期性 IDR**。关键帧请求完全交给客户端的 `IDX_REQUEST_IDR_FRAME`，避免"定时大帧"制造规律性卡顿 |
| `lowDelayKeyFrameScale` | 只在 `nvenc_ldkfs > 0` 时才覆盖（[:315](../../../src/nvenc/nvenc_base.cpp#L315)），否则**保留驱动从 tuning info 推导的值** | 驱动会按 tuning 给出合理值（LL→2、ULL→1）；写死会反过来干扰 `nvenc_tuning` |
| `repeatSPSPPS` / `repeatSeqHdr` | **`1`**（H.264 [:433](../../../src/nvenc/nvenc_base.cpp#L433)、HEVC [:471](../../../src/nvenc/nvenc_base.cpp#L471)、AV1 [:513](../../../src/nvenc/nvenc_base.cpp#L513)） | 丢包后中途加入/恢复必需的参数集重复 |
| `sliceMode` / `sliceModeData` | **`3`（按 slice 数切）/ `client_config.slicesPerFrame`** | slice 数由客户端决定，是为了配合 UDP 分片大小与 FEC 粒度 |
| `list0_option` | **恒为 `NV_ENC_NUM_REF_FRAMES_1`**（`configure_reference_frames()`） | 每帧只参考前一帧以压低延迟；DPB 里多留几帧只是为了 RFI 回退有帧可用 |
| `intraRefreshPeriod` / `Cnt` | **`300` / `299`**，且仅在客户端要求 + 能力位支持时启用 | 由客户端 `enableIntraRefresh` 触发，Sunshine 不主动开 |
| `enablePTD` | **`1`** | 让驱动决定每帧的 picture type（只有在这套"无 B 帧 + CBR"配置下才安全） |
| `averageBitRate` | **`client_config.bitrate * 1000`**（[:341](../../../src/nvenc/nvenc_base.cpp#L341)） | 直接来自协商结果，**不接受任何配置缩放** |
| `vbvBufferSize` | **`bitrate*1000 / framerate`**（[:343](../../../src/nvenc/nvenc_base.cpp#L343)）= **恰好一帧的比特预算** | 这是全系统最小延迟的发包曲线；`nvenc_vbv_increase` 是唯一松开它的口子 |
| `insert_filler_data` | 结构体里有字段，**但当前工作树没有对应配置键** | 仅用于测试（CBR 填充到恒定帧大小），无用户价值 |
| AV1 tile 切分 | `numTileRows = 2^⌈log2(slices)/2⌉`、`numTileColumns = 2^⌊log2(slices)/2⌋`，由 `slicesPerFrame` 推导 | 手工指定 tile 网格会与 slice 数打架，交给推导更稳 |
| 硬件 profile | H.264：YUV444→`HIGH_444`，否则 `HIGH`；逐编码器覆写 | 由采集缓冲格式决定，不是用户选项 |

> ★ **一句话总结**：Sunshine 的 NVENC 是**"无限 GOP + 无 B 帧 + 零重排序 + 无 lookahead + 单帧 VBV 的 CBR"** 这一整套固定骨架。你能调的只是**在这套骨架里怎么分配比特**，不能换骨架。

---

## 4. 参数详解

### 4.1 `preset` / `tuning` / `twopass`：编码速度与质量档

#### `nvenc_preset`（1–7，默认 1）

- **是什么**：NVENC 的**预置配置档**。数字越大 → 编码器用越多计算资源找更优的运动估计/模式决策 → 同码率下画质更好，但**单帧编码耗时更长**。
- **为什么需要设**：它是"画质 vs 编码延迟/GPU 占用"的总开关，也是唯一能同时影响两者的档位键。
- **改了有什么效果**：
  - P1 → P7：画质 ↑、编码耗时 ↑；**请求码率完全不变**（`averageBitRate` 与 preset 无关）。
  - 实测影响排序：在 [TODO/01 的差异分析](<../../01/Rustdesk 和 Sunshine 延迟 带宽差异.md>) 里，preset 对"同码率画质"的影响排在第 5 位，**远小于编码格式（H.264 vs HEVC/AV1）、分辨率、帧率、tuning 这几项**。
- **什么时候该动**：客户端/网络是瓶颈时**不要动它**（官方文档口径：只在网络或解码器受限时再调）；GPU 有富余、且你在意文字锐度/暗部细节时才往 4–7 走。
- **副作用**：高 preset + 高分辨率高帧率会让单帧编码时间逼近甚至超过帧间隔（4K120 时 8.33 ms），此时**延迟不是来自码率而是来自编码器吞吐**。
- **⚠️ 一条不写在 Sunshine 文档里的 preset 副作用**：SDK 头文件说明 **P3–P7 在 `hq` / `lossless` 档下会内部启用 B 帧**（`nvEncodeAPI.h` 中 `NV_ENC_PRESET_P1_GUID` 附近的注释：*"Presets P3 to P7 for H264 and Presets P2 to P7 for HEVC have B frames enabled by default for HIGH_QUALITY and LOSSLESS tuning info"*）。Sunshine 把 `frameIntervalP` 硬编码为 1（[:310](../../../src/nvenc/nvenc_base.cpp#L310)），**所以这条不会给你带来 B 帧延迟** —— 但它会牵连 `nvenc_weighted_prediction` 的合法性判定（见 [§4.3](#43-nonref_p--weighted_prediction--ldkfs帧类型与参考行为)）。

#### `nvenc_tuning` ⚠️（`hq` / `ll` / `ull` / `uhq`，默认 `ull`）

- **是什么**：向驱动声明"这个流的优化目标"。它在底层同时做两件事：
  1. 决定 `NV_ENC_INITIALIZE_PARAMS::tuningInfo`；
  2. 作为 `nvEncGetEncodePresetConfigEx()` 的输入，**改变 preset 基线配置本身**（不只是加个开关）。
- **为什么需要设**：`preset` 只说"花多少算力"，`tuning` 才说"算力花在哪"。低延迟场景必须显式选 `ull`，否则驱动按 `hq` 给你一套追求压缩率的基线（更多参考帧、更激进的运动搜索），延迟会明显高于预期。
- **取值语义**：

  | 值 | 定位 | 延迟 | 同码率画质 | 典型用途 |
  |:--:|------|:----:|:---------:|---------|
  | `hq` | High Quality | 最高 | 最好 | 录屏、本地高质量归档 |
  | `ll` | Low Latency | 中 | 中 | 局域网高刷游戏（帧时间敏感但对 1 帧差异不敏感） |
  | `ull` | **Ultra Low Latency**（默认） | **最低** | 最低（同码率下需多 20–50% 码率才能追平 hq） | 云游戏 / 网络串流 |
  | `uhq` | Ultra High Quality | — | 最高 | 4K HDR 高质量串流 |

- **为什么默认是 `ull`**：Sunshine 的定位是"实时串流"，UL 档会缩短编码器内部流水线、减少参考帧依赖，代价是同画质需要更高码率 —— 而这个代价在"码率可协商"的场景里是划算的。
- **⚠️ 不可用组合**：`uhq` + **H.264**。
  - Windows 原生路径：打 warning `ultra high quality tuning is not supported for H.264, using high quality tuning`，回退 `hq`（[`tuning_info_from_config()`](../../../src/nvenc/nvenc_base.cpp#L213)）。
  - Linux FFmpeg 路径：**静默**把 `tune` 从 `ULTRA_HIGH_QUALITY` 换成 `HIGH_QUALITY`（[src/video.cpp:849](../../../src/video.cpp#L849) 的 lambda），日志无任何提示。
  - 换句话说：**H.264 上写 `uhq` = 写 `hq`，但你在一条路径上会被警告、在另一条上完全不知道。**
- **⚠️ 前置条件**：`uhq` 还要求 **HEVC/AV1 + Turing 及更新**的 GPU。
- **改动副作用**：`tuning_info` 属于**只能在初始化时设定**的字段 —— 改它必须**重建编码会话**（活动会话热重载不生效）。另外它会重设 `lowDelayKeyFrameScale` 的驱动默认值，所以调 `nvenc_ldkfs` 之前先确定 `tuning`。

#### `nvenc_twopass`（`disabled` / `quarter_res` / `full_res`，默认 `quarter_res`）

- **是什么**：两遍编码（多遍率失真优化）。
  - `quarter_res`：用 1/4 分辨率的帧做第一遍统计 → 便宜，已经是收益的绝大部分。
  - `full_res`：全分辨率第一遍 → 更准，成本高得多。
- **为什么需要设**：第一遍统计能让码控把比特分得更准（尤其是 GOP 内比特分配），这是"同码率下画质变好"的最廉价手段之一。
- **副作用（关键）**：**两遍编码占用 CUDA 核心**（源码注释原文口径），也就是说它会**和游戏抢 SM**。在 GPU 已经满载的游戏场景里，这会把"编码变快"变成"游戏掉帧 + 帧时间抖动"。
- **建议**：默认 `quarter_res` 是很好的折中；`full_res` 只在 GPU 有余量且你追求画质时用；**笔记本/Optimus 或 GPU 满载时应该改成 `disabled`**。

### 4.2 `spatial_aq` / `aq_strength` / `temporal_aq`：自适应量化

#### `nvenc_spatial_aq`（默认 `disabled`）

- **是什么**：空间 AQ。逐帧分析画面的**局部复杂度**并生成一张 QP 映射表，据此在**帧内重分配比特**。范围必须说清楚：**它不改变帧的目标码率**（CBR + 单帧 VBV 仍然管着总量），只改变"同一帧内哪些区域多分、哪些区域少分"。
- **⚠️ 比特往哪个方向分：本仓库里的两份文档说的是相反的**。本 fork 的 `src/nvenc/nvenc_config.h` 字段注释写 *"Allocate more bitrate to flat regions since they're visually more perceptible"*（**平坦区多给**比特）；而 WebUI 与 `docs/configuration.md` 的说明写 *"Assign higher QP values to flat regions of the video"*（**平坦区更高 QP = 少给**比特）。SDK 头文件本身只写 `enableAQ` = "enable adaptive quantization (Spatial)"、`aqStrength` 1–15，**不给方向**。因此：**不要指望从文档推出方向**，理解成"帧内比特重分配"即可 —— 这也正是它不该被当成"必开优化"的原因。
- **为什么需要设**：低码率下帧内比特分配很粗，若纯粹按"复杂度"分配，平坦区域（大面积纯色、渐变、UI 背景）会首先出现色带/块状 —— AQ 让码控把"人眼对哪里更敏感"纳入分配。官方说明的口径就是 *"Recommended to enable when streaming at lower bitrates"*。
- **代价**：占用 CUDA 核心（源码注释明确标注）。
- **效果量级**：低码率（≤15 Mbps @1080p）时肉眼可见；高码率（≥40 Mbps）时收益趋近于零，纯白付 CUDA 算力。

#### `nvenc_aq_strength` ⚠️（0–15，默认 0）

- **是什么**：AQ 的强度。**0 表示"用驱动的默认强度"，不是"关掉 AQ"**。
- **为什么需要设**：驱动默认强度是为"通用场景"选的。桌面/UI 内容需要更弱的 AQ（过强会让文字边缘被"抹平"）；电影/游戏远景需要更强的 AQ。
- **改了有什么效果**：强度 ↑ → 平坦区域更干净、纹理区更糊；强度 ↓ → 纹理更锐、平坦区域更容易出色带。
- **⚠️ 三个坑**：
  1. **只写 `nvenc_aq_strength` 而不开 `nvenc_spatial_aq` / `nvenc_temporal_aq`，完全没有效果** —— 但日志里照样会打印 `aq-strength=9`（[src/nvenc/nvenc_base.cpp:633](../../../src/nvenc/nvenc_base.cpp#L633)），非常容易误判成"已经生效"。
  2. 该字段在 SDK 里是 **4 bit**，代码用 `std::min(..., 15)` 夹紧（[:331](../../../src/nvenc/nvenc_base.cpp#L331)）。配置校验的范围也是 0–15，两层一致，所以这里不会出问题 —— 但不要以为"填 30 会变成 15"，校验层会先把整项丢掉。
  3. **⚠️ Linux 上必须给 1–15。** FFmpeg 里 `aq-strength` 的合法范围是 1–15，默认的 0 越界；这项无论是否开 AQ 都会被下发，所以 0 会让编码器直接建不起来 —— 不想要 AQ 就填 `1`。

#### `nvenc_temporal_aq` ⚠️（默认 `disabled`）

- **是什么**：时间自适应量化。看**帧间**：在时间上做平滑，把比特集中到"这一帧变化大"的区域。
- **为什么需要设**：快速的镜头运动、粒子特效、水面等高时间复杂度的场景画质会明显改善。
- **⚠️ 为什么默认关**：源码注释原话 —— **"may conflict with strict CBR"**。它会在 GOP 内主动重排比特分布，而 Sunshine 是严格 CBR + 单帧 VBV；两者叠加会让帧大小波动变大，撞上单帧 VBV 上限后反而产生画质抖动。
- **另需**：能力位 `NV_ENC_CAPS_SUPPORT_TEMPORAL_AQ`；不支持时打 warning 忽略。
- **代价**：占用 CUDA 核心。

> **三者的关系**：`temporal_aq` 和 `spatial_aq` 可以同时开（分别管"帧间"和"帧内"），但**同时开会把 CUDA 占用叠加**，且两者都会与单帧 VBV 的严格律冲突。低延迟游戏场景建议**只开 spatial**，或者都关掉换稳定的帧时间。

### 4.3 `nonref_p` / `weighted_prediction` / `ldkfs`：帧类型与参考行为

#### `nvenc_nonref_p` ⚠️（默认 `disabled`）

- **是什么**：允许编码器把一部分 P 帧标成"非参考帧"（后面没有帧参考它）。
- **为什么需要设**：静态内容（办公桌面、文档、终端）里，一帧画面常常和上一帧几乎一样但又必须发出去；非参考帧可以用更少的比特编码这类帧 → **省码率**。
- **效果**：静态/低运动内容码率明显下降；快节奏游戏里收益接近 0（没有"可省"的静态帧）。
- **⚠️ 代价（官方 warning 口径）**：非参考帧自身**不能作为后续帧的恢复点**，所以**对丢包更敏感**。在已经有丢包的链路上开它，会让"一次丢包影响几帧"变成"影响更多帧"。
- **⚠️ 前置条件（SDK 原文）**：`enableNonRefP` 的注释写 *"no effect if enablePTD=0"*。Sunshine 在 [:740](../../../src/nvenc/nvenc_base.cpp#L740) 把 `enablePTD` 设为 1，所以这个键在 Sunshine 里**确实生效** —— 但如果你在别处看到"开了 nonref_p 没变化"，先查 PTD 是否被关掉。
- **结论**：有线局域网、办公/桌面串流可以开；弱网/无线/公网**不要开**。

#### `nvenc_weighted_prediction` ⚠️（默认 `disabled`）

- **是什么**：加权预测。编码器对参考帧做加权平均来预测当前帧，对**淡入淡出**（fade in/out）这类全局亮度变化的场景压缩率提升很明显 —— 没有它时每个像素都在"变"，编码器只能老老实实发残差。
- **为什么需要设**：游戏里过场动画、加载画面、HDR 场景切换常见淡入淡出，这个开关能省下可观的码率。
- **⚠️ 限制与冲突（每条都给出处）**：
  - **只有 NVENC H.264 编码器支持**（WebUI 与官方键文档口径）。原生路径还要求能力位 `NV_ENC_CAPS_SUPPORT_WEIGHTED_PREDICTION`，不支持时**静默不设**（[:742](../../../src/nvenc/nvenc_base.cpp#L742)）。
  - **与 B 帧冲突（SDK 原文）**：`enableWeightedPrediction` 的注释写明 *"Not supported if encode session is configured for B-Frames (i.e. frameIntervalP > 1 **or preset >= P3 when tuningInfo = HIGH_QUALITY or LOSSLESS**)"*。Sunshine 把 `frameIntervalP` 硬编码为 1（[:310](../../../src/nvenc/nvenc_base.cpp#L310)），这条在最终会话里被压掉了；但 **SDK 把"P3 以上 + HQ 档"单独列为构成 B 帧的条件**，说明这半条要由驱动来解释 preset。保守做法：开了加权预测就**别同时用 `hq` / `uhq` 档 + P3 以上**。
  - **与自适应量化冲突**：Sunshine 的官方键文档写 *"it conflicts with B frames and with adaptive quantization"*，但 SDK 头文件**只有 B 帧那一条**，没有 AQ 互斥的条文。也就是说这一条的后果**以驱动实际行为为准**，而 Sunshine **不做任何交叉校验** —— 你同时开两者不会收到报错，日志里 `weighted-prediction` 和 `spatial-aq` 会并排出现，看不出谁生效。**要加权预测就别要 AQ，二选一。**
  - **与 split encode 冲突（SDK 明文）**：`splitEncodeMode` 的注释写 *"Not supported if any of the following features: weighted prediction, ..."*，详见 [§4.6](#46-split_encode--h264_cavlc格式与并行)。
  - **Linux 差异（不是"取决于驱动"，是硬失败）**：`weighted_pred` 在 FFmpeg 路径上对 **AV1 / HEVC / H.264 三种编码器都会被写入**（[video.cpp:796](../../../src/video.cpp#L796)、[:823](../../../src/video.cpp#L823)、[:861](../../../src/video.cpp#L861)），而 FFmpeg 在 `weighted_pred > 0` 且能力位 `NV_ENC_CAPS_SUPPORT_WEIGHTED_PREDICTION` 查询失败时**直接返回 `AVERROR(ENOSYS)`**（[nvenc.c:509](../../../third-party/build-deps/third-party/FFmpeg/FFmpeg/libavcodec/nvenc.c#L509)），且 Sunshine 的 nvenc `fallback_options` 是空的（无重试）→ **编码器直接起不来**。能力位按 codec guid 查询，所以"只有 H.264 支持加权预测"这个原生口径意味着 **Linux 上在 HEVC/AV1 开它很可能是初始化失败，而不是被忽略** → **建议只在 H.264 下开这个键**。
- **代价**：占用 CUDA 核心。

#### `nvenc_ldkfs` ⚠️（0–10，默认 0）

- **是什么**：`lowDelayKeyFrameScale` —— **I 帧与 P 帧的比特比**（相对值）。它决定"关键帧允许多花几倍于普通帧的比特"。
- **为什么需要设**：I 帧天然比 P 帧大得多。在严格 CBR + 单帧 VBV 下，编码器必须压小 I 帧才能塞进一帧的预算，于是 I 帧质量被牺牲；把它调大就是"允许 I 帧超发"。
- **默认值 0 的含义（重要）**：**0 = 保留驱动从 tuning info 推导出的值**（LL 档通常给 2，ULL 档通常给 1），不是"用 1"。
- **改了有什么效果**：
  - 调大（2→10）：关键时刻（新会话开始、丢包后请求 IDR）画面更干净，但**那次关键帧会明显超发**，在配速窗口内造成一次突发。
  - 调小到 1：关键帧被压得很狠 → 会话刚开始的几百毫秒画面糊；但对网络最友好。
- **⚠️ 为什么会在这个 fork 里被暴露**：在此之前它被**硬编码成 1**，结果反复覆盖驱动按 tuning 推导的值，导致 `nvenc_tuning = ll` 想按 `ldkfs = 2` 工作却做不到 —— 也就是 **`nvenc_tuning` 部分失效**。现在的正确用法是：**改了 `tuning` 之后把 `ldkfs` 留 0，让驱动自己配**。
- **⚠️ 有效范围**：该字段只在**单帧 VBV + CBR** 的语义下由驱动使用。一旦你为了画质把 `nvenc_vbv_increase` 开大，单帧窗口被撑宽，调 `ldkfs` 的意义就被 VBV 的放宽淹没了。

### 4.4 `vbv_increase`：码率窗口

- **是什么**：把 NVENC 的 **VBV/HDR 缓冲**放大若干百分比。默认缓冲 = **恰好一帧的比特数**（`bitrate*1000/framerate`，[:343](../../../src/nvenc/nvenc_base.cpp#L343)）。设成 100 就是"允许某帧用掉两帧的预算"。
- **为什么它是全局最贵的旋钮（双向的）**：
  - **为什么默认是单帧**：缓冲越小，编码器越不敢攒比特 → 每帧大小越接近恒定 → 发包曲线越平滑 → 网络抖动越小。这是低延迟串流的正统做法。
  - **为什么又需要松开它**：单帧预算在**难帧**（爆炸、粒子、镜头快速横扫、HUD 全屏刷新）下会逼编码器把 QP 拉到很高 → 那一帧变成"糊成一坨"。给一点缓冲能让难帧借用前后帧的预算。
- **改了有什么效果**：
  - 画质：难帧明显改善，尤其配合 `nvenc_enable_max_qp`。
  - **延迟**：本身不增加平均延迟（CBR 平均目标不变），但**增加瞬时突发**。
  - **码率使用**：峰值 = `单帧预算 × (1 + vbv_increase/100)`。
- **⚠️ 上限为什么是 400**：400% 也只是"一帧 + 四帧"，即 **5 倍单帧预算**。它**做不到** FFmpeg 那种"多秒缓冲"的 VBR 效果 —— `nvenc_vbv_increase` 是"帧级余量"，不是"秒级余量"。想靠它替代 VBR 是方向性错误。
- **⚠️ 静默失效**：能力位 `NV_ENC_CAPS_SUPPORT_CUSTOM_VBV_BUF_SIZE` 不支持时整段跳过（[§1.3](#13-关-3能力位不满足时的表现)）。
- **⚠️ Linux 差异**：FFmpeg 路径（[src/video.cpp:2261](../../../src/video.cpp#L2261)）**不做能力位检查**，直接往 `rc_buffer_size` 上加。同一份配置在两条路径上的实际窗口可能不同。

### 4.5 QP 上下限

这一组共 8 个键，是本 fork 新增的，也是**唯一能直接和码控"顶牛"的参数**。

#### `nvenc_enable_min_qp` + `nvenc_min_qp_{h264,hevc,av1}`（默认关；19 / 23 / 23）

- **是什么**：给 QP 设**下限**，也就是"不许画质好到这个程度以上"。
- **为什么要设**（官方口径）：**限制编码器允许花在峰值画质上的比特数，让码率在 GOP 内摊得更均匀。当码率控制器跟不上极低的请求码率时有用**，代价是最简单的那些帧画质下降。
- **直觉解释**：静态画面太容易编码了，编码器会把它编到 QP 15（几乎无损）—— 这在"总预算固定"的前提下是**浪费**。把下限拉到 QP 24，省下的比特就能留给后面真正困难的帧。
- **改了有什么效果**：
  - 向上调：码率曲线更平稳（对网络友好）；**静态画面（比如办公桌面的纯色背景）会开始有可见的块状/色带**。
  - 向下调（趋向 0）：等于关闭，编码器重新获得"烧比特买画质"的自由。
  - 对峰值码率：**降低**（这是它和 `max_qp` 最重要的区别）。
- **默认值差别的原因**：H.264 与 HEVC **共用同一个 0–51 标度**；**AV1 用的是 0–255 的 qindex**，是另一把尺子，所以默认值给了 23。更要注意这三个默认值在**各自标度上的位置完全不同**：19/51 ≈ 37%、23/51 ≈ 45%、23/255 ≈ **9%** —— **`min_qp_av1` 的默认值几乎不构成限制**（详见 [§4.5.1](#451-av1-的-qp-标度不可按数字换算)）。
- **⚠️ 调过头会"欠码"**：`min_qp` 是硬下限，编码器算出更低的 QP 也没用。把三档推到 40+，会让码率**无论内容多难都上不去** —— 实测 20 Mbps 目标下 `qmin=40 qmax=51` 只跑出 **1.98 Mbps（0.10×）**，表现是"网络很空闲但画面就是糊"。
- **⚠️ 别把 min/max 写反**：配置层**没有** `min ≤ max` 的交叉校验（[src/config.cpp:1644](../../../src/config.cpp#L1644) 起八项各自独立检查），反着写（`min_qp=30` + `max_qp=25`）会被驱动照单接受，结果是码率**既到不了目标又剧烈抖动** —— 实测均值 3.83 Mbps（0.19×），单帧最大 414 kB ≈ **9.9×** 单帧预算。
- **⚠️ Linux 上不生效**（见 [§1.2](#12-关-2windows-与-linux-不是同一套实现)）。

#### `nvenc_enable_max_qp` + `nvenc_max_qp_{h264,hevc,av1}`（默认关；51 / 51 / 255）

- **是什么**：给 QP 设**上限**，也就是"不许烂到这个程度以上"。
- **为什么要设**（官方口径）：码控会一直抬高 QP 直到一帧塞进预算，于是一帧"编码器负担不起"的画面就被量化成一坨模糊。封住 QP 上限能保住这帧的可辨识度，**代价是让编码器突破单帧 VBV/HRD 上限**。所以官方建议**与 `nvenc_vbv_increase` 配合使用**。
- **改了有什么效果**：
  - 向下调（例如 HEVC 到 36–40）：难帧不再糊；但该帧大小**不可预测**，瞬时码率上不封顶。
  - 对峰值码率：**升高**（可能非常显著，见 [§7.2](#72-放大机制二max_qp-让编码器放弃码率约束)）。
  - 官方 warning：**Can lead to network packet loss**。
- **⚠️ 默认值是"无操作"**：51（H.264/HEVC）/255（AV1）已是各自标度下最差质量，"打开开关 + 保持默认"= 什么都没改。
- **⚠️ 两套标度不要混（本 fork 最容易被误用的一组键）**：真正的坑有两个方向，**两个方向都会让实际力度偏离预期**：
  - **照数字抄**（`max_qp_h264 = 38`，于是 `max_qp_av1 = 38`）：38 只占 0–255 的 15%，实测却等价于 H.264 的 **QP 29–30**，比你以为的"38"**严得多** → **超发比预期更狠**。
  - **按 ×5 折算**（38 × 5 = 190，而等价值只有 **≈142**）：上限**比预期松得多**，极端情况下几乎不再约束，等于把保险撤掉。
  另注意：`60` 在 AV1 上合法且"相当严"，而 H.264/HEVC 写 `60` 会被校验直接拒绝（范围 0–51）。换算表与实测见 [§4.5.1](#451-av1-的-qp-标度不可按数字换算)。
- **⚠️ Linux 上不生效**（见 [§1.2](#12-关-2windows-与-linux-不是同一套实现)）。

> **min / max QP 的分工一句话**：`min_qp` 是**刹车**（压峰值码率，牺牲简单帧），`max_qp` 是**保险**（压难帧的糊，牺牲码率纪律）。**两者方向相反，不要为了"画面更好"同时开。** 另外记住：**刹车和保险的力度都取决于数值在各自标度上的位置**，见下节。

#### 4.5.1 AV1 的 QP 标度：不可按数字换算

**结论先说**：H.264 与 HEVC 共用 **0–51** 标度（数字可互相参考，HEVC 同数值略更狠）；**AV1 是 0–255 的 qindex，是完全另一把尺子**。"H.264 用 38，AV1 也填 38"是错的，而且是**错向"更严"**的方向 —— 超发更厉害。

**为什么不能按比例换算**：QP 与量化步长是**指数**关系（约 `2^((QP-4)/6)`），而 NVENC 的 AV1 qindex 并非把 0–51 线性拉长：**低段步子细、高段步子粗**。于是"×5"这条常见经验**只在高端近似成立**：

| H.264/HEVC QP | 28 | 34 | 38 | 42 | 47 | 51 |
|---|---|---|---|---|---|---|
| 字节等价的 AV1 qindex | **≈28** | **≈100** | **≈142** | **≈178** | **≈215** | **≈242** |
| 比值（AV1 : H.264） | 1.0 | 2.9 | 3.7 | 4.2 | 4.6 | 4.7 |

（若用 ×5 去换算 H.264 的 `34`，会写成 `170`，而等价值只有 ≈100 → **上限会比你以为的松得多**。）

**实测依据**（同片源、`rc=constqp`、同 preset，平均字节/帧，720p）：

困难内容（逐帧随机噪声，最坏情况）：

| H.264 QP | 19 | 23 | 28 | 34 | 38 | 42 | 47 | 51 |
|---|---|---|---|---|---|---|---|---|
| H.264 B/帧 | 45651 | 37555 | 28049 | 18765 | 16087 | 13320 | 10428 | 8215 |
| HEVC B/帧 | 27312 | 24979 | 21697 | 18135 | 15980 | 13061 | 10152 | 7959 |

| AV1 qindex | 23 | 51 | 76 | 92 | 102 | 128 | 138 | 152 | 176 | 204 | 230 | 255 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| AV1 B/帧 | 29200 | 22784 | 20259 | 19192 | 18644 | 17120 | 16385 | 15356 | 13511 | 11359 | 9258 | 7270 |

简单内容（渐变 + 移动方块）：

| H.264 QP | 19 | 23 | 28 | 34 | 38 | 42 | 47 | 51 |
|---|---|---|---|---|---|---|---|---|
| H.264 B/帧 | 255 | 209 | 160 | 121 | 105 | 112 | 106 | 106 |

| AV1 qindex | 23 | 51 | 76 | 92 | 102 | 128 | 138 | 152 | 176 | 204 | 230 | 255 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| AV1 B/帧 | 890 | 292 | 234 | 181 | 197 | 125 | 119 | 136 | 140 | 96 | 85 | 89 |

两个直接推论：

- **AV1 的可用下限段（qindex ≈23）在简单内容上比 H.264 的 QP 19 还精细**（890 B vs 255 B）—— **AV1 低段基本是留给近无损场景的**，把 H.264 的低数值抄过去没有意义。
- **AV1 的最粗糙端（255）比 H.264 的最粗糙端（51）还要狠**（困难内容 7270 B vs 8215 B；简单内容 89 B vs 106 B）。所以**上限侧的默认值是等价的**（都是 100% 标度），**下限侧不是**。

**实践规则（三条）**：

1. **禁止换算，只认实测。** 改 AV1 的 QP 上下限时，用 [§5](#5-自检怎么确认参数真的生效了) 的 `qpmax=` 后缀确认已下发，再看帧大小/画质收敛。
2. **粗粒度起步值**：想要"H.264 38 的效果"→ AV1 给 **≈140**；想要"H.264 47"→ AV1 给 **≈215**。数字只能当起点。
3. **注意默认值位置**：`min_qp_h264 = 19`（标度 37%）、`min_qp_hevc = 23`（45%）、`min_qp_av1 = 23`（**9%**）。所以"打开 `nvenc_enable_min_qp` 但不动数值"在 **AV1 上几乎等于没设**。

> ⚠️ 这也解释了本文 [§8.2](#82-办公与桌面生产力) / [§8.4](#84-4k-hdr-高质量有线串流) 的 `max_qp_av1 = 60 / 56` 为什么要**重新实测定标**：按上表折算，`60` ≈ H.264 的 **30.5**（同组 `max_qp_hevc = 36` 的等位置本该 ≈118）、`56` ≈ **30**（同组 `max_qp_hevc = 34` 本该 ≈100）—— 两处 AV1 值都比同组 HEVC 值**严得多**，而"上限越严 → 超发越狠"，与配方"H.264 30 / HEVC 36 / AV1 60 表示同一严重程度"的书写意图不符。

### 4.6 `split_encode` / `h264_cavlc`：格式与并行

#### `nvenc_split_encode`（`disabled` / `driver_decides` / `enabled`，默认 `driver_decides`）

- **是什么**：**帧切分编码（Split Frame Encoding）** —— 把一帧切成多个条带，**同时喂给 GPU 上多个 NVENC 单元**并行编码，最后拼回一帧。
- **为什么需要设**：它直接砍**编码延迟**（一帧的编码时间除以并行度），是对"高分辨率 + 高帧率"最有效的手段，且**不改变码率与画质目标**。
- **改了有什么效果**：延迟 ↓；画质有极小的损失（条带边界处运动估计受限）；**码率基本不变**。
- **⚠️ 五重前置条件（不满足就静默无效）**：
  1. **仅 Windows**（Linux 走 FFmpeg，选项表里没有 split 项）。
  2. **NVENC SDK ≥ 13.0**；更老版本上 `enabled`（强制）只会打一条 log 然后忽略。
  3. **非 H.264**：`configure_split_frame()` 在 `client_config.videoFormat <= 0`（H.264）时**直接 return**。
  4. **GPU 上的 NVENC 单元数 > 1**（`NV_ENC_CAPS_NUM_ENCODER_ENGINES`）；单单元的卡上永远不生效。
  5. **不能与 `nvenc_weighted_prediction` 同时开**（SDK 明文）：`splitEncodeMode` 的注释写 *"Not supported if any of the following features: weighted prediction, alpha layer encoding, subframe mode, ..."*。这是"两个看起来无关的开关互相否决"的典型 —— 而且两种情况下都不会有配置层报错。
- **`driver_decides` 的真实行为**：驱动只在 **≥4K + P1–P4** 时自动开。所以"1080p 高刷 + 想开 split"必须用 `enabled` 强制，否则 `driver_decides` 会什么都不做。
- **建议**：默认值就很好。**1080p120 / 1440p144 这类"低分辨率高帧率"场景值得手动 `enabled`**（因为驱动不会帮你开），4K 交给驱动。

#### `nvenc_h264_cavlc`（默认 `disabled`，即用 CABAC）

- **是什么**：H.264 的熵编码模式切换。`disabled` = CABAC（算术编码，压缩率高），`enabled` = CAVLC（上下文自适应变长编码，压缩率低）。
- **源码注释原话**：这个选项"**不相关，仅为历史原因保留**"。
- **为什么不该开**：CABAC 相对 CAVLC 通常省 5–15% 码率，而**NVENC 的 CABAC 是硬件流水线实现的，不引入额外延迟**（软件 x264 时代 CAVLC 的"低延迟"理由在这里不成立）。
- **唯一合理用途**：某些老/劣质客户端解码器对 CABAC 支持有问题时作为兼容开关。
- **⚠️ 能力位回退**：驱动不支持 CABAC 时代码会自动改用 CAVLC（[:436](../../../src/nvenc/nvenc_base.cpp#L436)），与这个配置键无关。
- **结论：永远保持 `disabled`。**

#### `nvenc_realtime_hags` / `nvenc_latency_over_power` / `nvenc_opengl_vulkan_on_dxgi`

这三个是**Windows 平台相关键**，不是编码器参数，但它们的"坑值"和编码配置平级，所以放在这里：

| 键 | 默认 | 关掉会发生什么 |
|----|:----:|---------------|
| `nvenc_latency_over_power` | `enabled` | 驱动改为**省电优先**：为省电会在帧间降频/降低编码器占空比，**串流延迟明显上升**。笔记本电池模式下会被系统改写。**串流场景必须保持 enabled**。 |
| `nvenc_realtime_hags` | `enabled` | 关掉后在 HAGS（硬件加速 GPU 调度）开启的系统上，采集进程不再获得实时优先级 → **GPU 满载时抓帧延迟变大**（表现为"游戏里一转视角就卡"）。只有在 HAGS 下出现编码卡死/黑屏时才考虑关它。 |
| `nvenc_opengl_vulkan_on_dxgi` | `enabled` | 它控制是否把**全屏 OpenGL/Vulkan 程序的呈现方式改成 "Prefer layered on DXGI Swapchain"**，属于**采集路径**而非编码。关掉后，这类程序在 DXGI 捕获下**可能抓不到画面或只能低帧率抓取**（表现为黑屏/卡顿）。它是**系统级**改动，Sunshine 退出时会还原；只有在它和其他采集/HAGS 行为冲突时才关。 |

---

## 5. 自检：怎么确认参数真的生效了

调参最大的浪费是"改了、以为生效了、结果是静默失效"。**唯一的权威证据是启动串流后那一条 `NvEnc: created encoder` 日志。**

```
NvEnc: created encoder v1301 HEVC  P1 ull async two-pass vbv+50 ldkfs=2 rfi
                    weighted-prediction spatial-aq aq-strength=8 nonref-p
                    qpmin=24 qpmax=38 sfe
```

（该行由 [`log_created_encoder()`](../../../src/nvenc/nvenc_base.cpp#L660) 按固定顺序拼接；上面是把所有**可能**出现的后缀都拼满的示意 —— 实际不会同时出现这么多。）

**读法**：

| 后缀 | 含义 | 看不到说明什么 |
|------|------|---------------|
| `async` | 异步编码已启用 | 能力位不支持 → 事件句柄被置空 |
| `yuv444` / `10-bit` | 缓冲格式 | 采集格式如此 |
| `two-pass` | 两遍编码生效 | 配置为 `disabled` |
| `vbv+N` | ★ VBV 放大生效 | **能力位不支持**（此时 `nvenc_vbv_increase` 是死键） |
| `ldkfs=N` | 覆盖了 `lowDelayKeyFrameScale` | 配置为 0（= 用驱动默认值，**这是推荐值**） |
| `rfi` | 参考帧失效可用 | 能力位不支持 |
| `weighted-prediction` | 加权预测生效 | 配置关，或能力位不支持 |
| `spatial-aq` | 空间 AQ 生效 | 配置关 |
| `aq-strength=N` | ⚠️ **字段被写入**，不代表 AQ 已开 | 配置为 0 |
| `temporal-aq` | 时间 AQ 生效 | 配置关，或能力位不支持（会另打 warning） |
| `nonref-p` | 非参考 P 帧生效 | 配置关 |
| `qpmin=N` / `qpmax=N` | QP 上下限生效 | 配置关，或 **在 Linux 上运行**（永远不出现） |
| `filler-data` | 填充数据 | **永远不出现** —— `nvenc_config::insert_filler_data` 固定 `false` 且没有配置键（[:647](../../../src/nvenc/nvenc_base.cpp#L647)） |
| `sfe` / `sfe-auto` | 帧切分编码生效 | 单 NVENC 单元、H.264、或 SDK<13 |

**第二条要看的日志**：

```
NvEnc: requested encoded frame size 166.7 kB
```

（[src/nvenc/nvenc_base.cpp:772](../../../src/nvenc/nvenc_base.cpp#L772)）这就是**单帧比特预算** = `bitrate / 8 / framerate`。它同时告诉你两件事：编码器被要求维持什么码率、以及默认 VBV 窗口是多大。

**调参顺序（一次只动一个）**：

```
① 先定码率与分辨率/帧率（客户端侧）        ← 影响最大，先别碰编码器参数
② 再定 nvenc_tuning（决定骨架）+ preset     ← tuning 需要重建会话
③ 再考虑 twopass / spatial_aq（画质，占 CUDA）
④ 最后才动 vbv_increase + max_qp（码率纪律，风险最高）
```

**⚠️ `nvenc_tuning` 的生效时机**：`tuning_info` 只在 `NV_ENC_INITIALIZE_PARAMS` 里设定，**改它必须重建编码会话**。热重载配置不会作用于正在运行的会话。

---

## 6. 避雷清单

按"后果严重度"排序。**A 类 = 功能失效或直接报错；B 类 = 反效果或纯浪费；C 类 = 无效果但看着像生效了。**

| # | 类 | 组合 | 后果 | 机制 |
|:-:|:-:|------|------|------|
| 1 | A | `nvenc_weighted_prediction = enabled` + `nvenc_spatial_aq = enabled` | 二者之一被驱动忽略，且**配置层不会报错**：日志里 `weighted-prediction` 与 `spatial-aq` 并排出现，看不出谁生效 | Sunshine 官方键文档写 *"conflicts with adaptive quantization"*，但 SDK 头文件**只有 B 帧那一半**；Sunshine 不做交叉校验 → 后果以驱动实际行为为准（[§4.3](#43-nonref_p--weighted_prediction--ldkfs帧类型与参考行为)） |
| 2 | A | `nvenc_split_encode = enabled` + `nvenc_weighted_prediction = enabled` | **两个开关同时失效**，且没有任何报错 | SDK 明文：`splitEncodeMode` *"Not supported if any of the following features: weighted prediction, alpha layer encoding, subframe mode, ..."* → 这是"两个看起来无关的开关互相否决"的典型（[§4.6](#46-split_encode--h264_cavlc格式与并行)） |
| 3 | A | `nvenc_min_qp_*` > `nvenc_max_qp_*`（如 min 40 / max 30） | **驱动照单接受**：码率**既到不了目标又剧烈抖动** —— 实测均值 0.19× 目标，单帧 9.9× 预算，排障极难 | ✅ **Sunshine 的校验不做交叉检查**（[src/config.cpp:1635](../../../src/config.cpp#L1635) 每键独立校验），错配直接穿透到驱动（[§4.5](#45-qp-上下限)） |
| 4 | A | `nvenc_tuning = uhq` + H.264 | Windows：警告后回退 `hq`；Linux：**静默**回退 `hq` | H.264 不支持 UHQ 档；两条路径处理方式不一致（[:222](../../../src/nvenc/nvenc_base.cpp#L222) vs. [video.cpp:849](../../../src/video.cpp#L849)） |
| 5 | A | 在 Linux 上设 `nvenc_enable_min_qp` / `nvenc_max_qp` 及数值 | **完全无效果且无任何提示** | ① FFmpeg 选项表里没有 `qpmin`/`qpmax`，`nv_legacy` 也不携带这两个字段；② **即便补上也没用** —— CBR 下 FFmpeg 根本不映射 QP（`nvenc_override_rate_control` 的 `case NV_ENC_PARAMS_RC_CBR: break;`，[nvenc.c:946](../../../third-party/build-deps/third-party/FFmpeg/FFmpeg/libavcodec/nvenc.c#L946)），只有 `set_vbr()` 会映射 |
| 6 | A | `nvenc_enable_max_qp = enabled`（**不配** `nvenc_vbv_increase`） | 单帧 VBV 不再被遵守 → 难帧无限超发 → 分片丢弃/重传/请求 IDR（正反馈，见 [§7.2](#72-放大机制二max_qp-让编码器放弃码率约束)） | 官方 warning 原文：capping the QP … **lets the encoder overshoot the single frame VBV/HRD limit** |
| 7 | A | `nvenc_enable_max_qp = enabled` 但 `max_qp` 保持默认（51/51/255） | **等同于没开** | 上限侧三个默认值都是各自标度的 100%，语义相互等价。⚠️ **下限侧不是这样**：`min_qp_av1 = 23` 只占 0–255 的 **9%**，而 `min_qp_h264/hevc = 19/23` 占 **37%/45%** —— "打开开关但用默认值"在 AV1 上近乎空操作（[§4.5.1](#451-av1-的-qp-标度不可按数字换算)） |
| 8 | B | `nvenc_vbv_increase = 400` + 弱网/无线 | 单帧尖峰 = 5× 平均码率 → 交换机/路由器队列溢出 → 整队丢弃 | [05 §5](./05-FEC-重传-丢帧与心跳.md#5-丢帧没有显式策略只有队列覆盖) 的 `drop_oldest` 整队清空；官方 warning：Can lead to network packet loss |
| 9 | B | `nvenc_temporal_aq = enabled` + `nvenc_vbv_increase = 0` | 画质抖动，收益为负 | 源码注释：**may conflict with strict CBR**；单帧 VBV 下没有余地容纳它重排的比特 |
| 10 | B | `nvenc_twopass = full_res` + GPU 满载游戏 | **游戏掉帧**（不是串流掉帧） | 两遍编码占 CUDA 核心，与游戏抢 SM |
| 11 | B | `nvenc_spatial_aq = enabled` + `nvenc_temporal_aq = enabled` + `nvenc_twopass = full_res` | CUDA 占用三重叠加，帧时间抖动 | 三项都标注 "uses CUDA cores" |
| 12 | B | `nvenc_nonref_p = enabled` + 有丢包的链路 | 丢包影响帧数变多，恢复变慢 | 非参考帧不能作为后续帧的恢复点 |
| 13 | B | `nvenc_enable_min_qp` + 静态桌面/办公串流 + 下限调太高（≥30） | 纯色区域出现可见块状/色带 | min QP 的语义就是"不许画质好过这个程度" |
| 14 | B | `nvenc_ldkfs` 调大（≥6）+ 已有 `nvenc_vbv_increase > 0` | 关键帧超发，短暂卡顿；调它的收益被 VBV 放宽淹没 | `ldkfs` 的语义属于单帧 VBV CBR，VBV 一宽就失去参考系 |
| 15 | B | `nvenc_preset = 7` + `nvenc_tuning = uhq` + 4K120 | 单帧编码时间有**超过帧间隔（8.33 ms）的风险** → 编码器吞吐成为延迟瓶颈 | 与码率无关的**吞吐型延迟**，且会被误诊为"网络卡" |
| 16 | B | `nvenc_h264_cavlc = enabled` | **纯损失 5–15% 压缩率**，零延迟收益 | NVENC 的 CABAC 是硬件流水线，不增加延迟；源码注释："not relevant, here for historical reasons" |
| 17 | B | `nvenc_split_encode = enabled` + H.264 或单 NVENC 单元 GPU 或 Linux | 无效果（Windows 上静默；Linux 上更彻底地无效） | `videoFormat <= 0` 直接 return；`NUM_ENCODER_ENGINES <= 1` 直接 return；FFmpeg 路径无对应选项 |
| 18 | C（Windows）/ **A（Linux）** | 只写 `nvenc_aq_strength`，不设 `nvenc_spatial_aq` / `nvenc_temporal_aq` | Windows：**无效果**，但日志会打印 `aq-strength=N`；**Linux：编码器直接起不来**（默认值 0 越界，该项在 FFmpeg 里是 1–15） | 强度字段单独存在无意义；Linux 上则是"以为在调 AQ、结果建流失败"（[§1.2](#12-关-2windows-与-linux-不是同一套实现)） |
| 19 | C | `nvenc_vbv_increase` 在能力位不支持的 GPU 上 | 无效果，且日志连 `vbv+N` 都不打印 | 整段 VBV 代码在 `NV_ENC_CAPS_SUPPORT_CUSTOM_VBV_BUF_SIZE` 判断内 |
| 20 | C | `nvenc_tuning = uhq` 在 Turing 之前的 GPU（Pascal / Volta）上 | 驱动不认这一档 → 初始化失败或由驱动回退，取决于驱动版本 | Sunshine **没有任何 GPU 代数检查**，只对 H.264 做了回退（[`tuning_info_from_config()`](../../../src/nvenc/nvenc_base.cpp#L213)）；UHQ 是 Turing（第 5 代 NVENC）才引入的档位 |
| 21 | C | `nvenc_twopass` / `nvenc_split_encode` 写错枚举值 | 静默降级到默认（`quarter_resolution` / `driver_decides`）并打 warning | 枚举解析的默认分支行为 |

### 6.1 三组"看起来能配、实际上不该配"

| 想做的事 | 为什么 Sunshine 里做不到 |
|---------|------------------------|
| 用 VBR 换画质 | `rateControlMode` 硬编码 CBR（[:311](../../../src/nvenc/nvenc_base.cpp#L311)），没有对应配置键。`nvenc_vbv_increase` 只是"帧级余量"，不是 VBR |
| 开 B 帧省码率 | `frameIntervalP` 硬编码 1，且与 `zeroReorderDelay` 配套。开 B 帧必然加延迟，与整个骨架矛盾 |
| 让关键帧间隔规律（例如每 2 秒一个 IDR） | `gopLength` / `idrPeriod` 硬编码 `NVENC_INFINITE_GOPLENGTH`；关键帧完全由客户端的 `IDX_REQUEST_IDR_FRAME` 按需触发 |

---

## 7. 请求码率为什么"失控"

这一节回答"哪些参数错误调整会让码率失控"。先建立一个基线事实：

> **`averageBitRate` 严格等于 `client_config.bitrate * 1000`，且 `vbvBufferSize` 严格等于"一帧的比特数"。**（[:341](../../../src/nvenc/nvenc_base.cpp#L341)、[:343](../../../src/nvenc/nvenc_base.cpp#L343)）
> **平均码率这一条是严格的**：实测（20 Mbps 目标、1080p60、40 帧最坏情况噪声内容）平均值就是 20.00 Mbps，一帧不多。
> 但**"最坏不超过平均 + 一帧"只对 P 帧成立**：同一实测里单帧最大 **123 kB ≈ 3.0× 单帧预算**（预算 41.7 kB），典型来源是 IDR / 切屏那一帧。因为 VBV 缓冲只有一帧，这 3× 是**真实上链路的突发**，之后由编码器在数帧内偿还。
> 还有两个容易误解的事实：
> - **静态画面不会省下码率存款**：CBR 会把每帧填到预算，实测静止内容依然是 20.00 Mbps（且填充数据开/关对码流**逐字节相同**）。
> - 所谓"失控"（超过 3× 的量级），全部来自**人为松开这两个约束**，加上下面几个把请求值本身放大的机制。

### 7.1 放大机制一：VBV 放大（单帧窗口被撑大）

峰值码率的换算公式：

```
单帧预算 P = 客户端请求码率 / 8 / 帧率        （日志里的 "requested encoded frame size"）
实际单帧上限 = P × (1 + vbv_increase / 100)
瞬时峰值码率 ≈ 请求码率 × (1 + vbv_increase / 100)
（作用窗口只有一帧的时间：60 fps 时 16.7 ms）
```

数值示例（4K60，客户端报 80 Mbps，`fec_percentage = 20`）：

| 项 | 数值 |
|----|------|
| 客户端请求（网络总预算，含 FEC/音频/头） | 80 Mbps |
| FEC 回算后的**视频目标码率** | ≈ 80 ÷ (100/80) ≈ 64 Mbps，再扣音频与头部预留后 ≈ **63 Mbps** |
| 单帧预算 P | 63 Mbps ÷ 8 ÷ 60 fps ≈ **131 kB** |
| `nvenc_vbv_increase = 0`（默认）时单帧上限 | 131 kB |
| `nvenc_vbv_increase = 100` | 262 kB → 该帧约 **126 Mbps** |
| `nvenc_vbv_increase = 400` | 655 kB → 该帧约 **315 Mbps** |

> ⚠️ **注意这里是"瞬时"而不是"持久"**：CBR 的平均目标不变，超支的比特会在后续帧里被还回去。但网络设备（家用路由器、NIC 队列、客户端接收缓冲）只认**瞬时**。所以 315 Mbps 的尖峰打在一个 1 Gbps 链路上看似没问题，**真正的瓶颈是中间设备的缓冲深度**，而不是链路带宽。

FEC 回算的方向请注意：**它把编码目标码率调低**（因为请求值被理解为已经含 FEC 冗余），所以 `fec_percentage` 越大，编码器被要求的目标越低、而网络要承载的越高 —— 两者之间的差值就是留给 FEC 分片的余量。**松开 VBV 就是从这条余量里偷钱。**

### 7.2 放大机制二：`max_qp` 让编码器放弃码率约束

这是**唯一一种"持久"的超发**：

- 默认行为：难帧 → QP 被抬到最高 → 帧被压进预算 → **码率守得住，画质牺牲**。
- `nvenc_enable_max_qp = enabled` + `max_qp = 38`：难帧 → QP 到 38 就不再升 → **帧塞不进预算了，编码器选择超发**。

官方文档对它的描述就是这个意思：*capping the QP keeps such frames recognizable and instead **lets the encoder overshoot the single frame VBV/HRD limit***，并配了 warning *Can lead to network packet loss*。

**为什么它比 VBV 放大更危险**：VBV 放大的上限是 `1 + vbv_increase/100`（最多 5× 且有硬上限），而 `max_qp` 造成的超发**没有上限**，只取决于该帧在给定 QP 下需要多少比特（4K 复杂场景单帧 500 kB 完全可能）。

**正反馈环（真实故障模式）**：

```
难帧超发
   └─► 超出配速窗口能摊平的突发      （[04 §6](./04-码率控制与QoS.md#6-帧内发包配速pacing)）
         └─► 路由器/客户端队列溢出，分片丢弃   （[05 §5](./05-FEC-重传-丢帧与心跳.md#5-丢帧没有显式策略只有队列覆盖)）
               └─► FEC 修不回来 → 客户端请求 IDR（关键帧）
                     └─► IDR 本身就是最大的帧 → 再超发一次
                           └─► 回到第一步
```

这个环一旦建立，表现是"画面糊一下 → 全屏花一下 → 恢复 → 再花"，且 `nvenc_max_qp` 关掉之前不会自愈。

### 7.3 放大机制三：`nvenc_vbv_increase` 与 `max_qp` 的乘积

两者叠加不是相加而是相乘。上例（4K60、131 kB 基线）：

| 配置 | 单帧上限 | 折算瞬时（该帧） |
|------|---------|-----------------|
| 默认 | 131 kB | 63 Mbps |
| `vbv_increase = 200` | 393 kB | 189 Mbps |
| `vbv_increase = 200` + `max_qp_hevc = 38` | **可达 500 kB+** | **240 Mbps+** |

再叠加客户端侧：空调链路的实际载荷还要加上 FEC 分片（本例 20% 冗余）与音频流。**这就是"请求码率明明只有 80 Mbps，但交换机端口在闪红"的完整解释。**

### 7.4 放大机制四：FEC 与音频的预算回算

`fec_percentage` 不是一个"网络层参数"，它会**直接改变编码器的目标码率**（[04 §1.1](./04-码率控制与QoS.md#11-configuredbitratekbps-的回算srcrtspcpp1257-起)）。所以：

> **调 FEC = 调编码码率。** 把 `fec_percentage` 从 20 提到 50，"免费"的纠错能力是用编码码率换来的 —— 在同一个客户端请求值下，视频目标码率会下降约 37%，画质同步下降。这不是坑，但**必须知道自己付了什么**。

### 7.5 ⚠️ 放大机制五：`nvenc` 原生路径不受 `max_bitrate` 约束

[04 §1.2](./04-码率控制与QoS.md#12-服务端封顶) 已经标出这个 fork 特性：服务端封顶 `config::video.max_bitrate` **只在 AVCodec 路径生效**，`configure_rate_control()` 里用的是裸的 `client_config.bitrate`。

**后果**：如果你依赖 `max_bitrate` 作为"绝对不可逾越的码率上限"（例如出租屋共享带宽、校园网限速），那么**在 Windows 上走原生 NVENC 路径时这个上限根本不存在**。要做硬性封顶，必须用 `nv_legacy`（AVCodec 路径）或从客户端侧限制请求码率。

### 7.6 一句话诊断表

| 现象 | 最可能的旋钮 |
|------|-------------|
| 平时不卡，遇到爆炸/粒子/镜头横扫就花屏一下 | `nvenc_enable_max_qp`（超发 → 丢包） |
| 链路带宽够但断续出现整帧花屏 | `nvenc_vbv_increase` 过大（单帧尖峰撞队列） |
| 静态画面也花、滚屏花 | `nvenc_enable_min_qp` 下限过高 |
| 请求码率被"放大"看不出来但有规律 | `fec_percentage` 回算 |
| 编码器正常建流、但码率**永远到不了目标**且数值一直在跳 | 反向 QP（`min_qp_*` > `max_qp_*`）或 `min_qp` 下限过高 —— 实测均值低至目标的 **0.10×**，同时单帧还能冲到 **9.9×** 预算 |
| AV1 的码率上限"力度不对"（比预期更松或更严） | ⚠️ 两套标度混用：把 H.264/HEVC 的 QP 数值直接抄给 `*_av1`，或按 ×5 折算（[§4.5.1](#451-av1-的-qp-标度不可按数字换算)） |
| 客户端设了码率上限但不生效 | ⚠️ 原生 NVENC 路径 + `max_bitrate` |

---

## 8. 场景推荐配置

以下配方都按"**可直接粘贴进 `sunshine.conf`**"给出，只列与本文相关的键。**其余键保持默认。**

### 8.0 通用安全基线

**目标**：不知道选什么时的起点 —— 让编码器对网络做出"不会超发"的承诺。

```ini
# 低延迟总纲：不碰码率纪律（vbv_increase = 0），不做质量上限（max_qp 关）
nvenc_preset = 1
nvenc_tuning = ull
nvenc_twopass = quarter_res
nvenc_spatial_aq = disabled
nvenc_temporal_aq = disabled
nvenc_vbv_increase = 0
nvenc_enable_min_qp = disabled
nvenc_enable_max_qp = disabled
nvenc_ldkfs = 0
nvenc_split_encode = driver_decides
nvenc_latency_over_power = enabled
```

**为什么这是默认值本身**：这套组合让 NVENC 输出**有硬上界**（平均码率 + 一帧；唯一例外是 IDR/切屏那一帧可到 ~3× 预算，见 [§7](#7-请求码率为什么失控)），是唯一能对网络做出"不超发"承诺的配置。所有激进配方都应该从这里出发、一次改一项。

> ⚠️ **Linux 用户必须额外加一行** `nvenc_aq_strength = 1`（1–15 任意值），否则默认的 0 会让编码器起不来。**下面所有配方同理。**

---

### 8.1 弱网场景（公网与移动网络）

**目标**：客户端请求 10–30 Mbps、丢包率 1–5% 的公网/移动网络。**瓶颈是丢包，不是带宽。**

```ini
nvenc_preset = 4
nvenc_tuning = ull
nvenc_twopass = quarter_res
nvenc_spatial_aq = enabled
nvenc_aq_strength = 8
nvenc_temporal_aq = disabled
nvenc_nonref_p = disabled
nvenc_weighted_prediction = disabled
nvenc_vbv_increase = 0
nvenc_enable_min_qp = enabled
nvenc_min_qp_h264 = 26
nvenc_min_qp_hevc = 30
nvenc_enable_max_qp = disabled
nvenc_ldkfs = 1
nvenc_split_encode = disabled
```

**为什么这样配**：

- **`vbv_increase = 0` 是铁律**：弱网的瓶颈是丢包而不是带宽，任何瞬时超发都会直接变成丢包，而丢包触发 IDR 请求又制造更大的帧（正是 [§7.2](#72-放大机制二max_qp-让编码器放弃码率约束) 的正反馈环）。**弱网场景下"平滑"比"画质"值钱得多。**
- **`enable_min_qp = enabled` 是这里唯一主动引入的手段**：官方口径适用条件恰好就是这个场景 —— *当码率控制器跟不上极低的请求码率时*。它让简单帧少烧比特，把预算留给难帧，等价于在固定预算内重新分配。代价是静态画面会略糊，这可以接受。
- **`spatial_aq` 开、`temporal_aq` 关**：低码率下空间 AQ 收益最大；时间 AQ 与严格 CBR 冲突，在弱网里只会增加帧间波动。
- **`nonref_p` 关**（重要）：非参考帧不能作为恢复点，弱网里开它等于放大每次丢包的影响范围。
- **`preset = 4`**：低码率下 preset 对画质有实际帮助，且编码耗时不会成为瓶颈。
- **`ldkfs = 1`**：避免 IDR 请求时的大超发。**代价是丢包恢复后的最初几帧画质较差** —— 这是弱网下正确的取舍。

**不要做**：`nvenc_enable_max_qp`（弱网下最危险的一个键）、`vbv_increase > 0`、`nonref_p`。

---

### 8.2 办公与桌面生产力

**目标**：文字、文档、终端、代码为主；链路稳定（有线或稳定 Wi-Fi）；**对 1 帧编码延迟不敏感**。

```ini
nvenc_preset = 5
nvenc_tuning = hq
nvenc_twopass = full_res
nvenc_spatial_aq = enabled
nvenc_aq_strength = 10
nvenc_temporal_aq = disabled
nvenc_nonref_p = enabled
nvenc_weighted_prediction = disabled
nvenc_vbv_increase = 50
nvenc_enable_min_qp = disabled
nvenc_enable_max_qp = enabled
nvenc_max_qp_h264 = 30
nvenc_max_qp_hevc = 36
nvenc_max_qp_av1 = 60
nvenc_ldkfs = 4
nvenc_split_encode = driver_decides
```

**为什么这样配**：

- **`nonref_p = enabled` 是办公场景收益最大的单项**：桌面内容绝大部分是静止或局部变化的，"省掉那些毫无意义的静态帧"直接换成码率。
- **`tuning = hq` 而不是 `ull`**：办公场景**对 1 帧的编码延迟不敏感**（没有需要输入延迟反馈的玩法），而 `hq` 在同码率下的文字锐度、细线还原明显更好。这是本文里最典型的"该换骨架就换骨架"的场景。
- **`max_qp + vbv_increase` 成对出现**：办公内容里"难帧"通常是滚屏/视频播放窗口/大表格重绘，单帧预算不够时会糊成一片文字。官方要求这一对必须配套。以 1440p60、客户端请求 50 Mbps（`fec_percentage = 20` ⇒ 视频目标约 40 Mbps）为例：单帧预算 ≈ 40 000 / 8 / 60 ≈ **83 kB**，`vbv_increase = 50` 后 ≈ **125 kB** —— 这点余量刚好覆盖上述难帧，又不会像游戏场景那样无限超发（见 [§7.3](#73-放大机制三nvenc_vbv_increase-与-max_qp-的乘积)）。
- **`min_qp` 关**：`min_qp` 的收益（摊平码率）在办公场景没有价值，而它的代价（静态画面糊）恰好是办公场景最不能接受的 —— 纯色背景色带、文字边缘块状。**这里应该用 `max_qp` 而不是 `min_qp`。**
- **`preset = 5` + `twopass = full_res`**：桌面内容帧间差异小，单帧编码时间本来就短，可以把算力换成画质。
- **编码格式建议 HEVC/AV1**：H.264 在低码率下的文字块效应最明显，办公场景应尽量避免。

**不要做**：把 `min_qp` 和 `max_qp` 同时打开（方向相反，见 [§4.5](#45-qp-上下限)）；打开 `nvenc_weighted_prediction`（本档是 `preset = 5` + `tuning = hq`，恰好落在 SDK 点名"**会内部启用 B 帧**"的组合里，而加权预测与 B 帧互斥 —— 想试 WP 请把 `preset` 降到 2 并换 `ull`）。

---

### 8.3 局域网游戏

**目标**：有线千兆，1080p120 / 1440p144 高刷竞技。**瓶颈是延迟，不是带宽。**

```ini
nvenc_preset = 3
nvenc_tuning = ull
nvenc_twopass = quarter_res
nvenc_spatial_aq = disabled
nvenc_temporal_aq = disabled
nvenc_nonref_p = disabled
nvenc_weighted_prediction = disabled
nvenc_vbv_increase = 0
nvenc_enable_min_qp = disabled
nvenc_enable_max_qp = disabled
nvenc_ldkfs = 0
nvenc_split_encode = enabled
nvenc_latency_over_power = enabled
nvenc_realtime_hags = enabled
```

**为什么这样配**：

- **局域网的瓶颈是延迟，不是带宽**：千兆链路上 100 Mbps 的流只占 10%。所以这一档**一个码率纪律的旋钮都不要松**，全部用来买"帧时间稳定"。
- **`spatial_aq` / `temporal_aq` 都关**：不是不想要画质，而是它们占 CUDA 核心、并且会让帧时间方差变大（AQ 是逐帧分析工作，负载随画面内容波动）。**高刷场景下"每帧都一样快"比"平均画质更好"重要。** 如果 GPU 有富余（例如独显编码 + 弱 GPU 渲染），可以只开 `spatial_aq`。
- **`split_encode = enabled`**：1080p/1440p **驱动不会自动开**（`driver_decides` 只在 ≥4K + P1–P4 才自动开），但手动强制能直接砍编码延迟 —— 这是本场景性价比最高的单项。前提：多 NVENC 单元 + 非 H.264。
- **`preset = 3`**：留出编码时间余量给高帧率；`ull` 已经保证骨架正确。**局域网不要追高 preset** —— 省下的带宽在这里没用，多出的编码耗时直接变成延迟。
- **`ldkfs = 0`**：交给驱动按 `ull` 配（通常是 1），避免自己制造关键帧尖峰。

**不要做**：`vbv_increase`、`max_qp`（局域网不需要它们，只会把 jitter 变大）、`preset = 7`（4K120 时编码耗时可能超过帧间隔）、**同时打开 `weighted_prediction`**（SDK 明文：split encode 与加权预测不能共存，见 [§6 避雷第 2 条](#6-避雷清单) —— 本档已经用 `split_encode` 换了延迟，就别再贪这一项）。

---

### 8.4 4K HDR 高质量有线串流

**目标**：2.5 GbE 及以上有线，HEVC 或 AV1，**追求画质而非最低延迟**。

```ini
nvenc_preset = 6
nvenc_tuning = uhq
nvenc_twopass = full_res
nvenc_spatial_aq = enabled
nvenc_aq_strength = 6
nvenc_temporal_aq = enabled
nvenc_nonref_p = enabled
nvenc_weighted_prediction = disabled
nvenc_vbv_increase = 120
nvenc_enable_max_qp = enabled
nvenc_max_qp_h264 = 28
nvenc_max_qp_hevc = 34
nvenc_max_qp_av1 = 56
nvenc_ldkfs = 3
nvenc_split_encode = enabled
```

**为什么这样配**：

- **`tuning = uhq` 只有在这一档才用得上**：它要求 HEVC/AV1 + Turing 及更新，且它追求的是压缩效率而非延迟 —— 与"有线、带宽充裕、追求画质"正好匹配。**H.264 上写 `uhq` 是无效的**（[§4.1](#41-preset--tuning--twopass编码速度与质量档)）。
- **`temporal_aq` 终于可以开**：`vbv_increase = 120` 已经把单帧窗口撑宽，有时间 AQ 制造帧间波动的余地了。**这是"先开 VBV 再开 temporal_aq"的顺序**，反过来做只会抖动。
- **`split_encode = enabled`**：4K 是本场景下最需要它的时候，且 4K 时驱动**本来**会自动开（`driver_decides` 即可），写成 `enabled` 只是更明确。
- **`vbv_increase` 与 `max_qp` 的数值要按链路核算**：4K60 请求 150 Mbps（乐观口径，尚未扣 FEC 与音频）时单帧预算 ≈ 150 000 / 8 / 60 ≈ 312 kB，`vbv_increase = 120` 后 ≈ 687 kB → 该帧瞬时约 **330 Mbps**。2.5 GbE 才吃得下这种尖峰；**千兆链路上不要照搬这一档**。
- **`weighted_prediction` 保持 `disabled` 是刻意的**：4K HDR 的过场动画/加载画面本来最吃这一项，但本档同时开了 `split_encode = enabled`，而 **SDK 明文规定 split encode 与加权预测不能共存**（[§4.6](#46-split_encode--h264_cavlc格式与并行)）。这里选择了"降延迟的 split"而不是"省码率的 WP" —— 你的链路是 2.5 GbE，码率不是瓶颈。
- **代价明确**：这一档的延迟**不是最低的**（`uhq` + `preset 6` + `full_res` 两遍 + 两个 AQ 都在抢 SM）。如果你玩的是竞技游戏，请回到 [§8.3](#83-局域网游戏)。

---

### 8.5 笔记本与混合输出

**目标**：笔记本 / Optimus / 掌机 —— 编码器与游戏**共享同一份 GPU 算力**。

```ini
nvenc_preset = 2
nvenc_tuning = ull
nvenc_twopass = disabled
nvenc_spatial_aq = disabled
nvenc_temporal_aq = disabled
nvenc_nonref_p = disabled
nvenc_weighted_prediction = disabled
nvenc_vbv_increase = 0
nvenc_enable_min_qp = disabled
nvenc_enable_max_qp = disabled
nvenc_split_encode = driver_decides
nvenc_latency_over_power = enabled
```

**为什么这样配**：

- **一切占 CUDA 核心的东西全部关掉**：`twopass`、两个 AQ、`weighted_prediction` —— 在笔记本/掌机上，编码器抢 SM 的代价会以**游戏掉帧**的形式体现，比画质损失严重得多。
- **`latency_over_power` 必须保持 `enabled`**：笔记本上关掉它会进入省电优先，驱动会主动降低编码器占空比 → 延迟明显上升。电池模式下系统可能改写这个行为，所以插电串流体验会更好。
- **`preset` 压到 2**：省算力，保帧时间。
- 如果 HAGS 下出现黑屏/编码卡死，**再**考虑 `nvenc_realtime_hags = disabled`（这是它的适用场景，不是默认建议）。

---

### 8.6 场景速查

| 场景 | preset | tuning | twopass | spatial_aq | temporal_aq | nonref_p | vbv↑ | min_qp | max_qp | split |
|------|:------:|:------:|:-------:|:----------:|:-----------:|:--------:|:----:|:------:|:------:|:-----:|
| [§8.0 通用基线](#80-通用安全基线) | 1 | ull | ¼res | ✗ | ✗ | ✗ | 0 | ✗ | ✗ | auto |
| [§8.1 弱网](#81-弱网场景公网与移动网络) | 4 | ull | ¼res | ✓8 | ✗ | ✗ | 0 | **✓26/30** | ✗ | ✗ |
| [§8.2 办公](#82-办公与桌面生产力) | 5 | **hq** | full | ✓10 | ✗ | ✓ | 50 | ✗ | **✓30/36/60** | auto |
| [§8.3 局域网游戏](#83-局域网游戏) | 3 | ull | ¼res | ✗ | ✗ | ✗ | 0 | ✗ | ✗ | **✓** |
| [§8.4 4K HDR](#84-4k-hdr-高质量有线串流) | 6 | **uhq** | full | ✓6 | **✓** | ✓ | 120 | ✗ | ✓28/34/56 | **✓** |
| [§8.5 笔记本](#85-笔记本与混合输出) | 2 | ull | ✗ | ✗ | ✗ | ✗ | 0 | ✗ | ✗ | auto |

> **规律**：**丢包多的场景 → 压码率波动（min_qp / 关一切 AQ）；带宽够的场景 → 放码率波动（max_qp + vbv↑ + AQ）。两者永不共存。**

---

## 9. 本 fork 新增键一览

上游 `LizardByte/Sunshine` 的 `master` 没有以下 14 个键（其余 9 个为上游已有）。提到"上游没有这个键"时以此为准。

| ⚠️ 本 fork 新增 | 上游已有 |
|----------------|---------|
| `nvenc_tuning` | `nvenc_preset` |
| `nvenc_ldkfs` | `nvenc_twopass` |
| `nvenc_temporal_aq` | `nvenc_spatial_aq` |
| `nvenc_aq_strength` | `nvenc_vbv_increase` |
| `nvenc_nonref_p` | `nvenc_h264_cavlc` |
| `nvenc_weighted_prediction` | `nvenc_split_encode` |
| `nvenc_enable_min_qp` | `nvenc_realtime_hags` |
| `nvenc_min_qp_h264` / `_hevc` / `_av1` | `nvenc_opengl_vulkan_on_dxgi` |
| `nvenc_enable_max_qp` | `nvenc_latency_over_power` |
| `nvenc_max_qp_h264` / `_hevc` / `_av1` | |

> 上游若把这些键合并过来，**行为差异请以 [§1.2](#12-关-2windows-与-linux-不是同一套实现) 的路径表为准**，尤其注意 Linux 上 12 个键（QP 组 8 个 + `split_encode` + 三个 Windows 平台键）无效或不存在。

### 9.1 关键源码位置速查

| 想确认什么 | 位置 |
|-----------|------|
| 配置解析与范围校验 | [src/config.cpp:1635](../../../src/config.cpp#L1635) |
| `nv_legacy` 镜像（FFmpeg 路径取值来源） | [src/config.cpp:1660](../../../src/config.cpp#L1660) |
| 结构体字段与默认值、注释 | [src/nvenc/nvenc_config.h](../../../src/nvenc/nvenc_config.h) |
| 码控（CBR / VBV / 两遍 / AQ / min-max QP） | [src/nvenc/nvenc_base.cpp:304](../../../src/nvenc/nvenc_base.cpp#L304) |
| 参考帧与 `list0` 限制 | [src/nvenc/nvenc_base.cpp:357](../../../src/nvenc/nvenc_base.cpp#L357) |
| 帧切分编码 | [src/nvenc/nvenc_base.cpp:278](../../../src/nvenc/nvenc_base.cpp#L278) |
| `tuning` → SDK 枚举（含 H.264 UHQ 回退） | [src/nvenc/nvenc_base.cpp:213](../../../src/nvenc/nvenc_base.cpp#L213) |
| 编码器创建与日志（自检入口） | [src/nvenc/nvenc_base.cpp:660](../../../src/nvenc/nvenc_base.cpp#L660) |
| FFmpeg 选项表（Linux 路径） | [src/video.cpp:788](../../../src/video.cpp#L788) |
| FFmpeg 路径的 VBV 放大 | [src/video.cpp:2261](../../../src/video.cpp#L2261) |
| WebUI 暴露面 | `src_assets/common/assets/web/configs/tabs/encoders/NvidiaNvencEncoder.vue` |
| 每键官方文档 | [docs/configuration.md](../../../docs/configuration.md)（NVENC 段） |

---

## 相关配套资料

- 参数语义的权威来源：`docs/configuration.md` 的 NVENC 段（默认值、范围、warning 原文）
- 码率数字的推导链：[04 §1](./04-码率控制与QoS.md#1-码率数字的完整旅程)、[04 §3](./04-码率控制与QoS.md#3-nvenc-原生-sdk-路径)
- 超发之后网络侧的行为：[05 §4](./05-FEC-重传-丢帧与心跳.md#4-重传sunshine-没有-arq)、[05 §5](./05-FEC-重传-丢帧与心跳.md#5-丢帧没有显式策略只有队列覆盖)
- 历史调优讨论（本文的上游素材，部分结论已被本 fork 的实现推翻）：`TODO/01/NVENC超低延迟调优作用与场景.md`、`TODO/01/Rustdesk 和 Sunshine 延迟 带宽差异.md`
- 上游 NVENC 编码器文档（SDK `NVENC_*` 枚举与能力的原始定义）：NVIDIA Video Codec SDK 的 `nvEncodeAPI.h`
