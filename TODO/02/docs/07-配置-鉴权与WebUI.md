# 07 · 配置系统 · 鉴权 · 持久化 · WebUI

> 本篇回答四个问题：
> 1. `sunshine.conf` 是怎么被读进来、校验、应用、写回磁盘的？
> 2. TLS 证书、WebUI 用户名密码这些**凭据**存在哪、怎么生成？
> 3. WebUI 的请求是怎么被**鉴权**的（Basic Auth / 来源准入 / CSRF）？
> 4. WebUI 暴露了哪些**页面与 REST API**，它们各自改动了什么状态？

主要源文件：

| 文件 | 职责 |
|------|------|
| [../../../src/config.h](../../../src/config.h) | 所有配置结构体（`video_t`/`audio_t`/`stream_t`/`nvhttp_t`/`input_t`/`sunshine_t`）与解析函数声明 |
| [../../../src/config.cpp](../../../src/config.cpp) | 默认值、文件解析、键消费、默认值回写 |
| [../../../src/httpcommon.cpp](../../../src/httpcommon.cpp) | 证书生成、凭据文件读写、`http::init` |
| [../../../src/confighttp.cpp](../../../src/confighttp.cpp) | WebUI 服务器：认证、CSRF、路由、全部 REST 端点 |
| [../../../src/nvhttp.cpp](../../../src/nvhttp.cpp) | GameStream HTTP/HTTPS 服务器、配对会话、已配对客户端数据库、`sunshine_state.json` |
| [../../../src/file_handler.h](../../../src/file_handler.h) | `appdata()` 与原子读写文件 |

---

## §1 配置系统

### 1.1 三个配置文件

Sunshine 只有三处磁盘状态，全部位于 `appdata()` 目录（Windows：`%ProgramData%\Sunshine\config`；Linux：`~/.config/sunshine`；macOS：`~/Library/Application Support/Sunshine`）：

| 文件 | 变量 | 默认值 | 内容 |
|------|------|--------|------|
| `sunshine.conf` | `config::sunshine.config_file` | `appdata()/sunshine.conf` | 用户可编辑的键值配置 |
| `sunshine_state.json` | `config::nvhttp.file_state` | `"sunshine_state.json"`（[../../../src/config.cpp:854](../../../src/config.cpp:854)） | 已配对客户端证书数据库 + 主机 `uniqueid` |
| `display_device.state` | 由 `display_device::init` 管理 | `appdata()/display_device.state` | 显示设备持久化状态 |

`sunshine_state.json` 有**双重身份**：它既是 GameStream 的配对数据库，也是 WebUI 的凭据文件。关联发生在：

```cpp
// src/config.cpp:1769-1773
path_f(vars, "file_state", nvhttp.file_state);
// Must be run after "file_state"
config::sunshine.credentials_file = config::nvhttp.file_state;
path_f(vars, "credentials_file", config::sunshine.credentials_file);
```

也就是说：把 `file_state` 指向别的路径，凭据文件也跟着搬；而 `credentials_file` 可以在其后被单独覆盖。

### 1.2 `sunshine.conf` 语法

配置是极简的 `key = value` 文本，没有节（section）、没有嵌套、没有引号转义：

```ini
# 这是注释，整行以 # 开头
port = 47989
pkey = /path/to/key.pem
address_family = ipv4

# 某些键接受列表，用逗号分隔
prep-cmd = [
  {do: "cmd1", undo: "cmd2"},
]
```

解析入口 `config::parse_config`（[../../../src/config.cpp:1040](../../../src/config.cpp:1040)）：

```cpp
std::unordered_map<std::string, std::string> parse_config(const std::string_view file_content) {
  std::unordered_map<std::string, std::string> vars;

  auto lines = split(file_content, '\n');

  for (auto &line : lines) {
    auto pos = line.find_first_of('=');
    if (pos == std::string::npos) {
      // 不含 = 的行直接跳过（空行、注释、纯文本）
      continue;
    }

    auto name = trim(line.substr(0, pos));
    auto value = trim(line.substr(pos + 1, std::string::npos));

    vars.emplace(std::move(name), std::move(value));
  }

  return vars;
}
```

要点：

* 结果类型是 `std::unordered_map<std::string, std::string>` —— **所有值先按字符串存**，类型转换推迟到消费时。
* 使用 `emplace`，因此**同名键只保留第一次出现的值**（后出现的被静默忽略）。
* `#` 注释只在没有 `=` 时才被跳过；`foo = #bar` 会把 `#bar` 当值。

### 1.3 `apply_config`：消费式解析

`config::apply_config`（[../../../src/config.cpp:1622](../../../src/config.cpp:1622)）是真正的语义层。它接收字符串 map，逐个键用一族模板辅助函数读取，**读到就 `erase`**：

```
apply_config(vars)
   │
   ├─ log_config_settings(vars, /*save=*/true)        ← 先打印全部原始键值
   │
   ├─ int_f(vars, "port", sunshine.port)               ┐
   ├─ int_between_f(vars, "ping_timeout", ..., [](auto x){ return x >= -1; })
   ├─ bool_f(vars, "upnp", flags[...])                 ├─ 每个 f 函数消费一个键
   ├─ string_f(vars, "locale", sunshine.locale)        │  （消费即 erase）
   ├─ string_restricted_f(vars, "locale", whitelist)   │
   ├─ generic_f(vars, "prep-cmd", parse_prep_cmds)     ┘
   ├─ ...
   │
   ├─ apply_flags(vars)                                ← flags = a,b,c
   │
   └─ for (auto &[name, _] : vars)                     ← 剩下的都是未识别键
        if (min_log_level <= 3)
          BOOST_LOG(warning) << "Warning: Unrecognized configurable option [" << name << "]";
```

`*_f` 族的语义（[../../../src/config.cpp:1092](../../../src/config.cpp:1092) 起）：

| 辅助函数 | 行为 |
|----------|------|
| `string_f(vars, name, target)` | 存在则赋值 |
| `string_restricted_f(vars, name, target, allowed)` | 赋值前必须在白名单内，否则告警并丢弃 |
| `path_f(vars, name, target)` | 赋值并做路径规范化/存在性检查 |
| `int_f(vars, name, target)` | `std::stoi`，解析失败告警 |
| `int_between_f(vars, name, target, validator)` | 额外跑断言 lambda，越界则告警丢弃 |
| `bool_f(vars, name, target)` | 接受 `true`/`false`/`1`/`0`/`enabled`/`disabled` 等 |
| `generic_f(vars, name, consumer)` | 交给自定义 lambda（列表、复合结构） |

**「未被任何 `*_f` 消费」= 拼写错误**，会在末尾变成 `Warning: Unrecognized configurable option`。这是排查「我的配置没生效」的第一条线索。

内置的白名单之一 —— `locale`（[../../../src/config.cpp:1916](../../../src/config.cpp:1916)）：

```
bg, cs, de, en, en_GB, en_US, es, fr, hu, it, ja, ko, pl, pt, pt_BR,
ru, sv, tr, uk, vi, zh, zh_TW
```

`min_log_level` 接受名字或 0–6 数字字符（[../../../src/config.cpp:1940](../../../src/config.cpp:1940)）：

| 名字 | 数值 |
|------|:----:|
| `verbose` | 0 |
| `debug` | 1 |
| `info` | 2 |
| `warning` | 3 |
| `error` | 4 |
| `fatal` | 5 |
| `none` | 6 |

### 1.4 命令行覆盖与优先级

