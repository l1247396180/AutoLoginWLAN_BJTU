# 校园网自动登录程序（AutoLogin）

一个轻量级的 Windows 后台程序，用于自动检测校园网连接状态并在需要时自动完成 Portal 认证登录。

- **运行开销**：常驻内存约 2 MB，事件驱动，CPU 占用接近 0
- **完全静默**：无窗口、无弹窗，所有运行信息写入日志文件
- **开机自启**：自动注册到用户级启动项，无需管理员权限

---

##  功能特性

| 功能 | 说明 |
|---|---|
|  网络识别 | 基于 Windows NLM（Network List Manager）识别指定校园网是否已连接 |
|  互联网检测 | NLM 连通性标志 + HTTP 内容校验双判据，可识别 Captive Portal 劫持 |
|  IP 校验 | 登录前对网卡 IP 做 5 道校验，避免取到 APIPA / 虚拟网卡地址导致登录失败 |
|  自动登录 | 调用 H3C ePortal 认证接口完成登录，支持失败重试 |
|  智能节流 | 60 秒内不重复发起登录请求，避免触发服务器风控 |
|  定时重试 | 登录失败后每 30 秒自动重试，登录成功立即停止 |
|  自动退出 | 未检测到校园网时程序静默退出，不占用资源 |
|  日志记录 | 全流程写入 `AutoLogin.log`，便于排查问题 |

---

## 适用环境

- **操作系统**：Windows 10 / Windows 11（需支持 NLM 与 WinHTTP）
- **认证系统**：H3C ePortal（`/eportal/portal/login` 接口）
- **网络环境**：WiFi 或以太网，校园网认证后访问外网

> 其他 Portal 系统（如深澜、Dr.COM）接口不同，需修改 `PerformCampusLogin()` 中的请求构造逻辑。

---

## 编译方法

### 依赖环境

使用 **MSYS2 UCRT64** 工具链（GCC 版本建议 ≥ 13）：

```bash
# 首次使用需安装工具链
pacman -S --needed base-devel mingw-w64-ucrt-x86_64-toolchain
```

### 编译命令

```bash
g++ AutoLoginWLAN_BJTU.cpp -o AutoLogin.exe ^
    -lole32 -loleaut32 -luuid ^
    -lwinhttp -ladvapi32 ^
    -liphlpapi -lws2_32 ^
    -municode -mwindows ^
    -static -static-libgcc -static-libstdc++
```

**参数说明**：

| 参数 | 作用 |
|---|---|
| `-lole32 -loleaut32 -luuid` | COM 组件与 `INetworkListManager` 相关符号 |
| `-lwinhttp` | HTTP 请求 |
| `-ladvapi32` | 注册表操作（开机自启动） |
| `-liphlpapi -lws2_32` | 网卡枚举与 IP 地址查询 |
| `-municode` | 支持 `wWinMain` 入口 |
| `-mwindows` | 不弹出控制台窗口 |
| `-static -static-libgcc -static-libstdc++` | **静态链接，避免依赖 MSYS2 运行时 DLL** |

>  **必须使用静态链接**。否则双击 exe 或开机自启动时，因环境变量 PATH 中没有 `C:\msys64\ucrt64\bin`，程序会因找不到 `libstdc++-6.dll` 等库而静默崩溃。

### 验证静态链接

```bash
ldd AutoLogin.exe
```

输出中**不应出现** `libstdc++-6.dll`、`libgcc_s_seh-1.dll`、`libwinpthread-1.dll`。

---

## ⚙️ 配置

修改源码顶部的**配置区**，然后重新编译：

```cpp
// ================================ 配置区 ================================
const std::wstring CAMPUS_NETWORK_NAME  = L"web.wlan.bjtu";      // 校园网名称
const std::wstring CAMPUS_IP_PREFIX     = L"172.29.";            // 校园网网段前缀（可留空）
const std::string  CAMPUS_USER_ACCOUNT  = "25531106";            // 校园网账号
const std::string  CAMPUS_USER_PASSWORD = "Li2418412312..";      // 校园网密码
const std::wstring CAMPUS_LOGIN_HOST    = L"10.10.42.3";         // 认证服务器 IP
const INTERNET_PORT CAMPUS_LOGIN_PORT   = 801;                    // 认证服务器端口
const DWORD        LOGIN_THROTTLE_MS    = 60000;                  // 登录节流（毫秒）
// =======================================================================
```

### 配置项说明

