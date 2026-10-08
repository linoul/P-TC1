# P-TC1 固件编译指南（含 HomeKit demo 分支）

本文件说明如何编译 PrestarLin/P-TC1（TC1 A1 / MK3031 / MiCO）固件，
尤其是加入 `TC1/homekit_demo.c` 后的构建与验证方法。

---

## 〇、为什么本机（Windows 沙箱）编译不了 —— 已实测

MiCO 的构建强依赖 MXCHIP 私有工具链 `mico-os/MiCoder`，在
`HOST_OS=Win32` 下：
- `SHELL = cmd.exe`；
- 强制调用 `mico-os/MiCoder/cmd/Win32/` 下的私有 `perl.exe` / `make.exe` /
  `echo.exe` / `bin2c.exe` / `Python27/python.exe`；
- 以及 `mico-os/MiCoder/compiler/arm-none-eabi-5_4-2016q2-20160622/Win64/bin/arm-none-eabi-gcc`
  （5.4-2016q2 特定版本）。

这些私有件**不在 winget / 任何包管理器**，且官方下载源已失效。本沙箱还实测：
- `wsl.exe` 被安全策略封禁（Program Blacklist），无法用 WSL 复用 CI 的 Linux 环境；
- 本机无 `make` / `arm-none-eabi-gcc` / `python2`。

→ **结论：本机物理上无法编译，唯一可靠路径是仓库自带的 GitHub Actions（`build.yml`）。**

---

## 一、推荐方案：用 GitHub Actions 编译（零环境、已验证）

`build.yml` 已在 Ubuntu 上验证：apt 装 `make/python3/dash/perl`，下载
arm-none-eabi-gcc 5.4-2016q2 放到 MiCoder 期望路径，软链工具，最后
`make TC1@MK3031@moc total` 产出 `ota.bin` 等。

> 前提：`git remote origin` 是对方的 `PrestarLin/P-TC1`，你**没有直推权限**，
> 必须先 **Fork 到你自己的 GitHub 账号**。

### 步骤

1. **Fork**：浏览器打开 https://github.com/PrestarLin/P-TC1 → 右上角 `Fork`
   → 创建到你的账号（例如 `https://github.com/<你的账号>/P-TC1.git`）。

1.5. **开启 fork 仓库的 Actions（关键！fork 默认禁用，否则跑不起来）**：
   进你 fork 的仓库 → Settings → Actions → General：
   - "Actions permissions" 选 **Allow all actions and reusable workflows** → Save；
   - "Workflow permissions" 选 **Read and write permissions** → Save
     （`build.yml` 末尾会 `git push` 回写 `.version` 并创建 Release，需要写权限；
     若只想要 Artifacts 也可保持 read，但最后两步会报错跳过）。

2. **把本机改动推到你的 fork**（在 `fw_prestar/` 目录执行）：
   ```bash
   # 如尚未登录 GitHub CLI（推荐，处理凭证最省事）：
   gh auth login
   # 添加你的 fork 为远程：
   git remote add myfork https://github.com/<你的账号>/P-TC1.git
   # 推到 dev 分支（Actions 会出 Prerelease）或 master（出正式 Release）：
   git push myfork HEAD:dev
   ```
   （若不用 `gh`，可改用 personal access token：
   `git push https://<token>@github.com/<你的账号>/P-TC1.git HEAD:dev`）
   > 注意：本仓库已给 `build.yml` 加了 `workflow_dispatch`，且你本地两次 commit
   >（`e71dede` HomeKit demo + `f740f93` workflow_dispatch）都需推上去才生效。

3. **触发构建**：开启 Actions 权限后页面会出现 `Run workflow` 按钮。两种方式任选：
   - **自动**：步骤 2 的 push 到 `dev`/`master` 会直接触发，无需手动；
   - **手动**：你 fork 的 `Actions` 标签页 → `Build and Release` → `Run workflow`
     → 选 `dev`（或 `master`）→ 运行。

4. **取产物**：
   - 构建完成后在 `Actions` 运行的 `Artifacts` 里下载
     `P-TC1-vX.Y.Z`，内含：
     - `ota.bin` —— OTA 升级用（Web 后台 /ota 上传）
     - `TC1@MK3031@moc.ota.bin` —— OTA 完整包
     - `TC1@MK3031@moc.all.bin` —— 全量固件（含 bootloader，需烧录器）
   - `master` 分支还会自动发 `Releases`（直接下载即可）。

5. **刷入**：
   - OTA：设备 Web 后台 `/ota` 上传 `ota.bin`（约 1 分钟，别断电）；
   - 救砖/全量：`TC1@MK3031@moc.all.bin` + FlashPlus / JTAG。

---

## 二、HomeKit demo 真机验证

编译出的固件启动后：
1. 串口日志应出现 HomeKit / bonjour 启动信息，且**不报 MFiAuth 致命错误**；
2. iPhone 家庭 App → `+` → 添加配件 → 输入配对码 **`123-45-678`**；
3. 应能看到 “Demo Switch”，可开关（回调里目前只翻转一个内存变量 +
   可选 GPIO，未接真实继电器，验证链路即可）。

### 已知未实测风险（必须在真机确认）
- **MFi 软件回退**：`mfi_cp_port = MICO_I2C_NONE` 后，闭源
  `Lib_HomeKit_Server.Cortex-M4.GCC.release.a` 能否优雅退化为自签名证书、
  还是会卡在 `MicoMFiAuthInitialize` 失败 —— 这是 demo 能否跑起来的第一道关。
- **`HKSendNotifyMessage` 签名**：头文件未声明，按 `.a` 导出符号推断；
  若编译报隐式声明或运行异常，把 `homekit_demo.c` 里那次调用注释掉即可。
- **160K SRAM**：链接后剩余堆是否够 HomeKit 配对期缓冲，需编译后看 map /
  运行时 free heap。
- **float ABI**：需 MK3031 板用 `-mcpu=cortex-m4`（软/硬浮点）与 `.a` 一致。
- iOS 会弹 “此配件未认证” 警告（无 MFi 芯片），功能正常，个人用无碍。

---

## 三、备选：本机硬搭 MiCO 工具链（不推荐，不保证成功）

仅在你想完全离线/本地编译时考虑，步骤概览与风险：
1. `winget install` 装 GnuWin32 make / Strawberry Perl / arm-none-eabi-gcc；
2. 从 MXCHIP 镜像找回 `MiCoder` 的 `cmd/Win32`（perl/make/echo/bin2c/python）
   与 arm-gcc 5.4 的 Win64 版，放到 `mico-os/MiCoder/` 对应路径；
3. 用系统 python3 软链伪装成 `cmd/Win32/Python27/python.exe`；
4. `make -f mico-os/makefiles/Makefile TC1@MK3031@moc total HOST_OS=Win32`。

**风险**：MiCoder 私有源多已 404；`SHELL=cmd.exe` 下 MiCO 大量 GNU make
`$(shell)`/`$(file)` 语法极易崩；arm-gcc 5.4 Windows 版难找。成功率低，
不建议把时间花在这条路上。