`config::parse(argc, argv)`（[../../../src/config.cpp:1996](../../../src/config.cpp:1996)）的完整优先级：

```
  高  ┌──────────────────────────────────────────────┐
      │ 命令行 `key=value` 参数                       │  同名键先 erase 再 emplace
   ↑  ├──────────────────────────────────────────────┤
   │  │ sunshine.conf 文件内容                       │  parse_config → map
   │  ├──────────────────────────────────────────────┤
      │ 编译期默认值（config.cpp 里的聚合初始化）      │
  低  └──────────────────────────────────────────────┘
```

分步流程：

```
parse(argc, argv)
  ├─ --help                → logging::print_help() 返回 1
  ├─ Windows: --shortcut / --shortcut-admin  → 提权重启自身/等待服务，返回 1
  ├─ --<name> [args...]    → sunshine.cmd = {name, argc, argv}; break
  │                          （子命令：creds / help / version / restore-nvprefs-undo）
  ├─ -x                    → 走 apply_flags 语义
  ├─ 含 '=' 的参数         → sunshine.cmd 之后的命令行覆盖项：vars.erase(k); vars.emplace(k, v)
  └─ 其它裸参数            → sunshine.config_file = 该路径
       │
       ▼
     file_handler::make_directory(appdata())
     if (!fs::exists(sunshine.config_file)) { std::ofstream {sunshine.config_file}; }   // 建空文件
     auto vars = parse_config(file_handler::read_file(sunshine.config_file.c_str()));
     for (cmd-line vars) vars.insert_or_assign(k, v);
     apply_config(vars);
```

* 命令行参数**后面**可以通过 `--` 之外的裸参数指定配置文件路径；例如 `sunshine.exe C:\my.conf port=48000`。
* 配置不存在时会**创建一个空文件**（[../../../src/config.cpp:2060](../../../src/config.cpp:2060)），这样首次运行不会报错。
* 子命令分发在 [../../../src/main.cpp](../../../src/main.cpp) 的 `cmd_to_func`；未知子命令退出码 **7**。
* 应用失败时（`std::filesystem::filesystem_error` / `boost::filesystem::filesystem_error`）打 fatal `Failed to apply config: ...`；Windows 上非 shortcut 启动还会提示：

  ```
  To relaunch Sunshine successfully, use the shortcut in the Start Menu. Do not run Sunshine.exe manually.
  ```

  随后 sleep 10 秒并返回 −1。

### 1.5 追加式默认值回写

有些默认值需要在运行时**被写进配置文件**（让用户看得见、可改），但又不能覆盖用户已有的设置。这是 `persist_config_option_if_missing`（[../../../src/config.cpp:1065](../../../src/config.cpp:1065)）的职责：

```
persist_config_option_if_missing(name, value)
  │
  ├─ file_content = read_file(sunshine.config_file)
  ├─ 若 file_content 里已经出现该键  →  return false（不动文件）
  ├─ 若末尾不是 '\n'               →  补一个 '\n'
  ├─ file_content += std::format("{} = {}\n", name, value)
  ├─ write_file(...) 失败          →  日志 "Failed to persist automatically selected config option '<name>'"
  └─ 成功                          →  日志 "Automatically selected config option '<name>' = <value>"，return true
```

这种「读—改—整文件写回」保留了用户注释与格式（相比之下 `saveConfig` 会整文件重写，见 §6.1）。

当前使用它的唯一路径是 Windows 的手柄驱动选择：

```cpp
// src/config.cpp:1084
bool select_all_gamepad_drivers_if_licensed(const bool virtualhid_licensed) {
  // 三个条件同时满足才写入：
  //   1. libvirtualhid 已授权（virtualhid_licensed）
  //   2. input.gamepad_driver 目前为空（尚未选择策略）
  //   3. 持久化成功
  // 结果：input.gamepad_driver = "all"
}
```

对应键常量：`GAMEPAD_DRIVER_ALL = "all"`、`GAMEPAD_DRIVER_VIRTUALHID = "virtualhid"`、`GAMEPAD_DRIVER_VIGEMBUS = "vigembus"`（[../../../src/config.h:49](../../../src/config.h:49)）。

### 1.6 日志与脱敏

`log_config_settings(vars, save)`（[../../../src/config.cpp:1605](../../../src/config.cpp:1605)）在 `apply_config` 最开头执行，逐条打印：

```
config: '<name>' = <val>
```

* 键名命中 `config::redacted_config` 时值被替换为 `[redacted]`。当前唯一的脱敏键是 `csrf_allowed_origins`（[../../../src/config.h:60](../../../src/config.h:60)）。
* `save == true` 时同时收集到 `config::modified_config_settings`（[../../../src/config.h:54](../../../src/config.h:54)）—— 这份集合供 WebUI/诊断页展示「本次启动实际生效的配置」。

### 1.7 配置键总览（按结构体分组）

#### `sunshine_t`（[../../../src/config.h:374](../../../src/config.h:374)）

| 键 | 类型 | 说明 |
|----|------|------|
| `locale` | 白名单字符串 | WebUI 与托盘语言 |
| `min_log_level` | 0–6 或名字 | 日志下限 |
| `flags` | 逗号列表 | 见下 |
| `credentials_file` | 路径 | 默认跟随 `file_state` |
| `port` | int | 端口基准（见 `01-总览与启动流程.md` 端口表） |
| `address_family` | `ipv4`\|`ipv6`\|`both` | 绑定地址族 |
| `bind_address` | 字符串 | 指定绑定 IP |
| `log_file` | 路径 | 落盘日志 |
| `notify_pre_releases` | bool | 预告版本更新通知 |
| `system_tray` | bool | 托盘图标 |
| `prep-cmd` | 列表 | 启动/结束前后执行的命令 |
| `csrf_allowed_origins` | 列表 | 允许跨源提交的 Origin 白名单（脱敏） |

`flags` 枚举（[../../../src/config.h:325](../../../src/config.h:325)）：

| 名字 | 值 | 含义 |
|------|:--:|------|
| `PIN_STDIN` | 0 | 配对 PIN 从标准输入读取，而不是 WebUI |
| `FRESH_STATE` | 1 | 不加载任何持久状态（见 §1.8） |
| `FORCE_VIDEO_HEADER_REPLACE` | 2 | 强制替换视频头 |
| `UPNP` | 3 | 启用 UPnP 端口映射 |
| `CONST_PIN` | 4 | 固定 PIN，便于自动化测试 |

`prep_cmd_t`（[../../../src/config.h:337](../../../src/config.h:337)）字段为 `do_cmd` / `undo_cmd` / `elevated`：启动应用前执行 `do_cmd`，退出后执行 `undo_cmd`；`elevated` 决定是否需要提权。

#### `stream_t`（[../../../src/config.h:262](../../../src/config.h:262)）

| 键 | 默认值 | 说明 |
|----|:------:|------|
| `ping_timeout` | `10s`（int ≥ −1） | 三种超时的共同上限，详见 [05](./05-FEC-重传-丢帧与心跳.md) §6 |
| `file_apps` | 路径 | 应用清单 JSON |
| `fec_percentage` | `20`（范围 1–255） | RS 校验分片百分比 |
| `lan_encryption_mode` | `0` | 局域网加密策略 |
| `wan_encryption_mode` | `1` | 广域网加密策略 |
| `packetsize` | `0`（范围 0–65535） | 0 = 自动；`PACKETSIZE_SMALL=500` / `PACKETSIZE_LARGE=1456` |

