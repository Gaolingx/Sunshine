# 05 · FEC、重传、丢帧与心跳

本篇围绕「**在不可靠的 UDP 上尽力还原实时画面**」这一个主题，拆成四块：

| 机制 | Sunshine 的做法 |
|------|----------------|
| **FEC 纠错** | 视频：自适应百分比 + 最多 4 个 RS 块；音频：固定 4+2 RS |
| **重传** | ❌ **完全没有应用层重传（ARQ）**；只靠客户端请求 IDR/RFI 恢复 |
| **丢帧** | ❌ 没有显式丢帧策略；丢弃是缓冲队列 `drop_oldest` 的副作用 |
| **心跳** | ✅ 两套：控制通道 ENet 事件刷新超时 + UDP 每流初始 PING |

> 图例见 [README · 图例与阅读约定](./README.md#图例与阅读约定)。

---

## 1. FEC 基础：Reed–Solomon

Sunshine 使用 moonlight-common-c 内置的 `nanors`（[third-party/moonlight-common-c/nanors/](../../../third-party/moonlight-common-c/nanors/)）做 Reed–Solomon 纠删码。

| 常量 | 值 | 来源 |
|------|:--:|------|
| `DATA_SHARDS_MAX` | **255** | [rs.h:8](../../../third-party/moonlight-common-c/nanors/rs.h#L8) |
| `RTPA_DATA_SHARDS` | **4** | [RtpAudioQueue.h:11](../../../third-party/moonlight-common-c/src/RtpAudioQueue.h#L11) |
| `RTPA_FEC_SHARDS` | **2** | 同上 |
| `RTPA_TOTAL_SHARDS` | **6** | 同上 |

约束：`data_shards + parity_shards <= 255`（`reed_solomon_new` 会拒绝越界）。

FEC 的价值：**接收端只要能收到 `data_shards` 个分片中的任意 `data_shards` 个（含校验），就能完整还原整块**。丢包不需要重传，代价是额外带宽。

---

## 2. 视频 FEC

### 2.1 编码函数 `fec::encode`（`src/stream.cpp:845`）

```cpp
fec::encode(payload, blocksize, fecpercentage, minparityshards, prefixsize)
```

**第一步：算分片数**

```cpp
auto pad = payload_size % blocksize != 0;
auto aligned_data_shards = payload_size / blocksize;
auto data_shards  = aligned_data_shards + (pad ? 1 : 0);
auto parity_shards = (data_shards * fecpercentage + 99) / 100;   // 向上取整
```

**第二步：最低校验分片数兜底**（关键！）

```cpp
if (parity_shards < minparityshards && fecpercentage != 0) {
  parity_shards = minparityshards;
  fecpercentage = (100 * parity_shards) / data_shards;

  BOOST_LOG(verbose) << "Increasing FEC percentage to " << fecpercentage
                     << " to meet parity shard minimum";
}
```

`minparityshards` 来自 `session->config.minRequiredFecPackets`（由 `cmd_announce` 从 SDP 的 `x-nv-vqos[0].fec.minRequiredFecPackets` 解析，默认 2）。

> **为什么需要这一步？** 当帧很小（例如 3 个数据分片）而客户端要求 `fec_percentage = 20` 时，`parity = ceil(3*20/100) = 1`，只有 1 个校验分片。客户端协议要求至少 2 个校验分片才能正确处理矩阵布局，所以 Sunshine **强制抬高百分比**（这里会变成 `100*2/3 = 66%`），并把**实际使用的百分比写进每个包的 `fecInfo`**，让客户端知道真实配置。

**第三步：内存布局**

```
payload ──────────────────────────────────────────────────────┐
                                                              │
            ┌──────────┬──────────┬──────────┬────────┬───────┴───┐
data_shards │ shard 0  │ shard 1  │ shard 2  │ shard 3│ 填充块(0) │
            └──────────┴──────────┴──────────┴────────┴───────────┘
              ↑ 直接指向 payload（零拷贝）        ↑ 最后一块需零填充 → 单独缓冲

parity      ┌──────────┬──────────┐
            │ parity 0 │ parity 1 │   ← 自己分配
            └──────────┴──────────┘

内存顺序（为减少碎片）：
  shards buffer = [零填充数据块(0~1 个)] [parity 0..P-1]
```

```cpp
auto parity_shard_offset = pad ? 1 : 0;
util::buffer_t<char> shards {(parity_shard_offset + parity_shards) * blocksize};
util::buffer_t<uint8_t *> shards_p {nr_shards};

// 前 aligned_data_shards 个直接指向 payload
for (x = 0; x < aligned_data_shards; ++x) {
  shards_p[x] = (uint8_t *) next;
  next += blocksize;
}
payload_buffers.emplace_back(std::begin(payload), aligned_data_shards * blocksize);

// 需要零填充的数据块 → 放到 shards buffer 的第 0 个位置
if (pad) {
  shards_p[aligned_data_shards] = (uint8_t *) &shards[0];
  memcpy(shards_p[aligned_data_shards], next, copy_len);
  if (copy_len < blocksize)
    memset(shards_p[aligned_data_shards] + copy_len, 0, blocksize - copy_len);   // ★ 必须清零
}
```

> 源码注释明确说明：**GCC 无法把 `std::copy_n` 优化成 `memcpy`**，会生成逐元素慢速拷贝循环，所以这里手工用 `memcpy`/`memset`。

**第四步：算校验数据**

```cpp
if (fecpercentage != 0) {
  for (x = 0; x < parity_shards; ++x)
    shards_p[data_shards + x] = (uint8_t *) &shards[(parity_shard_offset + x) * blocksize];

  rs_t rs {reed_solomon_new(data_shards, parity_shards)};
  reed_solomon_encode(rs.get(), shards_p.begin(), nr_shards, blocksize);
}
```

**返回值** —— `struct { data_shards, nr_shards, percentage, blocksize, prefixsize, payload, headers, payload_buffers }`。

- `payload` / `headers` 是 `util::buffer_t<char>`，分别存放「零填充块 + parity」与「每分片前缀（加密时非空）」；
- `payload_buffers` 是给 `send_batch` 用的**分散-聚集（scatter-gather）描述符**，只有 2 项：`{payload 前缀段, shards buffer}`，因此零拷贝路径只需 2 个 iovec。

### 2.2 帧级分块：一个帧最多 4 个 RS 块

RTP 头里 **`multiFecBlocks` 只有 2 bit 表示块数**（最大 4），所以一帧最多 4 个 RS 块（`src/stream.cpp:1608`）：

```cpp
constexpr auto MAX_FEC_BLOCKS = 4;

// 解方程组 D = 255 - P, P = D * F  →  D = 255 / (1 + F)
// 乘 100 因为 F 是整数百分比
auto max_data_shards_per_fec_block = (DATA_SHARDS_MAX * 100) / (100 + fecPercentage);

auto max_data_per_fec_block = max_data_shards_per_fec_block * blocksize;
auto fec_blocks_needed = (payload.size() + (max_data_per_fec_block - 1)) / max_data_per_fec_block;

if (fec_blocks_needed > MAX_FEC_BLOCKS) {
  BOOST_LOG(warning) << "Skipping FEC for abnormally large encoded frame (needed "
                     << fec_blocks_needed << " FEC blocks)";
  fecPercentage = 0;                 // ★ 本帧干脆关掉 FEC
  fec_blocks_needed = MAX_FEC_BLOCKS;
}
```

| `fec_percentage` | `max_data_shards_per_fec_block` | 单块最大数据量（`packetsize=1024`，`blocksize=1040`） |
|:----------------:|:-------------------------------:|:---------------------------------------------------:|
| 0 | 255 | 265 KB |
| 20（默认） | 212 | 220 KB |
| 50 | 170 | 177 KB |
| 100 | 127 | 132 KB |

> 客户端要求大包（如 `packetsize = 8` + `fec = 20%`）时 `blocksize` 很小，`fec_blocks_needed` 会迅速上升。超过 4 块就**整帧不发 FEC**——宁可让这一帧裸奔，也不能让协议头失效。

**块切分与对齐**：

```cpp
auto unaligned_size = payload.size() / fec_blocks_needed;
auto aligned_size = ((unaligned_size + (blocksize - 1)) / blocksize) * blocksize;

// 10-bit FEC 包索引 → 每块最多 1024 个包
if (aligned_size / blocksize >= 1024) {
  BOOST_LOG(error) << "Encoder produced a frame too large to send! Is the encoder broken? (needed "
                   << (aligned_size / blocksize) << " packets)";
}

for (x = 0; x < fec_blocks_needed; ++x) {
  if (x == fec_blocks_needed - 1)
    fec_blocks[x] = payload.substr(x * aligned_size);            // 最后一块延伸到末尾
  else
    fec_blocks[x] = payload.substr(x * aligned_size, aligned_size);
}
```

> **为什么 `aligned_size` 要按 `blocksize` 对齐？** 每个 FEC 块的每个分片都必须是 `blocksize` 的整数倍（RS 是对等长块做运算），且后续 `fecInfo` 里的分片索引只有 10 bit（`x << 12` 与 `shards.data_shards << 22` 共 10 bit 字段），超过 1024 个包时客户端协议无法表达，帧就彻底不可恢复——所以这里直接报 error。

### 2.3 帧头与 `fecInfo`

广播前会给 payload 插入 8 字节的 **`video_short_frame_header_t`**（`concat_and_insert`，见 `src/stream.cpp:1603`）：

```
每个 blocksize 切片开头插入 sizeof(video_packet_raw_t) 字节
   ├─ RTP 头（软件侧占位，发送前填充）
   ├─ NV_VIDEO_PACKET（Sunshine 私有扩展）
   └─ 8 字节 video_short_frame_header_t（仅第一个切片）
```

`concat_and_insert(insert_size, slice_size, data1, data2)` 把 `frame_header` 与编码数据拼成一个连续缓冲，**每个 `slice_size` 边界前插入 `insert_size` 字节空位**（后续填 RTP/NV 头）。长度不是 `slice_size` 整数倍时会补齐最后一个切片。

**`fecInfo` 字段布局**（`src/stream.cpp:1736`，写进每个分片）：

```cpp
inspect->packet.fecInfo = (uint32_t)( x << 12              // 分片在块内的索引（10 bit）
                                    | shards.data_shards << 22  // 本块数据分片数（10 bit）
                                    | shards.percentage << 4); // 实际使用的 FEC 百分比（8 bit）
inspect->packet.multiFecBlocks = (blockIndex << 4) | ((fec_blocks_needed - 1) << 6);
inspect->packet.multiFecFlags  = 0x10;
inspect->packet.frameIndex     = packet->frame_index();
inspect->packet.streamPacketIndex = ((uint32_t) lowseq + x) << 8;
inspect->packet.flags = FLAG_CONTAINS_PIC_DATA;   // + FLAG_SOF（首包）+ FLAG_EOF（末包）
```

> 客户端正是靠 `fecInfo` 里的 `data_shards` / `percentage` 而非 SDP 里的声明值来重建矩阵——这就是 §2.1 「抬高百分比」必须写回包里的原因。

### 2.4 视频 FEC 完整流程

```
编码器输出一帧
   │
   ├─ 插入 8 字节帧头 + 每切片预留 NV/RTP 头空间 (concat_and_insert)
   ├─ 算 fec_blocks_needed（≤ 4，否则关 FEC）
   ├─ 按 aligned_size 切成 1~4 块
   └─ for each block:
        ├─ fec::encode(block, blocksize, fecPercentage, minRequiredFecPackets, prefixsize)
        ├─ 填 RTP/NV/FEC 头（fecInfo / multiFecBlocks / 序列号 / 90kHz 时间戳）
        ├─ 可选 AES-GCM 加密（IV = 64-bit 计数器 + iv[11]='V'）
        └─ 按速率控制组批量发送（见 04 · 码率控制）
   │
   └─ lowseq += shards.size();  session->video.lowseq = lowseq;
```

---

## 3. 音频 FEC（固定 4+2）

音频走的是完全不同的固定结构（`src/stream.cpp:1852` 起），因为 moonlight-common-c 的 `RtpAudioQueue` 是**硬编码** 4+2 的。

### 3.1 神秘的校验矩阵替换

```cpp
fec::rs_t rs {reed_solomon_new(RTPA_DATA_SHARDS, RTPA_FEC_SHARDS)};

// For unknown reasons, the RS parity matrix computed by our RS implementation
// doesn't match the one Nvidia uses for audio data. I'm not exactly sure why,
// but we can simply replace it with the matrix generated by OpenFEC which
// works correctly. This is possible because the data and FEC shard count is
// constant and known in advance.
constexpr std::array<unsigned char, 8> parity {0x77, 0x40, 0x38, 0x0e, 0xc7, 0xa7, 0x0d, 0x6c};
memcpy(rs.get()->p, parity.data(), parity.size());
```

> 这是整个 FEC 实现里最"魔幻"的一段：Sunshine 自己的 `nanors` 生成的 RS 矩阵与 Nvidia 客户端期望的**不一致**，作者直接硬编码了 OpenFEC 生成的矩阵。只在固定 4+2 结构下可行，所以这段代码无法复用到视频路径。

### 3.2 发送节奏

```
音频包序列 (sequenceNumber)
   seq 0 ──► 写 shards_p[0]，AES-CBC 加密，发 UDP
   seq 1 ──► 写 shards_p[1]，发
   seq 2 ──► 写 shards_p[2]，发
   seq 3 ──► 写 shards_p[3]，发
              └─ (seq+1) % 4 == 0 → reed_solomon_encode(rs, shards_p, 6, bytes)
                                    发 2 个 FEC 包（src 127, fecShardIndex 0/1）
   seq 4 ──► 新块开始，记录 baseSequenceNumber / baseTimestamp
   ...
```

**加密与 IV**（与视频不同，音频用 **AES-CBC**）：

```cpp
auto sequenceNumber = session->audio.sequenceNumber;
auto timestamp      = session->audio.timestamp;

// IV 的前 4 字节 = BE(avRiKeyId + sequenceNumber)
*(std::uint32_t *) iv.data() = util::endian::big<std::uint32_t>(session->audio.avRiKeyId + sequenceNumber);

auto bytes = encode_audio(session->config.encryptionFlagsEnabled & SS_ENC_AUDIO,
                          packet_data,
                          shards_p[sequenceNumber % RTPA_DATA_SHARDS],   // 数据写入第 (seq%4) 个分片
                          iv, session->audio.cipher);
if (bytes < 0) { BOOST_LOG(error) << "Couldn't encode audio packet"; break; }

audio_packet.rtp.sequenceNumber = util::endian::big(sequenceNumber);
audio_packet.rtp.timestamp      = util::endian::big(timestamp);

session->audio.sequenceNumber++;
session->audio.timestamp += session->config.audio.packetDuration;
```

`avRiKeyId` 来自 `launch_session.iv` 的前 4 字节（`cmd_announce` 的 `rikeyid`），保证与客户端同步。

**FEC 头发包**（`AUDIO_FEC_HEADER`）：

```cpp
if (sequenceNumber % RTPA_DATA_SHARDS == 0) {           // 块首初始化基序号/基时间戳
  fec_packet.fecHeader.baseSequenceNumber = big(sequenceNumber);
  fec_packet.fecHeader.baseTimestamp      = big(timestamp);
}
if ((sequenceNumber + 1) % RTPA_DATA_SHARDS == 0) {     // 块尾生成并发送 2 个校验包
  reed_solomon_encode(rs.get(), shards_p.begin(), RTPA_TOTAL_SHARDS, bytes);
  for (x = 0; x < RTPA_FEC_SHARDS; ++x) {
    fec_packet.rtp.sequenceNumber = big<uint16_t>(sequenceNumber + x + 1);
    fec_packet.fecHeader.fecShardIndex = x;
    platf::send(...);
    BOOST_LOG(verbose) << "Audio FEC [" << (sequenceNumber & ~(RTPA_DATA_SHARDS - 1)) << ' ' << x << "] ::  send...";
  }
}
```

**会话级缓冲区**（`session::alloc`，`src/stream.cpp:2344`）：

```cpp
constexpr auto max_block_size = crypto::cipher::round_to_pkcs7_padded(2048);  // → 2048

util::buffer_t<char>     shards   {RTPA_TOTAL_SHARDS * max_block_size};
util::buffer_t<uint8_t*> shards_p {RTPA_TOTAL_SHARDS};
for (x = 0; x < RTPA_TOTAL_SHARDS; ++x)
  shards_p[x] = (uint8_t *) &shards[x * max_block_size];

session->audio.shards   = std::move(shards);
session->audio.shards_p = std::move(shards_p);

// FEC 包（RTP type 127）
session->audio.fec_packet.rtp.header       = 0x80;
session->audio.fec_packet.rtp.packetType   = 127;
session->audio.fec_packet.rtp.timestamp    = 0;
session->audio.fec_packet.rtp.ssrc         = 0;
session->audio.fec_packet.fecHeader.payloadType = 97;
session->audio.fec_packet.fecHeader.ssrc         = 0;
```

> 音频 FEC 是**跨包**的（4 个数据包 + 2 个 FEC 包构成一个块），所以分片缓冲必须挂在**会话**上而不是每包栈上。

| 参数 | 数据包 | FEC 包 |
|------|:------:|:------:|
| RTP `header` | `0x80` | `0x80` |
| RTP `packetType` | `97` | `127` |
| 载荷 | 编码音频实际字节数 | 校验分片（`bytes` 字节） |
| 序列号 | `seq` | `seq + x + 1` |

### 3.3 音频为什么不用配速、不用丢帧？

- 速率固定：`packetDuration`（默认 5 ms）→ 每周期 4 个数据包 + 2 个 FEC 包 = **1200 包/秒**；
- 每包最大 1400 字节（`MAX_AUDIO_PACKET_SIZE`），即最坏约 **1.7 MB/s**，远低于链路容量；
- 因此 `audioBroadcastThread` 里既没有速率控制组，也没有队列满丢弃逻辑。

---

## 4. 重传：Sunshine 没有 ARQ

**结论：整个项目不存在应用层重传机制。**

| 通道 | 传输 | 可靠性来源 |
|------|------|-----------|
| RTSP（48010） | TCP | TCP 自带 |
| nvhttp / WebUI | TCP (HTTPS/HTTP) | TCP 自带 |
| **控制通道**（47999） | ENet over UDP | ENet **可靠有序**通道（内部有 ACK + 重传） |
| **视频**（47998） | 裸 UDP + FEC | 只靠 FEC + 客户端请求关键帧 |
| **音频**（48000） | 裸 UDP + FEC | 只靠 FEC |

> 唯一"重传"发生在 **ENet 库内部**，用于控制通道的可靠投递。视频/音频路径**完全没有重传逻辑**——因为重传一个 5 ms 前的视频分片没有意义（那帧可能已经显示过了）。

### 4.1 客户端驱动的恢复

丢失恢复**完全由客户端发起**，通过控制通道两个消息：

| 消息 | 值 | 载荷 | Sunshine 处理 |
|------|:--:|------|--------------|
| `IDX_REQUEST_IDR_FRAME` | `0x0302` | 空 | `session->video.idr_events->raise(true)` |
| `IDX_INVALIDATE_REF_FRAMES` | `0x0301` | `int64_t[2]` = {firstFrame, lastFrame} | `session->video.invalidate_ref_frames_events->raise(make_pair(first, last))` |

```cpp
server->map(packetTypes[IDX_REQUEST_IDR_FRAME], [&](session_t *session, const std::string_view &) {
  BOOST_LOG(debug) << "type [IDX_REQUEST_IDR_FRAME]";
  session->video.idr_events->raise(true);
});

server->map(packetTypes[IDX_INVALIDATE_REF_FRAMES], [&](session_t *session, const std::string_view &payload) {
  auto frames = (std::int64_t *) payload.data();
  auto firstFrame = frames[0];
  auto lastFrame  = frames[1];
  BOOST_LOG(debug) << "type [IDX_INVALIDATE_REF_FRAMES]"
                   << "\nfirstFrame [" << firstFrame << ']'
                   << "\nlastFrame [" << lastFrame << ']';
  session->video.invalidate_ref_frames_events->raise(std::make_pair(firstFrame, lastFrame));
});
```

**IDR 请求**（关键帧请求）：客户端发现丢包率过高 / 解码器出错，就请求一个立即刷新帧。Sunshine 在 `encode_run` 循环顶部消费该事件并强制下一帧为 IDR。

**RFI 请求**（参考帧失效）：比 IDR 更"便宜"——不重发完整关键帧，只让编码器**从某一帧起不再引用之前的帧**。客户端只需等待新帧链建立即可恢复。这是 Sunshine 特有的优化（对应 `video.h::encoder_t::flag_e::REF_FRAMES_INVALIDATION` 能力位与 SDP 的 `x-ss-video.rfi`）。

### 4.2 `IDX_LOSS_STATS`：只记录，自适应

```cpp
server->map(packetTypes[IDX_LOSS_STATS], [&](session_t *session, const std::string_view &payload) {
  int32_t *stats = (int32_t *) payload.data();
  auto count = stats[0];                        // 自上次报告以来的丢包数
  std::chrono::milliseconds t {stats[1]};       // 自上次报告以来的毫秒数
  auto lastGoodFrame = stats[3];                // 最后一个成功解码的帧号

  BOOST_LOG(verbose)
    << "type [IDX_LOSS_STATS]"
    << "\n---begin stats---"
    << "\nloss count since last report [" << count << ']'
    << "\ntime in milli since last report [" << t.count() << ']'
    << "\nlast good frame [" << lastGoodFrame << ']'
    << "\n---end stats---";
});
```

> ⚠️ **这是 fork 的一个已知缺口**：`IDX_LOSS_STATS` 只写日志，**没有任何码率自适应**（不像某些串流方案会据此降码率）。真正影响发送的是 `IDX_REQUEST_IDR_FRAME` / `IDX_INVALIDATE_REF_FRAMES`。

### 4.3 重传缺失的代价评估

| 场景 | FEC 能救吗 | 结果 |
|------|:----------:|------|
| 一个 FEC 块内丢 1~P 个包 | ✅ | 客户端 RS 解码还原 |
| 一个 FEC 块内丢 > P 个包 | ❌ | 整块丢弃；解码器报错 → 客户端请求 IDR/RFI |
| 视频包乱序 | ✅ | FEC + RTP 序列号足够重建顺序（FEC 不依赖顺序） |
| 音频块丢 > 2 包 | ❌ | 该音频块丢弃（20 ms 静音/杂音），不重传 |

---

## 5. 丢帧：没有显式策略，只有队列覆盖

### 5.1 唯一的"丢帧"来源：`mail::video_packets` 溢出

```cpp
// src/thread_safe.h:391
template<class T>
class queue_t {
  enum class overflow_policy_e {
    drop_oldest,  ///< Clear the queue and accept the new item (current default behavior)
    reject        ///< Refuse the new item, leaving existing queued items intact
  };

  explicit queue_t(std::uint32_t max_elements = 32,
                   overflow_policy_e overflow = overflow_policy_e::drop_oldest);

  bool raise(Args &&...args) {
    std::lock_guard ul {_lock};
    if (!_continue) return false;

    if (_queue.size() >= _max_elements) {
      if (_overflow_policy == overflow_policy_e::reject) return false;
      _queue.clear();              // ★★★ 不是丢最旧的一个，而是【整队清空】
    }
    _queue.emplace_back(std::forward<Args>(args)...);
    _cv.notify_all();
    return true;
  }
};
```

**行为解读**：

| 事实 | 值 |
|------|----|
| 视频包队列容量 | `32`（`mail_raw_t::queue<T>` 硬编码，`src/thread_safe.h:851`） |
| 超容量时行为 | **清空整个队列**，再插入新元素 |
| 谁用 `reject` 策略 | 只有 `platform/linux/kmsgrab.cpp:183` 与 `platform/linux/graphics.cpp:474` |

> 「清空整个队列再插入新帧」这个策略在**实时串流里是正确选择**：如果编码器产出速度超过了发包线程的消费速度，累积的旧帧只会越积越晚显示，不如整体丢掉、从最新帧重新开始。代价是这一瞬间会出现一次明显的卡顿（而不是平滑的降帧）。

### 5.2 捕获/编码路径本身没有丢帧逻辑

`encode_run`（`src/video.cpp:2420`）里：

```cpp
while (true) {
  // 消费 idr_events / invalidate_ref_frames_events
  ...
  auto minimum_fps_target = config::video.minimum_fps_target > 0
                              ? config::video.minimum_fps_target
                              : config.framerate / 2;
  auto max_frametime = 1000.0 / minimum_fps_target;
  ...
  // 编码 → mail::video_packets->raise(...)
}
```

`minimum_fps_target` / `max_frametime` 只用于**调整"重复帧"的生成与超时判定**，不会主动跳过已捕获的帧。

**真正的"丢帧"发生链**：

```
编码器产出速度  >  videoBroadcastThread 发送速度
        （因为速率配速把带宽限制在 1Gbps 的 80% 之内）
                      │
                      ▼
        mail::video_packets 积压到 32 个
                      │
                      ▼
        下一帧 raise() → _queue.clear() → 32 帧全部丢弃
                      │
                      ▼
        客户端看到卡顿 → 发 IDX_REQUEST_IDR_FRAME → 恢复
```

### 5.3 重复帧（Dupe Frame）

画面静止时编码器仍会按 `framerate` 产出"重复帧"（相同内容、无 `frame_timestamp`）。这些帧：

```cpp
bool frame_is_dupe = false;
if (!packet->frame_timestamp) {
  packet->frame_timestamp = ratecontrol_next_frame_start;   // 用速率控制时间轴当时间戳
  frame_is_dupe = true;
}
...
BOOST_LOG(verbose) << "Sent Frame seq [" << packet->frame_index() << "] pts [" << timestamp
                   << "] shards [" << shards.size() << "/" << shards.percentage << "%]"
                   << (frame_is_dupe ? " Dupe" : "")
                   << (packet->is_idr() ? " Key" : "");     // " Key" = 关键帧
```

重复帧**占很少字节**（几乎全是 skip 块），但仍会占队列槽位——这是静止画面下队列不会积压的原因。

---

## 6. 心跳：双套机制

Sunshine 有**两套互相独立**的心跳，分别负责"会话是否还活着"和"客户端在哪"。

### 6.1 机制 A：控制通道 ENet 超时（保活判定）

**刷新点**（`control_server_t::iterate`）：

```cpp
while (enet_host_service(host.get(), &event, timeout_ms) > 0) {
  // ★ 任何 ENet 事件（CONNECT / RECEIVE / DISCONNECT / NONE）都刷新超时
  session->pingTimeout = std::chrono::steady_clock::now() + config::stream.ping_timeout;
  ...
}
```

**判定点**（`controlBroadcastThread`）：

```cpp
if (now > session->pingTimeout) {
  auto address = session->control.peer
                   ? platf::from_sockaddr((sockaddr *) &session->control.peer->address.address)
                   : session->control.expected_peer_address;
  BOOST_LOG(info) << address << ": Ping Timeout";
  session::stop(*session);
}
```

- 客户端**周期性发送 `IDX_PERIODIC_PING`（wire `0x0200`）**，Sunshine 收到即刷新（回调里只打 verbose 日志）；
- 超时阈值 = `config::stream.ping_timeout`，**默认 10 秒**；
- 连接被 ENet 判定为 DISCONNECT 且会话已是 RUNNING → 直接 `session::stop`。

**服务器主动断开的报文**（`controlBroadcastThread` 退出时，`src/stream.cpp:1385`）：

```cpp
// Let all remaining connections know the server is shutting down
// reason: graceful termination
std::uint32_t reason = 0x80030023;

control_terminate_t plaintext;
plaintext.header.type = packetTypes[IDX_TERMINATION];     // 0x0109
plaintext.header.payloadLength = sizeof(plaintext.ec);
plaintext.ec = util::endian::big<uint32_t>(reason);

send_control_message(...);
shutdown_event->raise(true);
controlEnd.raise(true);
server->flush();
```

### 6.2 机制 B：UDP 每流初始 PING（发现客户端端口 + 建 QoS）

控制通道是 ENet，但**视频/音频是裸 UDP**——Sunshine 不知道客户端在哪个端口收包。所以 `cmd_setup` 时告诉客户端一个"暗号"，客户端在**它自己的接收端口**上把这个暗号回送一次。

**暗号的下发**（`src/rtsp.cpp:1052`）：

| SETUP 目标 | 响应头 | 值 |
|-----------|--------|-----|
| `control` | `X-SS-Connect-Data` | `session->control_connect_data`（随机 u32） |
| `video` | `X-SS-Ping-Payload` | `session->video.ping_payload` |
| `audio` | `X-SS-Ping-Payload` | `session->audio.ping_payload` |

**暗号来源**（`src/nvhttp.cpp:519`）：

```cpp
RAND_bytes(raw_payload, 8);
launch_session->av_ping_payload = util::hex_vec(raw_payload);   // 8 随机字节 → 16 个 hex 字符
```

**等待函数**（`src/stream.cpp:2073`）：

```cpp
int recv_ping(session_t *session, safe::mail_raw_t::queue_t<packet_t> &ref,
              socket_e socket_type, const char *expected_payload,
              udp::endpoint &peer, std::chrono::milliseconds timeout)
```

```
recv_ping(...)
  ├─ 在 mail 上注册一个 30 元素队列：
  │    · 键 = expected_payload 字符串
  │    · 键 = peer.address()      ← 仅当客户端不支持 ML_FF_SESSION_ID_V1 时的兼容路径
  ├─ 循环直到 config::stream.ping_timeout：
  │    · bytes == 4                        → 旧式 PING，按【对端地址】匹配
  │    · bytes >= sizeof(SS_PING (20 B))   → 新式 PING，按【payload 字符串】匹配
  │    · payload 含 expected_payload（v2）或 恰好为 "PING"（v1）
  │         → peer = recv_peer; return 0
  └─ 超时 → BOOST_LOG(error) << "Initial Ping Timeout";  return -1
  失败守卫：注销两个键并 stop() 该队列（无论成功失败）
```

**`SS_PING` 结构**（[third-party/moonlight-common-c/src/Video.h:51](../../../third-party/moonlight-common-c/src/Video.h#L51)）：

```c
typedef struct _SS_PING {
  char payload[16];            // 就是十六进制字符串（16 字符，不含 NUL）
  uint32_t sequenceNumber;
} SS_PING;                     // 共 20 字节
```

**接收侧的匹配**（`recvThread`，`src/stream.cpp:1423`）：

```cpp
// 维护 peer_to_video_session / peer_to_audio_session（键 = av_session_id_t）
populate_peer_to_session();        // 每次收包前先同步 message_queue_queue，null 队列 → 删表项

async_receive(...) → handler(bytes):
  if (bytes == 4)                          // 旧式："PING"
     通过【对端地址】查表
  else if (bytes >= sizeof(SS_PING)) {     // 新式：16 字节 payload
     auto ping = (SS_PING *) buf;
     通过 std::string{ping->payload} 查表
  }
  命中 → queue->raise(peer, msg)         // 把 (endpoint, 数据) 投给等待中的 recv_ping
```

**这里的核心作用**：PING 一旦成功，Sunshine 就记住了客户端的**真实 UDP 源端口**，然后：

1. 创建 **QoS 打标**（见 [04 · 码率控制与 QoS](./04-码率控制与QoS.md) 第 5 节「qWAVE / DSCP 打标」）；
2. 启动真正的采集与编码（`video::capture` / `audio::capture`）。

```cpp
// src/stream.cpp videoThread
recv_ping(session, session->mail->... , socket_e::video, session->video.ping_payload, session->video.peer, ping_timeout);

session->video.qos = platf::enable_socket_qos(
    video_sock.native_handle(), address, port,
    platf::qos_data_type_e::video, session->config.videoQosType != 0);

video::capture(session->mail, session->config.monitor, session);
```

### 6.3 机制 C（补充）：RTSP 侧的事件超时

`rtsp_server_t::session_raise`（`src/rtsp.cpp:595`）里还有一个**仅针对 ANNOUNCE 阶段的事件守卫**：

```cpp
raised_timer.expires_after(config::stream.ping_timeout);
// 到期后：弹出 pending 的 launch event 并记录
BOOST_LOG(info) << "Event timeout: " << unique_id;
```

它的作用是：`/launch`（nvhttp）已经创建了 `launch_session`，但客户端一直没有走 RTSP `ANNOUNCE` 来领取——超时后自动清理，避免僵尸会话占住串流槽位。

### 6.4 三重超时速查

| 机制 | 超时值 | 触发后行为 | 位置 |
|------|:------:|-----------|------|
| ENet 控制通道 | `config::stream.ping_timeout`（默认 **10 s**） | `session::stop` | `controlBroadcastThread` |
| UDP 每流初始 PING | `config::stream.ping_timeout` | `videoThread`/`audioThread` 失败守卫 → `session::stop` | `recv_ping` |
| RTSP 未领取的 launch | `config::stream.ping_timeout` | 清理 pending 事件，打 `Event timeout: <unique_id>` | `session_raise` |
| RTSP `SETUP` 响应的 Session 超时 | **90 s**（常量） | 客户端自行重连 | `cmd_setup` 的 `Session: DEADBEEFCAFE;timeout = 90` |

---

## 7. 一张图总结

```
                              编码器产出帧
                                   │
                    ┌──────────────┴──────────────┐
                    │  mail::video_packets        │
                    │  capacity = 32, drop_oldest │  ← 【丢帧发生处】
                    │  满 → clear() 整队丢弃       │
                    └──────────────┬──────────────┘
                                   ▼
                     videoBroadcastThread
                    ┌──────────────────────────────────────┐
                    │ 插帧头 → 分 1~4 个 FEC 块             │
                    │ fec::encode（RS 255 上限，最低 P 兜底）│  ← 【FEC 纠错】
                    │ 填 fecInfo / multiFecBlocks           │
                    │ 可选 AES-GCM（IV 计数器 + 'V'）        │
                    │ 速率配速（1Gbps×80%，1ms 一组）        │  ← 见 04
                    └──────────────┬───────────────────────┘
                                   ▼
                          裸 UDP → 网络（会丢包）
                                   │
                        客户端 RS 解码
                     ┌─────────────┴─────────────┐
                     │ 恢复成功 → 正常解码显示     │
                     │ 恢复失败 → 解码器报错       │
                     └─────────────┬─────────────┘
                                   ▼
                  控制通道（ENet 可靠）
                  ├ IDX_REQUEST_IDR_FRAME (0x0302)      ← 请求关键帧
                  ├ IDX_INVALIDATE_REF_FRAMES (0x0301)  ← 请求参考帧失效
                  ├ IDX_LOSS_STATS (0x0201)             ← 只记日志，无自适应
                  └ IDX_PERIODIC_PING (0x0200)          ← 保活心跳
                                   │
                                   ▼
                  编码器下一帧强制 IDR / RFI → 恢复
```

---

## 8. 参数与调优点速查

| 参数 | 位置 | 默认 | 说明 |
|------|------|:----:|------|
| `stream.fec_percentage` | [src/config.h:264](../../../src/config.h#L264) | `20` | 范围 `[1, 255]`。**LAN 建议 5~20，WAN 20~50** |
| `stream.packetsize` | 同上 | `0`（= 客户端 SDP 值） | 范围 `[PACKETSIZE_MIN, PACKETSIZE_MAX]`。小包 → FEC 块数增加，可能触发「关 FEC」 |
| `stream.ping_timeout` | 同上 | `10 s` | 整数，`>= -1` |
| `minRequiredFecPackets` | SDP `x-nv-vqos[0].fec.minRequiredFecPackets` | `2` | 校验分片下限，会抬高实际 FEC 百分比 |
| `MAX_FEC_BLOCKS` | [src/stream.cpp:1608](../../../src/stream.cpp#L1608) | `4` | 协议 2 bit 上限 |
| `DATA_SHARDS_MAX` | [nanors/rs.h:8](../../../third-party/moonlight-common-c/nanors/rs.h#L8) | `255` | RS 库上限 |
| `MAX_AUDIO_PACKET_SIZE` | [src/stream.cpp](../../../src/stream.cpp) | `1400` | 音频载荷上限 |
| `MAX_RTP_HEADER_SIZE` | [Video.h:40](../../../third-party/moonlight-common-c/src/Video.h#L40) | `16` | 计算 `blocksize` 用 |
| `round_to_pkcs7_padded(2048)` | [src/stream.cpp:2344](../../../src/stream.cpp#L2344) | `2048` | 音频分片大小（AES-CBC 需要 PKCS#7 对齐） |

**调优直觉**：

| 目标 | 手段 | 代价 |
|------|------|------|
| 减少丢包导致的花屏 | 调高 `fec_percentage` | 带宽上升（20% → 50% 约多 25% 带宽） |
| 降低延迟 | 调低 `fec_percentage`（甚至 0） | 一旦丢包只能等 IDR，卡顿更明显 |
| 降低单包开销 | 调大 `packetsize` | 丢一个包损失更大；MTU 之外会被分片 |
| 静止画面即时恢复 | 依赖 RFI（客户端支持时自动） | — |

---

## 9. 小结

| 问题 | 答案 |
|------|------|
| 有 FEC 吗？ | 有。视频自适应百分比 + 最多 4 个 RS 块；音频固定 4+2 |
| 有重传吗？ | **应用层没有**。只有 ENet 控制通道内部有 ACK/重传 |
| 丢包怎么恢复？ | 客户端发起 `IDX_REQUEST_IDR_FRAME` / `IDX_INVALIDATE_REF_FRAMES` |
| 有码率自适应吗？ | **没有**。`IDX_LOSS_STATS` 只写日志 |
| 有丢帧策略吗？ | **没有显式策略**。`mail::video_packets` 满 32 就整队 `clear()` |
| 心跳有几套？ | 两套：ENet 控制通道超时 + UDP 每流初始 PING（外加 RTSP launch 事件超时） |
| 默认超时多久？ | `config::stream.ping_timeout` = 10 秒（RTSP Session 头声明 90 秒） |
| PING 除了保活还干什么？ | 发现客户端真实 UDP 端口 → 建 QoS → 启动采集编码 |

下一篇：[06 · 采集与编码](./06-采集与编码.md)