| 配置项 | 获取方法 |
|---|---|
| `CAMPUS_NETWORK_NAME` | 打开「控制面板 → 网络和共享中心」，查看当前连接的网络名称 |
| `CAMPUS_IP_PREFIX` | 连接校园网后，`ipconfig` 查看 IPv4 前两段（如 `172.29.`）。留空则跳过网段校验 |
| `CAMPUS_LOGIN_HOST` / `PORT` | 浏览器打开认证页面，地址栏中的 IP 和端口 |
| `CAMPUS_USER_ACCOUNT` / `PASSWORD` | 校园网账号密码 |

>  **安全提醒**：密码以明文形式存储在源码中，编译后的 exe 存在被反编译泄露的风险。建议后续升级为 Windows DPAPI 加密存储（见"后续改进"一节）。

---

## 使用方法

### 1. 首次运行

双击 `AutoLogin.exe` 运行一次，程序会：

- 自动写入注册表 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`
- 在当前目录生成 `AutoLogin.log`
- 若检测到校园网且未联网，立即尝试登录

### 2. 日常使用

无需手动干预：

- 开机自动启动，监听网络事件
- 检测到校园网未认证时自动登录
- 登录成功后停止一切活动，静默驻留内存
- 断开校园网（切换到其他网络）时自动退出

### 3. 手动停止

在任务管理器中结束 `AutoLogin.exe` 即可。若想同时**取消开机自启动**：

```cmd
reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v MyCampusAutoLogin /f
```

---

## 📋 日志说明

日志文件位于 **exe 所在目录**的 `AutoLogin.log`（不是当前工作目录，避免自启动时无写权限）。

### 关键日志片段示例

**成功登录流程**：

```
[2026-10-20 22:05:47] ===== 程序启动 =====
[2026-10-20 22:05:47] 已连接至校园网: web.wlan.bjtu
[2026-10-20 22:05:47] [联网/NLM] 网络 'web.wlan.bjtu' conn=0x00000022 v4=N v6=N
[2026-10-20 22:05:47] [联网/HTTP] http://www.msftconnecttest.com/connecttest.txt status=302
[2026-10-20 22:05:47] [联网/HTTP] 被重定向，判定为 captive portal 劫持
[2026-10-20 22:05:47] [联网/汇总] NLM=N HTTP=N
[2026-10-20 22:05:49] 无法访问互联网，准备登录。
[2026-10-20 22:05:49] 启动重试定时器（30 秒）。
[2026-10-20 22:05:49] [IP校验] 校园网网卡 IP: 172.29.91.195
[2026-10-20 22:05:49] [IP校验] 通过。
[2026-10-20 22:05:49] 登录响应: dr1003({"result":1,"msg":"Portal协议认证成功！"});
[2026-10-20 22:05:49] 校园网登录成功。
[2026-10-20 22:05:51] 复检：互联网已可访问。
[2026-10-20 22:05:51] 停止重试定时器。
```

### 日志级别关键字

| 关键字 | 含义 |
|---|---|
| `[联网/NLM]` | Windows NLM 连通性检测结果（`v4=Y` 表示系统已识别互联网） |
| `[联网/HTTP]` | HTTP 内容检测结果；出现"疑似被劫持到登录页"说明需要认证 |
| `[联网/汇总]` | 双判据汇总，`NLM=Y` 或 `HTTP=Y` 任一成立即判定为已联网 |
| `[IP校验]` | 本机 IP 多道校验，只有全部通过才会发起登录 |
| `网络连通性变化事件` | NLM 事件回调；连续多次出现属于正常现象（系统状态迁移会触发多次） |
| `距上次登录不足 60 秒` | 触发登录节流，程序会等定时器再次触发 |
| `未检测到校园网，程序退出` | 当前所有网络均不匹配校园网，程序主动退出 |

---

## 程序架构

```
wWinMain
  ├── 单实例互斥（防止多开）
  ├── 初始化 COM / WinSock
  ├── SetAutoStart(true)              写入开机启动项
  ├── 注册 NLM 事件监听
  │     └── NetworkConnectionConnectivityChanged → CheckAndAct
  ├── CheckAndAct()                    启动时立即检查一次
  └── GetMessage 消息循环              阻塞等待事件（CPU 占用 ≈ 0）
        └── WM_TIMER（30 秒）→ CheckAndAct   登录失败重试