加密模式常量（[../../../src/config.h:248](../../../src/config.h:248)）：

| 常量 | 值 | 语义 |
|------|:--:|------|
| `ENCRYPTION_MODE_NEVER` | 0 | 从不加密 |
| `ENCRYPTION_MODE_OPPORTUNISTIC` | 1 | 客户端支持就加密 |
| `ENCRYPTION_MODE_MANDATORY` | 2 | 不满足即拒绝（RTSP `cmd_announce` 返回 403） |

#### `nvhttp_t`（[../../../src/config.h:281](../../../src/config.h:281)）

| 键 | 默认值 | 说明 |
|----|:------:|------|
| `origin_web_ui_allowed` | `"lan"` | WebUI 来源准入：`pc` / `lan` / `wan` |
| `pkey` | `PRIVATE_KEY_FILE` | TLS 私钥 |
| `cert` | `CERTIFICATE_FILE` | TLS 证书 |
| `sunshine_name` | `platf::get_host_name()` | 客户端看到的服务器名 |
| `file_state` | `sunshine_state.json` | 配对数据库 + 凭据 |
| `external_ip` | 空 | `/serverinfo` 回填的 `ExternalIP` |

#### `input_t`（[../../../src/config.h:297](../../../src/config.h:297)）

| 键 | 默认值 | 说明 |
|----|:------:|------|
| `keybindings` | 见下 | 三组快捷键映射 |
| `back_button_timeout` | `-1ms` | 长按 BACK 触发 HOME 的阈值，−1 禁用 |
| `key_repeat_delay` | `500ms` | 重复前延迟 |
| `key_repeat_period` | `1/24.9` ≈ 40 ms | 重复周期 |
| `gamepad` | 平台首个支持的设备名 | 虚拟手柄类型 |
| `gamepad_driver` | 空 | Windows 驱动策略，见 §1.5 |
| `ds4_back_as_touchpad_click` | `true` | DS4 的 BACK 映射为触摸板点击 |
| `motion_as_ds4` / `touchpad_as_ds4` | `true` | 有运动/触摸板的客户端手柄按 DS4 模拟 |
| `virtualhid_randomize_mac` | `true` | 虚拟设备 MAC 随机化 |
| `keyboard` / `mouse` / `controller` | 平台默认 | 设备类型覆盖 |
| `key_rightalt_to_key_win` | 平台默认 | 右 Alt → Win 键 |
| `always_send_scancodes` | 平台默认 | 强制扫描码路径 |
| `high_resolution_scrolling` | 平台默认 | 高分辨率滚轮 |
| `native_pen_touch` | 平台默认 | 原生笔/触摸 |

默认 `keybindings`（[../../../src/config.cpp:863](../../../src/config.cpp:863)）为 `{SHIFT→LSHIFT, CTRL→LCTRL, ALT→LALT}` 三对。

#### `video_t` / `audio_t`

`video_t`（[../../../src/config.h:71](../../../src/config.h:71)）与 `audio_t`（[../../../src/config.h:239](../../../src/config.h:239)）的键极多，分别在 [04](./04-码率控制与QoS.md) §4 与 [06](./06-采集与编码.md) 中按功能列出。这里只给出**解析命名空间与起始行**，便于定位：

| 子命名空间 | `config.cpp` 起始行 | 覆盖内容 |
|------------|:------------------:|----------|
| `nv` | 65 | NVENC 全部选项 |
| `amd` | 132 | AMF 全部选项 |
| `qsv` | 362 | QuickSync |
| `vaapi` | 429 | VAAPI |
| `vt` | 512 | VideoToolbox |
| `sw` | 587 | 软件编码器（x264/x265/SVT-AV1） |
| `dd` | 614 | 显示设备配置选项 |

`video_t::dd_t` 的子枚举：`config_option_e{disabled, verify_only, ensure_active, ensure_primary, ensure_only_display}`、`resolution_option_e{disabled, automatic, manual}`、`refresh_rate_option_e{disabled, automatic, manual}`、`hdr_option_e{disabled, automatic}`，以及 `mode_remapping_entry_t{requested_resolution, requested_fps, final_resolution, final_refresh_rate}` 与 `mode_remapping_t{mixed, resolution_only, refresh_rate_only}` 两张重映射表。

### 1.8 `FRESH_STATE`：把所有持久状态踢到临时目录

`config::flag::FRESH_STATE` 是一个贯穿全局的开关，作用是**让这次进程运行不读写任何长期状态**：

| 位置 | FRESH_STATE 行为 |
|------|------------------|
| `http::init`（[../../../src/httpcommon.cpp:55](../../../src/httpcommon.cpp:55)） | 生成新 `http::unique_id`，证书与私钥改指向 `temp_directory_path()/"Sunshine"/cert-<id>` 与 `pkey-<id>` |
| `nvhttp::start`（[../../../src/nvhttp.cpp:1604](../../../src/nvhttp.cpp:1604)） | 跳过 `load_state()`，即不读 `sunshine_state.json` |
| `add_authorized_client`（[../../../src/nvhttp.cpp:441](../../../src/nvhttp.cpp:441)） | 新配对客户端**不写盘** |
| `http::save_user_creds` | 写入临时路径下的凭据文件 |

用途：CI 测试、多实例并行、一次性调试会话。

### 1.9 测试专用入口

```cpp
#ifdef SUNSHINE_TESTS
  int apply_config_for_test(const std::string_view file_content);
#endif
```

（[../../../src/config.h:429](../../../src/config.h:429)、[../../../src/config.cpp:1988](../../../src/config.cpp:1988)）等价于 `apply_config(parse_config(file_content))`，让单元测试跳过文件系统直接验证解析语义。

---

## §2 凭据：TLS 证书与 WebUI 密码

### 2.1 `http::init`：证书生命周期

`http::init`（[../../../src/httpcommon.cpp:55](../../../src/httpcommon.cpp:55)）在 `main()` 的**致命初始化阶段**执行（失败即 `return -1`）：

```
http::init()
  │
  ├─ FRESH_STATE?
  │    ├─ 是 → unique_id = uuid_util::uuid_t::generate().string()
  │    │        cert = temp_directory_path()/"Sunshine"/("cert-" + unique_id)
  │    │        pkey = temp_directory_path()/"Sunshine"/("pkey-" + unique_id)
  │    └─ 否 → 使用 config::nvhttp.cert / .pkey
  │
  ├─ pkey 或 cert 不存在?
  │    └─ create_creds(pkey, cert)
  │         └─ crypto::gen_creds("Sunshine Gamestream Host", 2048)
  │              写 pkey（权限 0600）、cert（权限 0644）
  │
  ├─ credentials_file 不存在?
  │    └─ 日志 "Open the Web UI to set your new username and password and getting started"
  │       （不阻塞启动；此时 WebUI 会把人重定向到 /welcome）
  └─ 存在 → reload_user_creds()
       │
       └─ origin_web_ui_allowed = net::from_enum_string(config::nvhttp.origin_web_ui_allowed)
```

要点：

* 自签证书 CN 固定为 `"Sunshine Gamestream Host"`，RSA **2048** 位。因此浏览器永远会警告「证书不受信任」——这是设计如此，不是 bug。
* `pkey` 与 `cert` 分开存放，私钥权限 `0600`（仅属主可读）。
* 凭据文件缺失**不影响启动**，只影响 WebUI 能否直接登录。

### 2.2 凭据文件格式与哈希

