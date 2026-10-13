# 11 · 构建与打包：Windows 源码编译与 MSI 交付产物

> **定位**：前十篇讲"系统怎么工作、坏了怎么办"，本篇讲**"怎么把它变成可交付的安装包"**。
> 全文只回答四件事：
> ① **环境怎么摆** —— 编译器在哪、node 在哪、WiX 会自动装到哪；
> ② **版本号是怎么来的** —— 为什么产物叫 `0.0.0-1ffccfe5-dirty`、为什么 MSI 的 `ProductVersion` 会退化成 `0.0.0`；
> ③ **五步操作手册** —— 配置 → 编译 → Web UI → 打 MSI → 验证，每条命令都可直接复制；
> ④ **产物怎么验** —— 不靠"文件是新的"，而是靠**解包比对 SHA256**。

> **与前序文档的分工**
>
> | 你想知道 | 去哪一篇 |
> |----------|----------|
> | 编译出来的 `sunshine.exe` 启动后做了什么 | [01 §2 `main()` 启动时序](./01-总览与启动流程.md#2-main-启动时序逐步拆解) |
> | 打出来的包里**应该**有哪些资产、分别被谁读 | [01 §1 仓库结构](./01-总览与启动流程.md#1-仓库结构)、[07 §4 WebUI 路由表](./07-配置-鉴权与WebUI.md#4-路由表) |
> | 编译选项（`BUILD_TESTS` / `BUILD_DOCS` / `NPM` 等）改了会怎样 | 本篇 §1.4 |
> | 安装后起不来 / 装不上 / 回滚到旧版本 | [10 §9 致命错误回滚手册](./10-疑难解答-日志排障与回滚.md#9-致命错误回滚手册) |
> | 上游 CI 怎么做多平台打包 | 仓库 [`.github/workflows/`](../../../.github/workflows/)、[cmake/packaging/](../../../cmake/packaging/) |
>
> **实测基准**：本篇所有命令与数字均为本 fork（`Gaolingx/Sunshine`）在
> **Windows 11 + 64 逻辑核 + 63.7 GB RAM + msys2/ucrt64** 上，对 `1ffccfe5` 提交的一次完整打包实测结果。
> 目录约定沿用本系列：文中 `src/xxx.cpp:123` 指向源码；`../../../` 指向仓库根。

---

## 1. 环境与前置条件

### 1.1 工具链清单

| 角色 | 本机实测路径 / 版本 | 谁需要它 | 缺失的后果 |
|------|--------------------|----------|-----------|
| **msys2（UCRT64 子系统）** | `E:\msys64\msys2_shell.cmd` | 所有命令的入口 | 无法编译 |
| **CMake** | `E:\msys64\ucrt64\bin\cmake.exe`（msys2 包 `mingw-w64-ucrt-x86_64-cmake`） | 配置 + 编译 + CPack | 无法配置 |
| **Ninja** | `E:\msys64\ucrt64\bin\ninja.exe` | 生成器（`CMAKE_GENERATOR=Ninja`） | 无法生成构建图 |
| **GCC (g++ / gcc)** | `E:\msys64\ucrt64\bin\g++.exe` | C++ 编译 | 无法编译 |
| **Node.js + npm** | `E:\node\node.exe` (v24.21.0)、`E:\node\npm.cmd` | Vite 打 Web UI | `find_program(NPM npm REQUIRED)` 直接 FATAL |
| **.NET SDK** | `C:\Program Files\dotnet\dotnet.exe` | `dotnet tool install wix` | ⚠️ **不是报错而是静默跳过**：`windows_wix.cmake` 只 `message(WARNING ...)` 就 `return()`，于是 `CPACK_WIX_*` 全部未定义 → **MSI 打不出来，且没有任何红色报错** |
| **WiX 4.0.4** | `cmake-build-msys2\.wix\wix.exe` | MSI 生成 | —— **无需手动装**，§3 步骤 1 会自动拉取 |
| **NSIS (`makensis`)** | msys2 包 `mingw-w64-ucrt-x86_64-nsis` | 可选的 NSIS 安装包 | 只有 §3 步骤 5 会失败，不影响 MSI |

★ **只需要记住一件事**：**所有命令都必须在 `msys2_shell.cmd` 里跑，不能在 PowerShell / cmd 里直接跑**。
本机固定前缀（下文一律写作 `<MSYS>`）：

```
E:\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c "<命令>"
```

⚠️ **注意 `-ucrt64` 与 `E:\msys64`**：本机 msys2 装在 **E 盘**（不是 `C:\msys64`）。装错位置或漏掉 `-ucrt64`，
会拿到 MSYS（POSIX）工具链，编译出来的东西不是 Windows 原生，链接阶段必然失败。

### 1.2 目录约定

| 概念 | 本机取值 | 说明 |
|------|----------|------|
| 源码根 | `E:\GitHub\fork\Sunshine` | msys2 里写作 `/e/GitHub/fork/Sunshine` |
| 构建目录 | `cmake-build-msys2` | 项目约定：**构建目录一律以 `cmake-build-` 开头**（便于 `.gitignore` 与人工区分） |
| 产物目录 | `cmake-build-msys2\cpack_artifacts` | 由 [packaging/common.cmake:10](../../../cmake/packaging/common.cmake#L10) 的 `CPACK_PACKAGE_DIRECTORY` 决定 |
| WiX 暂存 | `cmake-build-msys2\cpack_artifacts\_CPack_Packages\win64\WIX\` | `Sunshine\` 是安装树镜像，`wix.log` 是 `wix.exe` 原始日志 |
| WiX 工具 | `cmake-build-msys2\.wix\` | `wix.exe` + `extensions\`（UI / Util 两个扩展） |

⚠️ **`cpack_artifacts` 里的东西不是"构建产物"而是"交付产物"**，别随手删。
`_CPack_Packages\` 是 CPack 的中间层，删掉只会导致下一次打包变慢，不会出错。

### 1.3 一图看懂：五步流水线

```
  [0] 工作树干净?  ──►  git status  ──►  决定版本号里有没有 "-dirty"
        │
  [1] 配置  cmake -S <src> -B cmake-build-msys2
        │        ├─ 拉取/安装 WiX 4.0.4 + UI/Util 扩展   ← 必须成功，否则打不出 MSI
        │        ├─ 解析版本号  → PROJECT_VERSION         ← §2
        │        ├─ 生成 build.ninja / CPackConfig.cmake
        │        └─ 把 wix_resources\ 复制到 wix_packaging\（含自定义 wix.template.in）
        │
  [2] 编译  cmake --build cmake-build-msys2 --target sunshine sunshinesvc dxgi-info audio-info
        │        └─ sunshine.exe / sunshinesvc.exe / dxgi-info.exe / audio-info.exe
        │
  [3] Web UI  cmake --build ... --target web-ui
        │        ├─ npm ci --ignore-scripts      ← 需要网络（NPM_OFFLINE=OFF）
        │        └─ npm run build (vite)         ← 输出到 cmake-build-msys2\assets\web
        │
  [4] 打包  cpack -G WIX            ──► cpack_artifacts\Sunshine.msi   ← 交付产物
        │    cpack                   ──► cpack_artifacts\Sunshine.exe   （NSIS，可选）
        │
  [5] 验证  msiexec /a + SHA256 比对  ──►  确认"新二进制真的进了包"
```

### 1.4 关键构建选项

以下值来自本机 `cmake-build-msys2\CMakeCache.txt`；
所有 `BUILD_*` / `NPM*` 开关的定义都在 [cmake/options.cmake](../../../cmake/options.cmake)，
配置时用 `-D<名字>=<值>` 覆盖即可。

| 变量 | 本机值 | 影响 |
|------|--------|------|
| `CMAKE_BUILD_TYPE` | `Release` | 交付包必须是 Release |
| `CMAKE_GENERATOR` | `Ninja` | 所有 `cmake --build` / `ninja` 命令的前提 |
| `CMAKE_INSTALL_PREFIX` | `C:/Program Files (x86)/Sunshine` | ⚠️ **只影响 CPack 暂存树的目录布局，不影响 MSI 落到哪**（MSI 永远落在 `cpack_artifacts`） |
| `BUILD_TESTS` | `ON` | 多出 `tests/test_sunshine.exe`；打包**不需要**它，可用 `--target` 定向绕开 |
| `BUILD_DOCS` | `ON` | 多出 `docs` 目标（doxygen + 它自己的 `npm ci`）；打包**不需要**它 |
| `NPM` | `E:/node/npm` | 必须是**原生 Windows** npm，见 §6 故障 4 |
| `NPM_OFFLINE` | `OFF` | 为 `OFF` 时 `npm ci` 需要联网（[targets/common.cmake:85](../../../cmake/targets/common.cmake#L85)） |
| `CPACK_BINARY_WIX` | **`OFF`** | ⚠️ 见下方"陷阱" |
| `CPACK_BINARY_NSIS` | `ON` | 所以 `ninja package` / 裸 `cpack` 出的是 **NSIS 安装包，不是 MSI** |

⚠️ **最大的陷阱**：`CPACK_BINARY_WIX=OFF` 意味着
**`ninja package`（= `cpack --config ./CPackConfig.cmake`）不会生成 MSI**，
它只会生成 NSIS 的 `Sunshine.exe`。
`CPACK_BINARY_WIX` 仅控制 CMake 生成哪些"打包目标"，**不控制 `cpack -G WIX` 命令行**——
所以打 MSI 必须**显式写 `-G WIX`**（见 §3 步骤 4）。

---

## 2. 版本号是怎么来的（先搞懂，否则产物"版本不对"）

### 2.1 两条路径：CI 覆盖 vs 本地 git 推导

全部逻辑在 [cmake/prep/build_version.cmake](../../../cmake/prep/build_version.cmake)。
`CMakeLists.txt:8` 里写死的 `project(Sunshine VERSION 0.0.0 ...)` **只是占位**，真实版本在这里被改写：

```
                        ┌─ 环境变量 BRANCH 已定义 且 BUILD_VERSION 非空 ?
                        │
              ┌─── 是 ──┴──►  PROJECT_VERSION = $ENV{BUILD_VERSION}（去掉开头的 "v"）
              │              ← CI 路径。本项目 GitHub Actions 走这条，
              │                版本形如 2026.1013.0 / 0.0.0-rc1
  版本推导 ───┤
              │              ① PROJECT_VERSION = 0.0.0
              └─── 否 ──────► ② 当前分支 != master ? → PROJECT_VERSION = 0.0.0-<short HEAD>
                             ③ git diff --quiet --exit-code 失败 ? → 追加 "-dirty"
                             ← 本地路径。本机走这条
```

★ **本机实测**：当前分支 `feature-add_nvenc_present`（≠ `master`）、HEAD `1ffccfe5`，再叠加 `-dirty`，得到：

```
0.0.0-1ffccfe5-dirty
```

⚠️ **`-dirty` 的触发条件最容易搞错**。脚本用的是 `git diff --quiet --exit-code`，
它**只看"已跟踪文件、未暂存"的改动**。实测四种情形的结果：

| 工作树状态 | `git diff --quiet --exit-code` | 会不会带 `-dirty` |
|------------|-------------------------------|:-----------------:|
| 完全干净 | `0` | 不会 |
| 只有未跟踪文件/目录（`?? third-party/dockle/`） | `0` | **不会** |
| 改动已 `git add` 进暂存区、还没 commit | `0` | **不会** |
| 任何 tracked 文件被改过且未 `git add` | `1` | **会** |

> 注意 `git status --short` 里的 `? third-party/libdisplaydevice`（`?` 在第 2 列）表示
> "子模块里有未跟踪文件"。它**不会**让版本号变 `-dirty`，是纯噪音。

所以看到 `-dirty` 时，用这两条定位元凶（分别对应"未暂存"和"已暂存"）：

```powershell
git diff --stat          # 已跟踪、未暂存的改动 ← 就是它触发的
git diff --cached --stat # 已暂存、未提交的改动
```

想要不带 `-dirty` 的版本号：把它们**提交掉或 stash 掉，再重新配置**。

### 2.2 版本号是怎么进到二进制里的

| 落点 | 机制 | 怎么看到 |
|------|------|----------|
| **编译宏** | [build_version.cmake:148](../../../cmake/prep/build_version.cmake#L148) 把 `PROJECT_VERSION="..."` 追加进 `SUNSHINE_DEFINITIONS`，再由 [targets/common.cmake:27](../../../cmake/targets/common.cmake#L27) `target_compile_definitions(sunshine PUBLIC ...)` 下发，于是出现在**每一个** `.obj` 的 `DEFINES` 里 | `grep 'PROJECT_VERSION=' cmake-build-msys2/build.ninja` |
| **启动日志** | [src/main.cpp:265](../../../src/main.cpp#L265) 打印版本行 | 见 [10 §2.1 健康启动骨架](./10-疑难解答-日志排障与回滚.md#21-先认这些-info-路标) |
| **WebUI** | [src/confighttp.cpp:1426](../../../src/confighttp.cpp#L1426) `/api/state` 的 `output_tree["version"]` | 浏览器打开 WebUI |
| **Windows 资源** | [src/platform/windows/windows.rc:11](../../../src/platform/windows/windows.rc#L11) 的 `FILEVERSION`/`ProductVersion` | 文件属性 → 详细信息 |
| **MSI 包** | `CPACK_PACKAGE_VERSION = ${PROJECT_VERSION}` → WiX `<Package Version=` | `wix.log` / MSI 属性表 |

⚠️ **反直觉但重要**：因为 `PROJECT_VERSION` 是**全局编译宏**，"改了版本号"会导致
**几乎全量重编**（Boost / libdisplaydevice / libvirtualhid 的 `.obj` 的 `DEFINES` 也变了）。
换版本号 = 准备好吃一次长编译。

### 2.3 为什么 MSI 的 `ProductVersion` 会退化成 `0.0.0`（WIX1148）

自定义模板 [wix_resources/wix.template.in:11](../../../cmake/packaging/wix_resources/wix.template.in#L11)
把 `Version="$(var.CPACK_PACKAGE_VERSION)"` 原样交给 WiX。而 MSI 规范要求
`major.minor.build` 三段全是**纯十进制数字**，`revision` 段被忽略，**且禁止出现字母标签**。
`0.0.0-1ffccfe5-dirty` 不满足，于是：

```
warning WIX1148: Invalid MSI package version: '0.0.0-1ffccfe5-dirty'.
    ... The revision value is ignored but version labels and metadata are not allowed.
```

结果是**有效 MSI 版本恒为 `0.0.0`**。这个警告从项目开始打本地包起就一直存在，**不是本次引入的回归**。

★ **那为什么升降级还正常？** 因为不用版本号判新旧：
[wix.template.in:26](../../../cmake/packaging/wix_resources/wix.template.in#L26) 声明了 `AllowDowngrades="yes"`，
[windows_wix.cmake:60](../../../cmake/packaging/windows_wix.cmake#L60) 定义了升级匹配用的 `UpgradeCode`，
[windows_wix.cmake:70](../../../cmake/packaging/windows_wix.cmake#L70) 又把 `REINSTALLMODE` 设成 `amus`，
并且 `UpgradeCode` 是**写死的常量**
`512A3D1B-BE16-401B-A0D1-59BBA3942FB8`。
所以无论装的是 release 还是 PR 包，直接双击新 MSI 覆盖安装即可。

### 2.4 想指定一个"好看"的版本号

在**配置之前**设置两个环境变量即可走 CI 路径（§2.1 左侧分支）：

```
export BRANCH=master
export BUILD_VERSION=2026.1013.1      # 开头带 v 会被自动剥掉；必须非空
```

⚠️ 代价：**版本号变了 → 全量重编**（§2.2）。所以别为了"版本号好看"反复 configure。
另外 `BUILD_VERSION` 只有形如 `YYYY.MMDD.N` 时才会顺带修好
`PROJECT_YEAR/MONTH/DAY` 与 `RC_VERSION_BUILD/REVISION`（见
[build_version.cmake](../../../cmake/prep/build_version.cmake) 后半段），否则它们留在 `1990/01/01`。

---

## 3. 操作手册

> 以下每条命令都假设：**源码根 = `E:\GitHub\fork\Sunshine`**，**构建目录 = `cmake-build-msys2`**，
> 且运行在 PowerShell 中、通过 `<MSYS>` 前缀进入 msys2。

### 步骤 0 · 先看工作树（决定版本号，也决定该不该重编）

```powershell
Set-Location E:\GitHub\fork\Sunshine
git status --short
git log --oneline -1
```

- 输出里若只有 `? third-party/...` / `?? third-party/...` 之类的**子模块脏**，那是历史遗留，**既不影响交付、也不会让版本号变 `-dirty`**（原因见 §2.1）。
- ⚠️ **真正会让版本号带 `-dirty` 的是"已跟踪文件有未暂存改动"**。所以这里要额外跑一条 `git diff --stat`：
  有输出就说明"上次打的包"和"现在的源码"不是一回事，必须走完 §3 全流程（或先提交/stash）。

### 步骤 1 · 配置

```powershell
& E:\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c "cmake -S /e/GitHub/fork/Sunshine -B /e/GitHub/fork/Sunshine/cmake-build-msys2 2>&1 | tail -40"
```

**这一步在做什么**（都在配置期完成，不是编译期）：

1. 解析版本号 → `PROJECT_VERSION`（§2.1）；
2. `dotnet tool install --tool-path cmake-build-msys2/.wix wix --version 4.0.4`
   —— ⚠️ 若 `.wix` 已存在会打印 `Cannot create a file when that file already exists.` 与
   `已安装工具"wix"`，**这是正常的**（`dotnet tool install` 的幂等提示），不是失败；
3. `wix extension add WixToolset.UI.wixext/4.0.4` + `WixToolset.Util.wixext/4.0.4`；
4. 复制 `cmake/packaging/wix_resources/` → `cmake-build-msys2/wix_packaging/`
   （含自定义 `wix.template.in`、`patch.xml`、`sunshine-installer.wxs`）；
5. 写入 `CPackConfig.cmake`（`CPACK_WIX_ROOT` / `_EXTENSIONS` / `_TEMPLATE` / `_UPGRADE_GUID` 全在这里）；
6. `include(CPack)` 生成 `package` / `package_source` 目标。

**实测耗时**：配置 88.5 s + 生成 0.9 s（首次拉 WiX 会更久）。

**自检**（三条都该有输出，缺任何一条都说明配置没走完）：

```powershell
Set-Location E:\GitHub\fork\Sunshine
Select-String cmake-build-msys2\CPackConfig.cmake -Pattern 'CPACK_PACKAGE_VERSION|CPACK_WIX_ROOT|CPACK_WIX_TEMPLATE'
Select-String cmake-build-msys2\build.ninja -Pattern 'PROJECT_VERSION=' | Select-Object -First 1
Get-ChildItem cmake-build-msys2\.wix | Select-Object Name
```

期望看到：版本号形如 `0.0.0-<hash>[-dirty]`、`CPACK_WIX_ROOT` 指向 `...\cmake-build-msys2/.wix`、
`.wix` 目录下有 `wix.exe` 与 `extensions\`。

### 步骤 2 · 编译 C++ 目标

```powershell
& E:\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c "cd /e/GitHub/fork/Sunshine/cmake-build-msys2 && cmake --build . --target sunshine sunshinesvc dxgi-info audio-info -j 16 > /tmp/build.log 2>&1; echo EXIT=`$?; tail -5 /tmp/build.log"
```

**为什么只点这四个 target**：

| target | 产物 | 装到哪（组件） |
|--------|------|---------------|
| `sunshine` | `sunshine.exe` | 安装根 `.`（`application`） |
| `sunshinesvc` | `tools/sunshinesvc.exe` | `tools/`（`application`，**mandatory**） |
| `dxgi-info` | `tools/dxgi-info.exe` | `tools/`（`dxgi`） |
| `audio-info` | `tools/audio-info.exe` | `tools/`（`audio`） |

（规则见 [cmake/packaging/windows.cmake](../../../cmake/packaging/windows.cmake)。）
不点 `tests` / `docs` 可以省掉 gtest、doxygen 与它们自带的 `npm ci`——**打包不需要这两者**。

⚠️ **必读：`-j` 必须手动限制，否则会 OOM**

本机 **64 逻辑核 / 63.7 GB RAM**。Boost 相关的翻译单元单个就能吃到 1–2 GB，64 个并发会打爆内存，实测现象是：

```
cc1plus.exe: out of memory allocating 65536 bytes
FAILED: [code=1] CMakeFiles/sunshine.dir/src/cbs.cpp.obj
...
ninja: build stopped: subcommand failed.
```

★ **经验公式**：`-j` ≈ `RAM_GB / 4`（本机即 `-j 16`）。**并且绝对不要把步骤 3 的 Vite 构建与步骤 2 并发跑**——
Rolldown 是内存大户，实测第一次失败正是"编译与 `npm run build` 抢内存"造成的。两者请**串行**。

⚠️ **不要用 `... | tail -60` 判断成功与否**。管道会让 `$?` 反映 `tail` 而不是 `cmake`，
失败也会显示成功。**先把日志写进文件，再显式打印 `$?`**（如上面的命令）。

⚠️ **退出码要写 `` `$? ``，不能写 `\$?`**。
在 PowerShell 双引号字符串里，反引号才是转义符：写 `` `$? `` 会把字面量 `$?` 交给 bash（bash 自己展开，
能拿到真退出码）；写 `\$?` 则会被 PowerShell 抢先展开成 `True`/`False`，永远看不出失败。

**成功判据**：日志末尾出现 `Linking CXX executable sunshine.exe`，且 `echo EXIT=0`。

### 步骤 3 · 构建 Web UI

```powershell
& E:\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c "cd /e/GitHub/fork/Sunshine/cmake-build-msys2 && cmake --build . --target web-ui > /tmp/webui.log 2>&1; echo EXIT=`$?; tail -8 /tmp/webui.log"
```

**什么时候必须做这一步**：只要动过
`src_assets/common/assets/web/**`（HTML / Vue / JS / 语言包 JSON）或根 `package.json`，
就必须重建——MSI 装的是 **Vite 的产物** `cmake-build-msys2/assets/web`，**不是源文件**。

**这个 target 实际执行两条命令**（见 [cmake/targets/common.cmake:90](../../../cmake/targets/common.cmake#L90)）：

```
cmd /C E:/node/npm ci  --ignore-scripts            # 每次都会清装 node_modules，需要网络
cmd /C E:/node/npm run build                       # = vite build --debug
```

（`WORKING_DIRECTORY` 是**源码根**，也就是根 `package.json` 所在处；不是 `src_assets`，也不是构建目录。）

环境变量由 target 自己注入，**不要手动设**：
`SUNSHINE_SOURCE_ASSETS_DIR=<源码>/src_assets`、`SUNSHINE_ASSETS_DIR=<构建目录>`、`SUNSHINE_BUILD_HOMEBREW=`（空）。

**实测耗时**：`npm ci` 视网络 ~1–3 min；`vite build` 16.7 s。

**成功判据**：`✓ built in ...`（Vite 会额外打印 `[PLUGIN_TIMINGS]` 提示），
且 `cmake-build-msys2\assets\web\index.html` 的时间戳是刚刚。

> **没有网络时**：`npm ci` 会失败。若 `node_modules` 已经完整，可以绕过 target 直接手动跑构建
> （PowerShell 中设置环境变量后 `E:\node\npm.cmd run build`），或在配置时打开 `-DNPM_OFFLINE=ON`。

### 步骤 4 · 生成 MSI（**本篇的主角**）

```powershell
& E:\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c "cd /e/GitHub/fork/Sunshine/cmake-build-msys2 && cpack -G WIX --config CPackConfig.cmake > /tmp/cpack.log 2>&1; echo EXIT=`$?; tail -20 /tmp/cpack.log"
```

⚠️ **`-G WIX` 不能省**（原因见 §1.4）。
`--config CPackConfig.cmake` 在构建目录里执行时可以省略，但写上更稳。

**内部发生了什么**：CPack 先用 `cmake --install` 把安装规则铺进
`cpack_artifacts/_CPack_Packages/win64/WIX/Sunshine/`（七个组件
`Unspecified/application/assets/audio/autostart/dxgi/firewall`），
再逐文件 `strip`（`CPACK_STRIP_FILES=YES`，`sunshine.exe` 122.8 MB → 72.1 MB），
最后调用 `.wix\wix.exe build` 生成 CAB 与 MSI。

**实测**：约 40 s，产物 `cpack_artifacts\Sunshine.msi` ≈ 32.5 MiB。

**成功判据**：日志末尾出现

```
CPack: - package: E:/GitHub/fork/Sunshine/cmake-build-msys2/cpack_artifacts/Sunshine.msi generated.
```

> ⚠️ 日志里必有 `Warning WIX1148: Invalid MSI package version: ...`
> （原因与无害性见 §2.3）。**看到它不代表失败**。

### 步骤 5 · 生成 NSIS 安装包（可选）

```powershell
& E:\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c "cd /e/GitHub/fork/Sunshine/cmake-build-msys2 && cpack --config CPackConfig.cmake > /tmp/nsis.log 2>&1; echo EXIT=`$?; tail -6 /tmp/nsis.log"
```

裸 `cpack` 走 `CPACK_GENERATOR=NSIS`（也可以写 `ninja package`，效果相同），产出
`cpack_artifacts\Sunshine.exe`（实测 29.9 MB）。

⚠️ **它与 `application\sunshine.exe` 同名但完全不同**：前者在 `cpack_artifacts\` 下、是可执行的**安装器**；
后者在 `cpack_artifacts\_CPack_Packages\...\Sunshine\application\` 下、是被打包的**主程序**。别混淆、别看错。

---

## 4. 验证产物

★ **原则**：不要用"文件时间新"当证据，要用**"包里的二进制和刚编译出来的是同一个"**当证据。

### 4.1 二进制自检（最快）

```powershell
& E:\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c "cd /e/GitHub/fork/Sunshine/cmake-build-msys2 && strings -a sunshine.exe | grep -E '^0\.0\.0-' | head -2"
```

期望：打印**当前 HEAD 短哈希 + 可能的 `-dirty`**（本机为 `0.0.0-1ffccfe5-dirty`）。
若打印的是上一个提交的哈希，说明 §3 步骤 1/2 没真正跑或跑在别的构建目录里。

### 4.2 解包比对（最硬的证据）

用 MSI 的"管理安装"模式把包解到一个临时目录，再逐文件比对 SHA256：

```powershell
$msi    = 'E:\GitHub\fork\Sunshine\cmake-build-msys2\cpack_artifacts\Sunshine.msi'
$staged = 'E:\GitHub\fork\Sunshine\cmake-build-msys2\cpack_artifacts\_CPack_Packages\win64\WIX\Sunshine'
$dest   = Join-Path $env:TEMP 'sunshine-msi-verify'

if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
New-Item -ItemType Directory -Path $dest | Out-Null

# /a = 管理安装（只解包，不装进系统、不写注册表）
Start-Process msiexec.exe -ArgumentList @('/a', "`"$msi`"", '/qn', "TARGETDIR=`"$dest`"") -Wait

$out    = Join-Path $dest 'PFiles64\Sunshine'     # ⚠️ 解出来的根是 PFiles64\Sunshine
$pairs  = @(
    @{ Exe = 'sunshine.exe';            In = 'application\sunshine.exe' },
    @{ Exe = 'tools\sunshinesvc.exe';   In = 'application\tools\sunshinesvc.exe' },
    @{ Exe = 'tools\dxgi-info.exe';     In = 'dxgi\tools\dxgi-info.exe' },
    @{ Exe = 'tools\audio-info.exe';    In = 'audio\tools\audio-info.exe' }
)
foreach ($p in $pairs) {
    $h1 = (Get-FileHash (Join-Path $out    $p.Exe) -Algorithm SHA256).Hash
    $h2 = (Get-FileHash (Join-Path $staged $p.In ) -Algorithm SHA256).Hash
    "{0,-6} {1}" -f ($h1 -eq $h2), $p.Exe
}
```

四个都要是 `True`。★ **只有这一条能证明"MSI 里的确是刚编译的那个二进制"**。

补充检查：

```powershell
# 1) 包内二进制里的版本号（等价于 §4.1 的自检，但扫的是"解包出来的那份"）
#    注意：不要用 findstr 直接扫 exe，二进制没有换行，会把整个文件当成一行打出来
$bytes = [IO.File]::ReadAllBytes("$out\sunshine.exe")
[regex]::Matches([Text.Encoding]::ASCII.GetString($bytes), '0\.0\.0-[0-9a-f]{7,40}(-dirty)?') |
    ForEach-Object { $_.Value } | Sort-Object -Unique      # 期望：0.0.0-<当前HEAD短哈希>[-dirty]

# 2) 包内 Web UI 是否已同步（示例：确认某个已删除的配置键不在产物里）
Get-ChildItem (Join-Path $out 'assets\web') -Recurse -File |
    Select-String -Pattern '<被删掉的键名>' -SimpleMatch     # 期望：无输出

# 3) 清理临时解包目录
Remove-Item -Recurse -Force $dest
```

### 4.3 一页检查清单

| # | 检查 | 期望 |
|:-:|------|------|
| 1 | `cpack_artifacts\Sunshine.msi` 时间戳 | 刚刚 |
| 2 | `cpack_artifacts\Sunshine.msi` 大小 | ~32–33 MiB |
| 3 | `_CPack_Packages\...\Sunshine\application\sunshine.exe` | 存在，且被 strip 过（比构建目录里小 ~40 MB） |
| 4 | 构建目录 `sunshine.exe` 的版本串 | = 当前 HEAD 短哈希 |
| 5 | MSI 解包后四个 exe 的 SHA256 | 与 `_CPack_Packages` 暂存树一致 |
| 6 | `wix.log` 的错误 | 只有 `WIX1148` 一条 warning，**无 error** |
| 7 | `cpack_artifacts\` 里不该出现的 | 上次打包留下的旧 `Sunshine.msi`（时间戳没更新 = 打包没跑）；以及**从不会更新的 `Sunshine.zip`**（历史遗留） |

---

## 5. 复制粘贴版全流程

```powershell
# ==== Sunshine Windows 打包全流程（msys2 / ucrt64）====
$MSYS   = 'E:\msys64\msys2_shell.cmd'
$SRC    = '/e/GitHub/fork/Sunshine'                        # msys 风格（给 -c 用）
$BUILD  = '/e/GitHub/fork/Sunshine/cmake-build-msys2'
$SRCW   = 'E:\GitHub\fork\Sunshine'                        # Windows 风格（给 PowerShell 用）
$BUILDW = 'E:\GitHub\fork\Sunshine\cmake-build-msys2'

Set-Location $SRCW
git status --short; git log --oneline -1

# 1) 配置（含 WiX 安装）
& $MSYS -defterm -here -no-start -ucrt64 -c "cmake -S $SRC -B $BUILD 2>&1 | tail -40"

# 2) 编译（-j 按 RAM_GB/4 取；先跑完再跑第 3 步，别并发）
& $MSYS -defterm -here -no-start -ucrt64 -c "cd $BUILD && cmake --build . --target sunshine sunshinesvc dxgi-info audio-info -j 16 > /tmp/build.log 2>&1; echo EXIT=`$?; tail -5 /tmp/build.log"

# 3) Web UI
& $MSYS -defterm -here -no-start -ucrt64 -c "cd $BUILD && cmake --build . --target web-ui > /tmp/webui.log 2>&1; echo EXIT=`$?; tail -8 /tmp/webui.log"

# 4) MSI（-G WIX 不能省）
& $MSYS -defterm -here -no-start -ucrt64 -c "cd $BUILD && cpack -G WIX --config CPackConfig.cmake > /tmp/cpack.log 2>&1; echo EXIT=`$?; tail -20 /tmp/cpack.log"

# 5) 自检
& $MSYS -defterm -here -no-start -ucrt64 -c "cd $BUILD && strings -a sunshine.exe | grep -E '^0\.0\.0-' | head -2"
Get-Item "$BUILDW\cpack_artifacts\Sunshine.msi" | Select-Object FullName,Length,LastWriteTime
Get-FileHash "$BUILDW\cpack_artifacts\Sunshine.msi" -Algorithm SHA256
```

---

## 6. 故障对照表

| # | 现象 | 原因 | 处置 |
|:-:|------|------|------|
| 1 | `cc1plus.exe: out of memory allocating N bytes` / `ninja: build stopped: subcommand failed.` | 并发数过高；或与 Vite 构建抢内存 | 加 `-j 16`（≈ RAM_GB/4）；**串行**执行步骤 2、3 |
| 2 | 明明编译报错，命令却"看起来"成功 | ① 用了 `... \| tail -N`，`$?` 反映的是 `tail`；② 退出码写成 `echo EXIT=\$?`，PowerShell 先把 `$?` 展开成了布尔值 | 日志重定向到文件；退出码用反引号转义：`` echo EXIT=`$? `` |
| 3 | `cpack` 只产出 `Sunshine.exe`，没有 MSI | 裸 `cpack` 走 `CPACK_GENERATOR=NSIS`（`CPACK_BINARY_WIX=OFF`） | 显式 `cpack -G WIX` |
| 4 | 配置期 FATAL：`The MSYS2 Node.js package is incompatible with Rolldown.` | msys2 自带的 node 用了 GNU 风格的 `dll.a` 绑定 | 用**原生** Node.js：配置时指定 `-DNPM=E:/node/npm`（本机已是） |
| 5 | `find_program(NPM npm REQUIRED)` 找不到 npm | 路径里没有 node | 把 `E:\node` 加进 `PATH`，或用 `-DNPM=...` |
| 6 | 没有任何报错，但 MSI 死活打不出来 | **缺 .NET SDK**：`windows_wix.cmake` 只 warning 就 `return()` | 装 .NET SDK，重跑步骤 1 |
| 7 | `Cannot create a file when that file already exists.` + `已安装工具"wix"` | `dotnet tool install` 的幂等提示 | 忽略 |
| 8 | `Warning WIX1148: Invalid MSI package version` | `PROJECT_VERSION` 含字母标签（§2.3） | 忽略；MSI 有效版本恒为 `0.0.0`，覆盖安装靠固定 `UpgradeCode` + `REINSTALLMODE=amus` |
| 9 | 版本号里有 `-dirty` | `git diff --quiet --exit-code` 非 0 —— 即有**已跟踪文件未暂存**的改动；未跟踪目录/脏子模块**不会**触发 | 无害。用 `git diff --stat` 定位后提交或 stash 掉即可消除 |
| 10 | 包里是旧 Web UI | 只跑了步骤 2，把 Vite 产物当成了源文件 | 跑步骤 3（`--target web-ui`） |
| 11 | `npm ci` 失败 / 超时 | `NPM_OFFLINE=OFF` 需要联网 | 恢复网络；或用 `-DNPM_OFFLINE=ON` + 完整 `node_modules` |
| 12 | 改了源码但包没变 | 只重跑了 `cpack`，跳过了步骤 1/2 | **版本号是编译宏**，改动后必须重跑配置与编译 |
| 13 | 全量重编，慢得离谱 | 版本号（`PROJECT_VERSION`）进了**每个** `.obj` 的 `DEFINES` | 正常现象。别为了试版本号反复 configure |

---

## 7. 产物清单（本机 `1ffccfe5` 实测）

| 产物 | 大小 | 说明 |
|------|------|------|
| `cmake-build-msys2\cpack_artifacts\Sunshine.msi` | 34,134,065 B（32.55 MiB） | ★ **交付产物**。`SHA256 D4389554…D800378` |
| `cmake-build-msys2\cpack_artifacts\Sunshine.exe` | 29,869,968 B | NSIS 安装器（步骤 5，可选） |
| `cmake-build-msys2\cpack_artifacts\Sunshine.zip` | 38,588,703 B | ⚠️ **历史遗留**（2026-10-11 的 ZIP 生成器产物，本流程不会再更新它）。别误当成新产物交付 |
| `cmake-build-msys2\sunshine.exe` | 122,839,354 B | 未 strip 的主程序（调试/直接运行用） |
| `…\_CPack_Packages\win64\WIX\Sunshine\` | 134 个文件 / 7 个组件 | 安装树镜像，组件为 `application` `assets` `audio` `autostart` `dxgi` `firewall` `Unspecified` |
| `…\_CPack_Packages\win64\WIX\wix.log` | — | `wix.exe` 原始日志，排打包问题先看它 |

**MSI 里的组件布局**（`_CPack_Packages\win64\WIX\Sunshine\`，`<组件>\<安装树相对路径>`）：

```
application\   sunshine.exe   zlib1.dll   tools\sunshinesvc.exe
assets\        assets\{apps.json, shaders\}   scripts\{*.ps1, *.bat, service\, migration\, path\}
Unspecified\   assets\web\ (81 个文件)   assets\{box.png, desktop.png, desktop-alt.png, steam.png}
audio\         tools\audio-info.exe
dxgi\          tools\dxgi-info.exe
autostart\     scripts\autostart-service.bat
firewall\      scripts\add-firewall-rule.bat   scripts\delete-firewall-rule.bat
```

> ⚠️ `Unspecified` 不带 `COMPONENT` 声明，是 [packaging/common.cmake:48](../../../cmake/packaging/common.cmake#L48)
> 的 `install(DIRECTORY .../assets/web ...)` 落下来的——**Web UI 就藏在这个名字很不起眼的组件里**。

---

> **维护提示**：本项目**只用 `en` 语言包**（不碰 `en-US` 等变体）；C/C++ 代码风格以 `.clang-format` 为准；
> 新增/修改的函数必须有 Doxygen 注释，否则文档构建会失败。
> 打包相关的**唯一真相源**是 [cmake/packaging/](../../../cmake/packaging/)：
> `common.cmake`（通用 + 资产）、`windows.cmake`（组件与安装规则）、`windows_wix.cmake`（MSI）、
> `windows_nsis.cmake`（NSIS）。上面的流程若与代码冲突，**以代码为准**并回来更新本篇。