CheckAndAct()
  ├── 遍历已连接网络，匹配 CAMPUS_NETWORK_NAME
  ├── 未匹配 → PostQuitMessage 退出
  ├── CheckInternetConnectivity()
  │     ├── CheckInternetViaNLM()      NLM_CONNECTIVITY_IPV4_INTERNET
  │     └── CheckInternetViaHttp()     多目标 + 禁止重定向 + 内容校验
  ├── 已联网 → 停止重试定时器，返回
  ├── 未联网 → 启动重试定时器
  ├── VerifyLocalIp()                  IP 多道校验
  └── PerformCampusLogin()             发送 Portal 认证请求
```

### 关键设计决策

| 设计 | 原因 |
|---|---|
| **事件驱动 + 定时器** | 用 `GetMessage` 阻塞代替轮询，实现低 CPU 占用 |
| **3 秒事件去重** | NLM 在认证成功后可能连发 5~6 次事件，避免重复检测 |
| **60 秒登录节流** | 防止服务器风控封禁账号或 IP |
| **NLM 或 HTTP 双判据** | NLM 对状态更新有 10~30 秒延迟；HTTP 更实时。任一通过即认为已联网 |
| **禁止 HTTP 自动重定向** | 校园网未认证时常劫持 HTTP 请求到登录页（返回 302 或伪 200），需识别并判为未联网 |
| **IP 多道校验** | ePortal 只认请求源 IP 与 `wlan_user_ip` 参数一致，取错 IP 会导致登录失败 |
| **日志使用绝对路径** | 开机自启动时 CWD 为 `C:\Windows\System32`，相对路径会静默写入失败 |

---

## 🔧 常见问题

### Q1: 双击 exe 后程序瞬间消失，无日志

**原因**：动态链接时缺少 MSYS2 运行时 DLL。

**解决**：使用上面的**静态链接命令**重新编译，并用 `ldd AutoLogin.exe` 确认无 `libstdc++-6.dll` 等依赖。

### Q2: 开机后程序不启动

**排查步骤**：

1. 打开注册表 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`，检查是否存在 `MyCampusAutoLogin` 项
2. 手动双击 exe 确认能否正常启动并生成日志
3. 检查 exe 路径是否被移动（移动后需重新运行一次以更新注册表）

### Q3: 日志里出现"距上次登录不足 60 秒，跳过"

**原因**：上次登录请求失败或未生效，60 秒节流保护期间程序不再发请求。

**解决**：属正常行为，30 秒定时器会在节流期满后自动重试。若持续出现，检查：

- 账号密码是否正确
- 认证服务器 IP/端口是否变化
- `wlan_user_ip` 是否与服务器实际看到的 IP 一致

### Q4: 登录成功但复检仍显示"未联网"

**可能原因**：

- 校园网对某些探测目标做了限制（如屏蔽 `msftconnecttest.com`）
- DNS 缓存了认证前的劫持结果

**解决**：修改 `CheckInternetViaHttp()` 中探测的 URL，换成校园网未拦截的目标（如 `http://www.baidu.com`，但需同时修改内容匹配逻辑）。

### Q5: 日志中网络名显示为乱码（如 `'c(�+...'`）

**原因**：`std::string(netName.begin(), netName.end())` 对含中文的 `wstring` 做了逐字符截断。

**影响**：仅日志显示不美观，不影响功能。

**解决**：若需修复，在 `WriteLog` 前用 `WideCharToMultiByte` 转换为 UTF-8 再传入。

---

## 后续改进建议

- [ ] **密码加密存储**：使用 Windows DPAPI（`CryptProtectData`）加密密码，运行时解密，避免明文暴露
- [ ] **配置文件外置**：将账号、网络名等从源码移到 `config.ini`，无需重新编译即可修改
- [ ] **托盘图标**：提供右键菜单（立即登录、查看日志、退出）而非完全静默
- [ ] **网络名称前缀匹配**：当前为精确匹配 `web.wlan.bjtu`，若 Windows 重新生成网络配置文件为 `web.wlan.bjtu 2`，可改 `rfind(..., 0) == 0` 前缀匹配
- [ ] **开机宽限期**：程序启动后前 60 秒若未检测到校园网，不立即退出而是等待重试，避免 NLA 服务未就绪导致的误退出
- [ ] **多校园网支持**：允许配置多个 SSID 及对应的认证参数

---

## 许可

本项目仅供学习交流使用。请勿用于任何违反校园网使用协议或相关法律法规的场景。使用本程序产生的任何后果由使用者自行承担。

---

## 致谢

- 参考了 H3C ePortal Portal 协议的公开实现
- 基于 [Microsoft Network List Manager API](https://learn.microsoft.com/en-us/windows/win32/api/netlistmgr/) 与 [WinHTTP](https://learn.microsoft.com/en-us/windows/win32/winhttp/) 构建