凭据以 JSON 形式存在 `config::sunshine.credentials_file` 里（即默认的 `sunshine_state.json`）：

```json
{
  "root": { "uniqueid": "…", "named_devices": [ … ] },
  "username": "admin",
  "salt": "Ab3xZ9…",          // 16 个字母
  "password": "3f2a…"          // hex(hash(password + salt))
}
```

`save_user_creds(file, username, password, run_our_mouth)`（[../../../src/httpcommon.cpp:88](../../../src/httpcommon.cpp:88)）：

```
save_user_creds()
  ├─ pt::read_json(file)                      ← 保留 root.uniqueid 与 named_devices
  ├─ salt = crypto::rand_alphabet(16)         ← 每次改密码都换新盐
  ├─ 写入 username
  ├─ 写入 salt
  ├─ 写入 password = util::hex(crypto::hash(password + salt)).to_string()
  ├─ pt::write_json(file, root)
  │    ├─ 成功 → 日志 "New credentials have been created"
  │    └─ 失败 → 提示 "perhaps try this again as an administrator?"
```

三个函数各司其职：

| 函数 | 行号 | 行为 |
|------|------|------|
| `save_user_creds` | [88](../../../src/httpcommon.cpp:88) | 加盐哈希并写盘 |
| `user_creds_exist` | [115](../../../src/httpcommon.cpp:115) | JSON 中 `username`/`password`/`salt` **三个键都要有**才算存在 |
| `reload_user_creds` | [135](../../../src/httpcommon.cpp:135) | 把三者装入 `config::sunshine.{username,password,salt}` |
| `create_creds` | [151](../../../src/httpcommon.cpp:151) | 生成 TLS cert/pkey（与用户密码无关） |

**关键设计：服务端不存明文密码，也不存 `hash(密码)`，而是存 `hash(密码 + 盐)`。** 验证时现场重算。因此 `sunshine_state.json` 被读走也不等于密码泄露（但仍应保护，因为配对证书在里面）。

### 2.3 验证逻辑

`authenticate`（[../../../src/confighttp.cpp:595](../../../src/confighttp.cpp:595)）的判定顺序：

```
authenticate(response, request)
  │
  1. 来源地址准入
  │    if (net::from_address(request->remote_endpoint().address()) > config::sunshine.origin_web_ui_allowed)
  │         → 403 Forbidden，返回 false
  │    （"pc" < "lan" < "wan" 是枚举序，所以 "lan" 也放行本机）
  │
  2. 尚未设置用户名
  │    if (config::sunshine.username.empty())
  │         → 302 重定向到 /welcome，返回 false
  │
  3. 缺少 Authorization 头
  │    → send_unauthorized(response, request)，返回 false
  │
  4. 解析 "Basic <base64>"
  │    base64 解码 → 按第一个 ':' 切成 user / pass
  │
  5. 校验
  │    hash = util::hex(crypto::hash(pass + config::sunshine.salt))
  │    if (!boost::iequals(user, sunshine.username) || hash != sunshine.password)
  │         → 401，返回 false
  │
  └─ 通过 → 返回 true
```

401 响应带标准质询头：

```
WWW-Authenticate: Basic realm="Sunshine Gamestream Host", charset="UTF-8"
```

注意第 5 步用 `boost::iequals`，**用户名大小写不敏感、密码大小写敏感**（因为密码是哈希比较）。

### 2.4 统一安全响应头

所有由 `send_response`（[../../../src/confighttp.cpp:538](../../../src/confighttp.cpp:538)）产出的 JSON 响应都固定携带：

```
Content-Type: application/json
X-Frame-Options: DENY
Content-Security-Policy: frame-ancestors 'none';
```

页面/图标/静态资源分别用各自的 `Content-Type`，但 `X-Frame-Options: DENY` 与 CSP 同样存在（见 `getPage`、`getFaviconImage`、`getSunshineLogoImage`、`getAsset`）。作用是**阻止 WebUI 被 iframe 嵌套**，从而封死点击劫持（clickjacking）路径。

### 2.5 `check_content_type`

所有 POST/DELETE 端点第一步都调用（[../../../src/confighttp.cpp:684](../../../src/confighttp.cpp:684)）：

```
check_content_type(response, request, "application/json")
  ├─ 取请求 Content-Type
  ├─ 截断 ';' 之后的参数（charset 等）
  ├─ trim + 转小写
  ├─ 缺失      → 400 "Content type not provided"
  ├─ 不匹配    → 400 "Content type mismatch"
  └─ 匹配      → true
```

这是对**跨站表单提交**的第一道拦截：HTML `<form>` 只能发 `application/x-www-form-urlencoded` / `multipart/form-data` / `text/plain`，发不出 `application/json`。

---

## §3 CSRF 防护

### 3.1 常量与存储

```cpp
// src/confighttp.cpp:205-209
constexpr auto CSRF_TOKEN_SIZE = 32;                            // 32 字节 = 256 位
constexpr auto CSRF_TOKEN_LIFETIME = std::chrono::hours(1);     // 有效期 1 小时
```

存储结构是 `std::unordered_map<std::string, csrf_token_t>`，`csrf_token_t{token, expires_at}`，配套 `csrf_tokens_mutex` 串行化访问。

### 3.2 客户端标识

`get_client_id(request)`（[../../../src/confighttp.cpp:715](../../../src/confighttp.cpp:715)）：

```
① 若 Authorization: Basic 能解码出用户名 → 用用户名
② 否则 → net::addr_to_normalized_string(request->remote_endpoint().address())
```

优先级设计意图：同一个 NAT 后的多台设备共享出口 IP，若用 IP 做键会互相覆盖 token；有用户名时更精确。反过来，未登录/无凭据的请求回退到 IP，避免匿名请求间互相干扰。

### 3.3 生成

`generate_csrf_token(client_id)`（[../../../src/confighttp.cpp:735](../../../src/confighttp.cpp:735)）：

```
generate_csrf_token(client_id)
  ├─ token = crypto::rand_alphabet(CSRF_TOKEN_SIZE)      ← CSPRNG
  ├─ lock(csrf_tokens_mutex)
  ├─ std::erase_if(csrf_tokens, [](auto &e){ return e.second.expires_at < now; })   ← 顺手清理过期项
  ├─ csrf_tokens[client_id] = { token, now + 1h }        ← 一客户端一 token，新 token 覆盖旧 token
  └─ return token
```

**一客户端同时只有一个有效 token**：再次调用会作废旧 token。前端因此只在会话开始时取一次。

### 3.4 校验：三分支决策树

`validate_csrf_token`（[../../../src/confighttp.cpp:796](../../../src/confighttp.cpp:796)）：

```
validate_csrf_token(response, request, client_id)
  │
  ├─ 取 Origin 头；没有则取 Referer 头
  │
  ├─ 分支 A：origin 存在，且前缀匹配 config::sunshine.csrf_allowed_origins 中任一项
  │    （要求其后紧跟 ':' 或 '/' 或完全相等 —— 防止 "malicious.com" 匹配 "allowed.com"）
  │      → 同源，放行 ✔
  │
  ├─ 分支 B：Origin 与 Referer 都不存在
  │    → 放行 ✔   （注释：curl / 脚本等非浏览器客户端从不发这些头；
  │                 恶意网页也无法代浏览器伪造它们）
  │
  └─ 分支 C：跨源浏览器请求
       ├─ 先查头 X-CSRF-Token
       ├─ 再查查询参数 csrf_token
       ├─ 都没有 → 400 "Missing CSRF token"
       │           并提示可将该 origin 加入 csrf_allowed_origins
       └─ 有 → validate_stored_csrf_token
```

`validate_stored_csrf_token`（[../../../src/confighttp.cpp:764](../../../src/confighttp.cpp:764)）：

| 情况 | 日志 | 响应 |
|------|------|------|
| 该 client 无 token | `no token found for client` | 400 `Invalid CSRF token` |
| token 已过期 | （删除该条目） | 400 `CSRF token expired` |
| token 不匹配 | `token mismatch` | 400 `Invalid CSRF token` |

### 3.5 一次完整的受保护请求

```
浏览器                         Sunshine WebUI (47990)
  │                                   │
  │  GET /api/csrf-token              │  ← authenticate()：Basic Auth
  │  Authorization: Basic …           │     get_client_id → "admin"
  │ ─────────────────────────────────►│     generate_csrf_token → "9f3c…"
  │  {"csrf_token":"9f3c…"}           │
  │ ◄─────────────────────────────────│
  │                                   │
  │  POST /api/config                 │  ← check_content_type("application/json")
  │  Authorization: Basic …           │     authenticate()
  │  Content-Type: application/json   │     get_client_id → "admin"
  │  X-CSRF-Token: 9f3c…              │     validate_csrf_token → 命中分支 C，比对成功
  │  {"port":"47989"}                 │     写盘 + 返回 {"status":true}
  │ ─────────────────────────────────►│
  │  {"status":true}                  │
  │ ◄─────────────────────────────────│
```

---

## §4 路由表

### 4.1 页面（`require_auth = true` 除非另注）

| 正则 | HTML 文件 | 备注 |
|------|-----------|------|
| `^/$` | `index.html` | 首页/主机页 |
| `^/apps/?$` | `apps.html` | 应用管理 |
| `^/clients/?$` | `clients.html` | 已配对客户端 |
| `^/config/?$` | `config.html` | 配置页 |
| `^/featured/?$` | `featured.html` | 精选应用 |
| `^/logout/?$` | — | `require_auth = false` |
| `^/password/?$` | `password.html` | 修改密码 |
| `^/pin/?$` | `pin.html` | 配对 PIN 输入 |
| `^/troubleshooting/?$` | `troubleshooting.html` | 诊断页 |
| `^/welcome/?$` | `welcome.html` | `require_auth = false`、`redirect_if_username = true` |

`page_handler(file, require_auth, redirect_if_username)` 是生成这些处理器的工厂 lambda。`getPage`（[../../../src/confighttp.cpp:882](../../../src/confighttp.cpp:882)）第一件事就是：

```cpp
if (redirect_if_username && !config::sunshine.username.empty()) {
  send_redirect(response, request, "/");
  return;
}
```

即：**只要已经设置过用户名，`/welcome` 就再也进不去**，直接踢回首页。

### 4.2 REST API

| 方法 | 路径 | 处理函数（行号） | 作用 |
|:----:|------|-----------------|------|
| GET | `/api/browse` | `browseDirectory`（[2234](../../../src/confighttp.cpp:2234)） | 目录浏览（选可执行文件）；有穿越防护 |
| GET | `/api/apps` | `getApps`（[1025](../../../src/confighttp.cpp:1025)） | 应用列表（含 `index`、`file-path` 检查） |
| POST | `/api/apps` | `saveApp`（[1110](../../../src/confighttp.cpp:1110)） | 新增应用 |
| DELETE | `/api/apps/([0-9]+)` | `deleteApp`（[1211](../../../src/confighttp.cpp:1211)） | 删除指定下标应用 |
| POST | `/api/apps/close` | `closeApp`（[1185](../../../src/confighttp.cpp:1185)） | 关闭当前运行的应用 |
| GET | `/api/clients/list` | `getClients`（[1262](../../../src/confighttp.cpp:1262)） | 已配对客户端列表 |
| POST | `/api/clients/unpair` | `unpair`（[1345](../../../src/confighttp.cpp:1345)） | 解除单个客户端绑定 |
| POST | `/api/clients/unpair-all` | `unpairAll`（[1389](../../../src/confighttp.cpp:1389)） | 清空全部绑定 |
| POST | `/api/clients/update` | `updateClient`（[1291](../../../src/confighttp.cpp:1291)） | 改名 / 启用停用 |
| GET | `/api/config` | `getConfig`（[1416](../../../src/confighttp.cpp:1416)） | 读取原始配置键值 |
| POST | `/api/config` | `saveConfig`（[1470](../../../src/confighttp.cpp:1470)） | **整文件重写**配置 |
| GET | `/api/configLocale` | `getLocale`（[1444](../../../src/confighttp.cpp:1444)） | 仅返回 `locale`（**不需鉴权**） |
| GET | `/api/covers/([0-9]+)` | `getCover`（[1519](../../../src/confighttp.cpp:1519)） | 取应用封面 |
| POST | `/api/covers/upload` | `uploadCover`（[1589](../../../src/confighttp.cpp:1589)） | 上传封面 |
| GET | `/api/csrf-token` | `getCSRFToken`（[1003](../../../src/confighttp.cpp:1003)） | 领取 CSRF token |
| POST | `/api/password` | `savePassword`（[1677](../../../src/confighttp.cpp:1677)） | 改用户名/密码 |
| GET | `/api/pin` | `getPendingPairings`（[1746](../../../src/confighttp.cpp:1746)） | 待审批配对的客户端列表 |
| POST | `/api/pin` | `savePin`（[1827](../../../src/confighttp.cpp:1827)） | 提交 PIN 完成配对 |
| DELETE | `/api/pin` | `cancelPairing`（[1776](../../../src/confighttp.cpp:1776)） | 取消某次待审批配对 |
| GET | `/api/logs` | `getLogs`（[1645](../../../src/confighttp.cpp:1645)） | 下载日志文件 |
| POST | `/api/reset-display-device-persistence` | `resetDisplayDevicePersistence`（[1878](../../../src/confighttp.cpp:1878)） | 清除显示设备持久化 |
| POST | `/api/reset-portal-token` | `resetPortalToken`（[1904](../../../src/confighttp.cpp:1904)） | 删除 XDG Portal restore token |
| POST | `/api/restart` | `restart`（[1939](../../../src/confighttp.cpp:1939)） | `platf::restart()`，可能不返回 |
| GET | `/api/virtual-input/license` | `getVirtualInputLicense`（[2053](../../../src/confighttp.cpp:2053)） | libvirtualhid 许可证状态 |
| POST | `/api/virtual-input/license` | `updateVirtualInputLicense`（[2069](../../../src/confighttp.cpp:2069)） | 更新许可证 |
| GET | `/api/virtual-input/status` | `getVirtualInputStatus`（[2032](../../../src/confighttp.cpp:2032)） | 驱动版本与后端状态 |

**默认处理器策略**（[../../../src/confighttp.cpp:2338](../../../src/confighttp.cpp:2338) 附近）：

| 方法 | 未匹配路径的响应 |
|:----:|-----------------|
| `DELETE` / `PATCH` / `POST` / `PUT` | `bad_request`（400） |
| `GET` | `not_found`（404） |

### 4.3 静态资源与目录穿越防护

| 正则 | 文件 |
|------|------|
| `^/images/sunshine.ico$` | 图标（`getFaviconImage`，[913](../../../src/confighttp.cpp:913)） |
| `^/images/logo-sunshine-45.png$` | Logo（`getSunshineLogoImage`，[931](../../../src/confighttp.cpp:931)） |
| `^/assets\/.+$` | 前端打包产物（`getAsset`，[958](../../../src/confighttp.cpp:958)） |

`getAsset` 的防护链：

```
1. filePath = fs::weakly_canonical(WEB_DIR / request->path.relative_path())
      └─ relative_path() 去掉前导 '/'，weakly_canonical 解析 '..'
2. isChildPath(filePath, WEB_DIR/"assets")
      └─ 用 fs::relative(base, query) 的首段是否等于 ".." 判断
      └─ 不在 assets 内 → 日志 "Someone requested a path … that is outside the assets folder"
                        + bad_request
3. !fs::exists(filePath) → not_found
4. 扩展名查 mime_types 表 → 查不到则 bad_request
5. 写出，Content-Type = 查表结果
```

`/api/browse` 同样有 `is_browsable_executable`（[2129](../../../src/confighttp.cpp:2129)）做白名单校验，避免 WebUI 变成一个任意文件读取器。

### 4.4 服务器启动

`confighttp::start()`（[../../../src/confighttp.cpp:2313](../../../src/confighttp.cpp:2313)）：

* 线程名 `confighttp`，accept 线程名 `confighttp::tcp`。
* `https_server_t server {config::nvhttp.cert, config::nvhttp.pkey}` —— 复用 GameStream 的同一份 TLS 凭据。
* 端口 `net::map_port(confighttp::PORT_HTTPS)`（基准 +1，默认 **47990**）。
* `server.config.reuse_address = true`；绑定 `net::get_bind_address(address_family)`。
* 启动成功：`Configuration UI available at [https://<addr>:<port>]`。
* 绑定失败：若 `shutdown_event->peek()` 为真则静默返回（正在关闭）；否则 fatal
  `Couldn't start Configuration HTTPS server on port [<port>]: …` 并 `shutdown_event->raise(true)` —— **WebUI 端口占用会导致整个进程退出**。
* `shutdown_event->view()` 阻塞，随后 `server.stop()` + `tcp.join()`。

---

## §5 关键 API 详解

### 5.1 `/api/config`

**GET**（[../../../src/confighttp.cpp:1416](../../../src/confighttp.cpp:1416)）：重新读盘，返回**文件里的原始键值**，不做类型转换。

```json
{
  "status": true,
  "platform": "windows",
  "version": "0.x.y",
  "port": "47989",
  "fec_percentage": "20"
}
```

注意所有值都是**字符串**——因为返回的就是 `parse_config` 的 `unordered_map<string,string>`。这意味着前端拿到的是「写进文件的字面量」，而不是「运行时的有效值」。

**POST**（[../../../src/confighttp.cpp:1470](../../../src/confighttp.cpp:1470)）：这是唯一会**整文件重写** `sunshine.conf` 的端点。

```
saveConfig()
  ├─ check_content_type → authenticate → validate_csrf_token
  ├─ 解析请求体 JSON
  ├─ for (k, v) in input_tree:
  │     if (v.is_null())                        → skip
  │     if (v.is_string() && v.get<string>().empty()) → skip   ← 空字符串 = 删除该键
  │     config_stream << k << " = "
  │                   << (v.is_string() ? v.get<string>() : v.dump()) << endl
  ├─ file_handler::write_file(sunshine.config_file, config_stream.str())
  ├─ 成功 → {"status": true}
  └─ 异常 → 日志 "SaveConfig: <e>" + bad_request(e.what())
```

三条重要推论：

1. **用户注释与手工格式全部丢失**——输出是规范化过的 `key = value` 列表。这是与 §1.5 的「选择性追加」的根本区别。
2. **空值表示删除**：前端把某键设为空字符串，就等价于从文件里去掉它，下次启动回到默认值。
3. **写入后不立即生效**：`saveConfig` 只写文件，不重新 `apply_config`。需要重启（或点 `/api/restart`）。文档注释里的 `@attention{It is recommended to ONLY save the config settings that differ from the default behavior.}` 就是为此。

### 5.2 `/api/password`

（[../../../src/confighttp.cpp:1677](../../../src/confighttp.cpp:1677)）请求体：

```json
{
  "currentUsername": "admin",
  "currentPassword": "…",
  "newUsername": "admin2",
  "newPassword": "…",
  "confirmNewPassword": "…"
}
```

处理逻辑：

```
① 若 newUsername 为空 → 沿用 currentUsername
② 若仍为空 → 错误 "Invalid Username"
③ 否则计算 hash = hex(hash(currentPassword + sunshine.salt))
   若 sunshine.username 为空（首次设置）
      或 (iequals(currentUsername, sunshine.username) && hash == sunshine.password)
      → 通过
      → 若 newPassword 为空 或 newPassword != confirmNewPassword
            → 错误 "Password Mismatch"
      → 否则 save_user_creds + reload_user_creds，{"status":true}
   否则 → 错误 "Invalid Current Credentials"
④ 有错误 → 拼接所有错误文本（", " 连接）→ bad_request
```

一个刻意的设计：`savePassword` 的鉴权是**条件式**的——

```cpp
if (!config::sunshine.username.empty() && !authenticate(response, request)) {
  return;
}
```

即 `username` 为空（首次设置，用户从 `/welcome` 进来）时**跳过 Basic Auth**，只需通过 CSRF 检查。这就是新装 Sunshine 只输一次密码就能建账号的原因。

### 5.3 `/api/pin` 与配对会话

这是 WebUI 与 GameStream 配对握手的**唯一交汇点**。

#### 服务端状态（`nvhttp.cpp`）

```cpp
std::unordered_map<std::string, pair_session_t> map_id_sess;  // 键为客户端 uniqueID
std::mutex &map_id_sess_mutex();
```

| 常量/校验 | 值 |
|-----------|-----|
| `PAIRING_ID_SIZE` | 32 个**十六进制**字符 |
| PIN 格式 | 恰好 **4 位数字**（`is_valid_pairing_pin`，[../../../src/nvhttp.cpp:625](../../../src/nvhttp.cpp:625)） |
| 客户端名 | 1 – `MAX_PAIRING_CLIENT_NAME_SIZE` 字节（`is_valid_pairing_name`，[631](../../../src/nvhttp.cpp:631)） |
| 未审批会话上限 | `MAX_PENDING_PAIRING_SESSIONS`，超限返回 **503** |
| 会话超时 | `PAIRING_SESSION_TIMEOUT`，超时以 **408 "Pairing session expired"** 回应客户端 |

`insert_pair_session`（[../../../src/nvhttp.cpp](../../../src/nvhttp.cpp:594) 附近）返回三态：

| 结果 | HTTP 状态 | 消息 |
|:----:|:---------:|------|
| `ALREADY_EXISTS` | 409 | `A pairing session with this uniqueid already exists` |
| `FULL` | 503 | `Too many pending pairing sessions` |
| `ADDED` | — | 生成 `pairing_id = crypto::rand_alphabet(PAIRING_ID_SIZE, "0123456789abcdef")` |

#### GET `/api/pin`

`getPendingPairings`（[../../../src/confighttp.cpp:1746](../../../src/confighttp.cpp:1746)）调 `nvhttp::get_pending_pairings()`（[../../../src/nvhttp.cpp:635](../../../src/nvhttp.cpp:635)）：

```
get_pending_pairings()
  ├─ lock; expire_pair_sessions_unlocked(now)     ← 先清理超时会话
  ├─ 只保留 last_phase == PAIR_PHASE::NONE 的会话 ← 尚未开始密码学握手的
  ├─ 按 expires_at 升序排序
  └─ 返回 [{id, name, address}, …]
```

响应：

```json
{"pairings":[{"id":"0123456789abcdef0123456789abcdef","name":"My PC","address":"192.168.1.50"}]}
```

#### POST `/api/pin`

`savePin`（[../../../src/confighttp.cpp:1827](../../../src/confighttp.cpp:1827)）先做三层格式校验，随后调 `nvhttp::pin(pairing_id, pin, name)`（[../../../src/nvhttp.cpp:1107](../../../src/nvhttp.cpp:1107)）并返回 `{"status": <bool>}`。

`nvhttp::pin` 内部：

```
pin(pairing_id, pin, name)
  ├─ 校验 id/pin/name 格式
  ├─ lock; expire; 找 last_phase==NONE 且 id 匹配的会话，找不到 → false
  ├─ getservercert(sess, tree, pin)     ← 用 PIN 派生密钥，把服务器证书响应写回
  ├─ 若未失败 → sess.client.name = name
  ├─ 取出 sess.completion（shared_ptr<pairing_completion_t>）
  └─ 在 completion 上等待，最长 config::stream.ping_timeout
       ├─ 客户端完成握手（clientchallenge → serverchallengeresp → clientpairingsecret）
       │     → complete_pairing(sess, true)
       ├─ PIN 错误 / 协议失败         → false
       ├─ 超时                        → false
       └─ 返回结果
```

即：**`POST /api/pin` 是一个同步阻塞的长请求**，最长挂 `ping_timeout`（默认 10 秒）。这正是 nvhttp.cpp 里 `write_pairing_response`（[536](../../../src/nvhttp.cpp:536)）要检查 `response.has_left()/has_right()` 的原因——客户端（Moonlight）那边的 `GET /pair?phrase=getservercert` 连接一直在等，PIN 一提交就要立刻把 XML 写回去并 `close_connection_after_response = true`。

> 冷知识：若启动时带了 `flags = PIN_STDIN`（`config::sunshine.flags[config::flag::PIN_STDIN]`），`start_pairing_session`（[../../../src/nvhttp.cpp:967](../../../src/nvhttp.cpp:967)）不会保存 response，而是打印 `Please insert pin: ` 并从 `std::cin` 读——这就是无头服务器的配对方式。此时 WebUI 的 `/api/pin` 列表仍会显示该会话。

#### DELETE `/api/pin`

`cancelPairing`（[../../../src/confighttp.cpp:1776](../../../src/confighttp.cpp:1776)）→ `nvhttp::cancel_pairing`（[../../../src/nvhttp.cpp:667](../../../src/nvhttp.cpp:667)）：

```
cancel_pairing(pairing_id)
  ├─ 格式校验失败 → false
  ├─ lock; expire
  ├─ 找 last_phase==NONE 且 id 匹配的会话 → 找不到 false
  ├─ 写回 XML：root.paired=0, status_code=400, "Pairing request cancelled by operator"
  ├─ complete_pairing(sess, false)
  └─ map_id_sess.erase(it) → true
```

注意超时清理 `expire_pair_sessions_unlocked`（[577](../../../src/nvhttp.cpp:577)）写回的是 **408 "Pairing session expired"**，与手动取消的 400 区分开。

### 5.4 `/api/clients/*`

底层是 `nvhttp.cpp` 里的 `client_root`（`client_t`，[../../../src/nvhttp.cpp:181](../../../src/nvhttp.cpp:181)）：

```cpp
struct named_cert_t {
  std::string name;   ///< 客户端友好名
  std::string uuid;   ///< 持久 UUID（解绑/改状态都按它定位）
  std::string cert;   ///< 规范化后的 PEM
  bool enabled = true;///< 是否允许连接
};

struct client_t {
  std::vector<named_cert_t> named_devices;
};
```

| 操作 | 函数 | 行号 |
|------|------|------|
| 列出 | `get_all_clients` | [1262](../../../src/nvhttp.cpp:1262) |
| 增 | `add_authorized_client` | [441](../../../src/nvhttp.cpp:441) |
| 删单个 | `unpair_client` | [1756](../../../src/nvhttp.cpp:1756) |
| 全清 | `erase_all_clients` | [1749](../../../src/nvhttp.cpp:1749) |
| 启/停 | `set_client_enabled` | [1773](../../../src/nvhttp.cpp:1773) |
| 取证书 | `get_cert_by_uuid` | [1789](../../../src/nvhttp.cpp:1789) |

**写入后必然重建信任链**：`rebuild_client_cert_chain()`（[310](../../../src/nvhttp.cpp:310)）清空 `cert_chain` 后，只把 `enabled == true` 的条目 `cert_chain.add(...)` 进去。因此「停用」不是打标记，而是**把它从 TLS 信任链里摘掉**，握手阶段就会被拒。

配套的校验：

```
verify_client_certificate(X509*)
  ├─ cert_chain.verify(cert) 失败 → 返回错误字符串
  └─ is_client_enabled(cert)?
       ├─ X509_cmp 精确比对（不是哈希/指纹近似匹配）
       ├─ 出现多于一次匹配 → 返回 false（拒绝歧义）
       ├─ 唯一匹配但 enabled == false → false
       └─ 唯一匹配且 enabled → true
```

`load_state`/`save_state`（[366](../../../src/nvhttp.cpp:366) / [257](../../../src/nvhttp.cpp:257)）负责与 `sunshine_state.json` 互转，并保留了**旧格式迁移**路径（`root.devices[].certs[]` → `named_cert_t`，uuid 现场生成）。

`unpairAll` 与 `unpair` 在 [../../../src/confighttp.cpp](../../../src/confighttp.cpp) 中的签名是 `(resp_https_t &, req_https_t &)`（非 const 引用，本 fork 为便于测试注入而调整）。

### 5.5 `/api/apps` 与 `/api/browse`

应用清单不是存在 `sunshine.conf` 里，而是 `config::stream.file_apps` 指向的 JSON。`getApps`/`saveApp`/`deleteApp`/`closeApp` 围绕这份清单增删改。

* `getApps`（[1025](../../../src/confighttp.cpp:1025)）内部用 `check_app_index`（[858](../../../src/confighttp.cpp:858)）校验下标。
* `deleteApp` 的路由带捕获组 `([0-9]+)`，下标非法时不会进入处理函数而是落到默认处理器（400/404）。
* `closeApp` 走 `proc::` 流程关闭当前应用。
* `/api/browse`（[2234](../../../src/confighttp.cpp:2234)）用于在 WebUI 里选可执行文件，靠 `is_browsable_executable`（[2129](../../../src/confighttp.cpp:2129)）过滤。

### 5.6 `/api/covers/upload`

`uploadCover`（[1589](../../../src/confighttp.cpp:1589)）配合 `getCover`（[1519](../../../src/confighttp.cpp:1519)）实现封面图管理：

* `GET /api/covers/<index>` 按应用下标取图。
* `POST /api/covers/upload` 上传，落盘路径由应用标识推导；请求体是 multipart 还是原始字节由前端约定。

### 5.7 `/api/restart` 与两个 `reset-*`

| 端点 | 函数 | 行为 |
|------|------|------|
| `POST /api/restart` | `restart`（[1939](../../../src/confighttp.cpp:1939)） | 直接 `platf::restart()`。注释明确写着 *"We may not return from this call"* —— 预期是重启进程，不要依赖它返回响应 |
| `POST /api/reset-display-device-persistence` | [1878](../../../src/confighttp.cpp:1878) | `display_device::reset_persistence()`，清掉 `display_device.state` |
| `POST /api/reset-portal-token` | [1904](../../../src/confighttp.cpp:1904) | 删除 XDG Portal restore token；无 Portal 采集的平台上也会成功（不动文件系统） |

### 5.8 `/api/virtual-input/*`（Windows）

三个端点：`status`（[2032](../../../src/confighttp.cpp:2032)）、`license` GET/POST（[2053](../../../src/confighttp.cpp:2053) / [2069](../../../src/confighttp.cpp:2069)）。

驱动版本判定逻辑：

| 常量 | 值 | 行号 |
|------|-----|------|
| `libvirtualhid_minimum_version` | `LIBVIRTUALHID_MINIMUM_VERSION` | [211](../../../src/confighttp.cpp:211) |
| `VIGEMBUS_MINIMUM_VERSION` | `"1.17.0.0"` | [212](../../../src/confighttp.cpp:212) |

| 函数 | 行号 | 作用 |
|------|------|------|
| `parse_driver_version_part` | [217](../../../src/confighttp.cpp:217) | 解析版本号中的一个分量 |
| `parse_driver_version` | [240](../../../src/confighttp.cpp:240) | 解析四段版本 |
| `is_driver_version_development` | [267](../../../src/confighttp.cpp:267) | 是否开发版本 |
| `is_driver_version_supported` | [272](../../../src/confighttp.cpp:272) | 是否 ≥ 最低版本 |
| `read_libvirtualhid_driver_version` | [464](../../../src/confighttp.cpp:464) | 从系统读取已安装驱动版本 |

`get_virtualhid_driver_status`（[../../../src/confighttp.cpp:1960](../../../src/confighttp.cpp:1960) 附近）在 Windows 上还会尝试 `platf::virtualhid::create_runtime()`，用 `capabilities()` 补出：

| 字段 | 含义 |
|------|------|
| `backend_name` | 实际后端名 |
| `requires_installed_driver` | 是否必须有已安装驱动 |
| `error` | 运行时异常信息（如 `std::bad_alloc`） |

非 Windows 平台统一返回 `error = "libvirtualhid driver status is only available on Windows"`、`backend_name = ""`、`requires_installed_driver = false`。

测试注入点：`set_virtual_input_license_status_provider_for_testing`（[166](../../../src/confighttp.cpp:166)）、`reset_*`（[170](../../../src/confighttp.cpp:170) / [178](../../../src/confighttp.cpp:178)）、`clear_sensitive_string_for_testing`（[184](../../../src/confighttp.cpp:184)）。

---

## §6 其它持久化状态

### 6.1 `sunshine_state.json`（一个文件，三种角色）

```
sunshine_state.json
  ├─ root.uniqueid            ← 主机唯一标识，HTTP 里以 X-SS-… / serverinfo 的 <uniqueid> 出现
  ├─ root.named_devices[]     ← 已配对客户端（name / cert / uuid / enabled）
  ├─ username                 ← WebUI 用户名
  ├─ salt                     ← WebUI 密码盐
  └─ password                 ← hex(hash(password + salt))
```

`unique_id` 的唯一生成点有两处：

| 场景 | 位置 |
|------|------|
| 文件不存在 / 无 `root.uniqueid` | `load_state`（[../../../src/nvhttp.cpp:366](../../../src/nvhttp.cpp:366)），日志 `File <path> doesn't exist` |
| `FRESH_STATE` | `http::init`（[../../../src/httpcommon.cpp:55](../../../src/httpcommon.cpp:55)） |

**丢失这个文件 = 所有配对客户端都要重新配对。** 备份时它比 `sunshine.conf` 更重要。

### 6.2 `display_device.state`

`main()` 里最早建立的 guard 之一：

```cpp
display_device::init(appdata() / "display_device.state", config::video);
```

它记录「串流前的显示配置」以便会话结束恢复。相关的配置项（`video_t::dd_t`）：

| 键 | 作用 |
|----|------|
| `dd.configuration_option` | `disabled`/`verify_only`/`ensure_active`/`ensure_primary`/`ensure_only_display` |
| `dd.resolution_option` | `disabled`/`automatic`/`manual` |
| `dd.refresh_rate_option` | 同上 |
| `dd.hdr_option` | `disabled`/`automatic` |
| `dd.config_revert_delay` | 会话结束后多久恢复 |
| `dd.config_revert_on_disconnect` | 断连时是否立即恢复（`session::join()` 里判断） |
| `dd.mode_remapping` | 分辨率/刷新率重映射表 |

`POST /api/reset-display-device-persistence`（§5.7）就是丢这个文件的持久化层——当恢复行为卡在错误状态时，这是救援入口。

### 6.3 nvprefs（Windows）

Windows 上 `main()` 会创建 `nvprefs_instance`：

```
main()
  ├─ nvprefs_instance 恢复上次的 undo 记录
  ├─ 修改 NVIDIA profile（允许在不接显示器的情况下推流）
  ├─ unload()
  ...（运行期）...
  └─ 退出前再次恢复
```

子命令 `restore-nvprefs-undo` 用于手动回滚这套改动。

### 6.4 UPnP 与 mDNS

| 机制 | 入口 | 触发条件 |
|------|------|---------|
| mDNS 广播 | `platf::publish::start()`（`main()` 内异步启动） | 始终 |
| UPnP 端口映射 | `upnp::start()`（`main()` 内异步） | `flags` 含 `flag::UPNP`（值 3） |

两者都在 `http::init()` 成功之后、三个 HTTP/RTSP 线程启动之前**异步**拉起，失败不影响主服务。

---

## §7 小结

1. **配置是「字符串 map + 消费式解析」**：`parse_config` 只做 `key = value` 提取，语义全在 `apply_config` 的 `*_f` 族里；没被消费的键会以 `Warning: Unrecognized configurable option` 暴露出来。
2. **优先级**：命令行 `key=value` > `sunshine.conf` > 编译期默认值。默认值需要落盘时用 `persist_config_option_if_missing`（保留注释），而 WebUI 的 `/api/config` POST 会**整文件重写**（注释丢失），且写完不立即生效。
3. **凭据分两类**：TLS 自签证书（CN `"Sunshine Gamestream Host"`、RSA 2048、pkey 0600）由 `crypto::gen_creds` 生成；WebUI 密码以 `hex(hash(password + 16 字节随机盐))` 存进 `sunshine_state.json`，从不存明文。
4. **WebUI 三道闸**：`origin_web_ui_allowed`（地址类型准入）→ HTTP Basic Auth（用户名 `iequals`，密码哈希比对）→ CSRF（同源放行 / 无 Origin 放行 / 否则需 `X-CSRF-Token` 或 `csrf_token`）。POST 端点额外先过 `check_content_type("application/json")`。
5. **`/api/pin` 是 WebUI 与 GameStream 配对的唯一交汇点**，且它是一个最多阻塞 `ping_timeout` 的长请求；`FRESH_STATE` 与 `PIN_STDIN` 是两个改变持久化/交互方式的重要开关。
6. **`sunshine_state.json` 是单一故障点**：它同时承载主机 `uniqueid`、配对证书库和 WebUI 凭据；丢失它意味着所有客户端都要重新配对。

---

**下一篇**：[08 · 附录：协议常量与结构速查](./08-附录-协议常量与结构速查.md)
