# ZS600B USB 采集卡黑屏 —— 根因诊断与处置建议

日期：2026-11-06
设备：DJI CrystalSky ZS600B（RK3288 / Android 5.1.1 / 内核 3.10.0），已 root
采集卡：MacroSilicon **534d:2109**（MS2109，UVC 1.00，MJPEG + YUY2，11 档分辨率，3 个 alt）

---

## 一、结论（先看这段）

**这不是 App 的问题，也不是采集卡或 HDMI 源的问题，是这台平板的 USB-A 主机控制器在内核层的等时（isochronous）传输数据通路有缺陷。**

平板 USB-A 口挂的不是标准 EHCI，而是 Synopsys 的 **dwc_otg**（`usb20_host`，驱动版本 3.10a / 21-DEC-2012，Rockchip 分支）。在这套驱动上，UVC 流走 usbfs 等时传输时：

- 控制面完全正常（PROBE / GET / COMMIT 全部返回 OK，设备回填的参数一字不差）
- 链路层数据真实存在（速率与分辨率、格式严格对应，最大跑到 17.3 MB/s）
- 内核 URB 完成时也报告了**正确且逐包波动**的 `actual_length`（12~3070 字节）
- **但 payload 一个字节都没有落进用户态 URB 缓冲区**，缓冲区里预填的 `0xAA` 哨兵零覆盖

也就是说：卡在发、控制器在收、驱动知道收了多少，**数据被 DMA 到了错误的地方或根本没回写**。任何走 UVC 的 App（USB Camera 的 libuvc、我们自编的 libusb 探针）在这台机器上都会拿到全零 → 黑屏。

> 同一张卡插手机正常、插 PC 正常，且插到平板时 PC 侧（HDMI 源）能识别到显示器 —— 说明卡、线、HDMI 源、供电全部无问题。

---

## 二、证据链

### 2.1 主机控制器身份（决定性）

```
<6>[1.513116] usb20_host ff540000.usb: DWC OTG Controller
<6>[1.513142] usb20_host ff540000.usb: new USB bus registered, assigned bus number 3
<6>[1.513375] usb usb3: Manufacturer: Linux 3.10.0 dwc_otg_hcd
```

| 总线 | 平台设备 | 驱动 | 产品名 | 用途 |
|---|---|---|---|---|
| bus1 | `ff500000.usb` | `rockchip_ehci_host` | EHCI Host Controller | 板载 WiFi（ath6kl，`maxchild=1`，内部占用） |
| bus2 | `ff580000.usb` | `usb20_otg` | DWC OTG Controller | micro-USB（接电脑） |
| **bus3** | `ff540000.usb` | **`usb20_host`** | **DWC OTG Controller** | **USB-A（本卡所在）** |

关键否定项：**只有 bus1 是真正的 EHCI**，而它的唯一端口被板载 WiFi 模块占死，没有外部连接器。USB-A 无法改走 EHCI —— 端口到控制器的路由是 DTS 里定死的。

### 2.2 数据面异常的直接证据

工具：`uvc_iso`（armv7/API21 交叉编译，静态链接 libusb 1.0.27）。
缓冲区在提交前整块填 `0xAA`，收完后统计"非 0xAA 字节数"。DMA 只要写过，哨兵必被破坏。

| 场景 | 收回速率 | URB 完成情况 | 包长范围 | 非哨兵字节 |
|---|---|---|---|---|
| MJPEG 1920×1080 | 3.19 MB/s | 全部 COMPLETED | 12 ~ 3070 | **0** |
| MJPEG 640×480 | 0.66 MB/s | 全部 COMPLETED | — | **0** |
| YUY2 640×480 | 17.3 MB/s（≈28 fps） | 全部 COMPLETED | — | **0** |

三种格式/分辨率、零到 17 MB/s 全带宽区间，行为完全一致：**长度对、数据无**。
17.3 MB/s 能稳定跑通，顺带证明**不存在供电不足**，也不存在带宽不足。

### 2.3 已排除的假设

| 假设 | 验证方法 | 结果 |
|---|---|---|
| 控制时序/选择子错误 | 早期我把 `VS_COMMIT_CONTROL` 写成 0x03（那是 STILL_PROBE），已修正为 0x02、`bRequest=SET_CUR` | 修正后 COMMIT 通过，排除 |
| UVC 版本导致结构长度不符 | 按 bcdUVC 选择 26 字节（UVC 1.0）/34 字节 | 设备要 26 字节，已按此提交 |
| `dwMaxPayloadTransferSize` 校验 | 试 0/800/1024/2048/3072，并保证先切 alt 再提交 | 与参数无关 |
| 等时提交顺序（先 commit 还是先切 alt） | 两种顺序各跑一遍 | 无差异 |
| ARM 缓存未失效（DMA 写了但 CPU 读到旧缓存） | 每次收完对全部缓冲区执行 `__builtin___clear_cache` | **无差异**，排除 |
| 缓冲区对齐/DMA 边界 | 32 字节 vs 4096 页对齐 | 无差异 |
| 多包 URB 的 stride 计算 | 每 URB 1 / 8 / 32 包 | 无差异 |
| 端口自动挂起打断流 | `power/control=on`，`runtime_status=active` 下重测 | 无差异 |
| 采集卡或 HDMI 源故障 | 插手机正常显示；插平板时 PC 侧识别到显示设备 | 排除 |
| 供电不足 | YUY2 17.3 MB/s 稳定 | 排除 |

### 2.4 内核日志里的驱动缺陷痕迹

抓流期间新增的 dmesg（20 行，其余全是噪声）：

```
<4>[8632.518517] dwc_otg_hc_halt: hcchar.b.chen 1, ep_type 1
<4>[8635.275943] dwc_otg_hc_halt: hcchar.b.chen 1, ep_type 1
...（重复）
```

`ep_type 1` 即等时端点。该打印来自 `dwc_otg_hc_halt()` —— 关通道时 `HCCHAR.ChannelEnable` 仍为 1 的超时告警，是 `dwc_otg` 周期通道处理不干净的典型症状。

### 2.5 为什么手机没事

手机用的是 dwc2 / dwc3 / xHCI，等时传输的 DMA 路径成熟。`dwc_otg` 是 Synopsys 2012 年的老驱动（`version 3.10a 21-DEC-2012`），Linux 主线在 3.17 之后就把它整体替换为 dwc2 了。RK3288 老 BSP 一直沿用，等时路径的 bug 从未修。

---

## 三、处置建议

| 方案 | 可行性 | 成本 | 延迟 | 说明 |
|---|---|---|---|---|
| **A. 改插 micro-USB（OTG）口再试** | 低（同为 dwc_otg 驱动，bus2/bus3 产品名一致） | 一个 OTG 转接头 | — | 唯一零成本尝试。注意该口切 Host 后 adb 会断，需脚本把结果写 `/sdcard` 再回插电脑读取 |
| **B. HDMI → IP 编码器 + 平板放流** | **高** | 200–500 元硬件 | 150–400 ms | 平板侧只需 RTSP/HTTP，完全不碰 USB 等时路径。推荐 |
| **C. 采集卡插手机/PC，无线推到平板** | 高 | 0（有手机即可） | 200–600 ms | 用现成设备的 UVC 能力做采集端 |
| **D. 放弃 HDMI 采集，平板专做 SDR 终端** | — | 0 | — | USB **批量（bulk）**传输在这台机器上完全正常：RTL-SDR 实测 3.47 MB/s、ADS-B 20 秒 194 条报文 |

**不建议**：改内核。`dwc_otg` 是内建的（`/proc/modules` 里只有 mali_kbase、drmboot），要修必须重编内核，而 DJI 的 DTS/U-Boot/boot header 校验没有源码，风险是再次变砖 —— 之前已经救过一次，不值得为一块 200 元的采集卡去赌。

---

## 四、复验工具（保留在设备 `/data/local/tmp/hdmi/`）

| 文件 | 用途 |
|---|---|
| `uvc_iso` | **核心工具**。分组测试（分包数×对齐×cache flush×alt/格式），输出哨兵覆盖率 |
| `uvc_cap` | 单次抓流，命中 `FFD8/FFD9` 才落盘 jpg |
| `uvc_probe` | 打印格式/帧/alt 表，变体矩阵测 PROBE/COMMIT |
| `uvc_raw` | 不做任何握手直接收原始流（用于判断"不提交时设备吐什么"） |
| `uvc_err` | 读 VS_STREAM_ERROR_CODE |
| `hdmi_check.sh` | 一键：抓 3 秒 + 报"完整帧"数量 |
| `run_variants.sh` | 三种格式/分辨率批量对比 |
| `dwcinfo.sh` / `kdump.sh` | 控制器身份、驱动参数、端口电源状态取证 |

换一张卡、换一个固件、或日后改了内核，用 `sh /data/local/tmp/hdmi/hdmi_check.sh 3` 看"完整帧"是否 > 0 即可判定。

源码在 `output/root/hdmi/*.c`，全部 armv7/API21 静态链接 libusb 1.0.27，无系统分区写入。

---

## 4.1 一键验证 OTG 口（`otg_onekey.sh`）

micro-USB（`ff580000`，bus2）是 adb 唯一的链路，一旦拔掉就失去远程控制。所以这里的难点不是抓流，而是**让脚本在 adb 掉线后继续活着**。

### 设计要点

| 问题 | 做法 |
|---|---|
| adb 掉线时 adb shell 的 pty 关闭 → SIGHUP 打死子进程 | 双重 fork `( ... & )` 脱离父子关系 + `trap '' HUP`。设备上**没有 `setsid`、没有 `nohup`**，只能这样 |
| 无法事后判断"它到底还活着吗" | 每 1 秒写 `/sdcard/otg_beat.txt`，内容含单调递增的等待秒数 |
| 采集卡此刻正插在 USB-A（`3-1`），会被立刻误认成"新设备" | 启动时快照 `/sys/bus/usb/devices/[0-9]-*` 成基线，只匹配**基线之外**的路径 |
| 判据要不依赖文本处理（toolbox **无 grep/sed/awk/head/tail/wc**） | `uvc_cap` 最后一行直接打印 `VERDICT OK|PARTIAL|NOFRAME`，脚本只落盘不算 |
| 心跳文件可能被读到半截 | 写 `.tmp` 再 `mv` 覆盖（rename 原子） |

### 用法

```
su -c 'sh /data/local/tmp/hdmi/otg_onekey.sh start'    # 启动，900 秒窗口
su -c 'sh /data/local/tmp/hdmi/otg_onekey.sh status'   # 看心跳与结果
su -c 'sh /data/local/tmp/hdmi/otg_onekey.sh stop'     # 停
```

结果落 `/sdcard/otg_result.txt`，同时导出 `/sdcard/otg_dmesg.txt`（看有无 `dwc_otg_hc_halt`）、命中帧时另存 `/sdcard/otg_frame.jpg`。

### VERDICT 取值

| 值 | 含义 |
|---|---|
| `OK` | 抓到完整 JPEG 帧 → OTG 口等时通路正常，黑屏另有原因 |
| `PARTIAL` | 内核写过数据但凑不出完整帧 |
| `NOFRAME` | 缓冲区一个字节都没被写过 → 与 USB-A 完全同一个病 |
| `NODEVICE` | 900 秒内没等到新设备（转接头 ID 脚没接地 / 卡没枚举上） |
| `FLAPPED` | 设备出现后 4 秒内消失（供电或接触） |
| `OTHERDEVICE` | 新设备 VID 不是 `534d`，不做 UVC 抓流 |

### 存活实测

启动后主动 `adb kill-server` 制造 25 秒断线，心跳从 `等待中 28s` 直接跳到 `等待中 58s` —— 计数器跨过断线窗口连续递增，证明进程未被回收。

### 先行结论

`usb2` 与 `usb3` 的 `manufacturer` 都是 `Linux 3.10.0 dwc_otg_hcd`：

```
usb2(OTG, ff580000) : Linux 3.10.0 dwc_otg_hcd / DWC OTG Controller
usb3(USB-A,ff540000): Linux 3.10.0 dwc_otg_hcd / DWC OTG Controller
usb1(WiFi) : Linux 3.10.0 ehci_hcd / EHCI Host Controller   ← 唯一 EHCI，但其端口被板载 WiFi 占死
```

OTG 口与 USB-A 挂在**同一个驱动**上，所以 `NOFRAME` 是极高概率结果。这一测的价值在于把"几乎必然"变成"确定"，避免再买一张卡重复验证同一缺陷。

### 实测结果（2026-11-06 11:31）：`NODEVICE`

采集卡经 OTG 转接头接入 micro-USB 后，**完全没有枚举**（脚本 226 秒心跳期间 `/sys/bus/usb/devices` 里始终只有三个根 hub）。dmesg 时间线给出了确切原因：

| 时刻 (s) | 内核日志 | 解读 |
|---|---|---|
| 9587.99 | `********session end ,soft disconnect***********` | micro-USB 从电脑拔出 → gadget 断开 |
| 9589.57 | `usb 3-1: USB disconnect, device number 5` | 采集卡同时从 USB-A 被拔下 |
| **9594.57** | `[otg id chg] last id 1 current id 0` | **ID 脚被拉低 → 转接头确实生效，口子切 Host** |
| 9594.68 | `Using Buffer DMA mode … ^^^^^^^^^^^^^^^^^^Host Mode` | 进入主机模式 |
| 9594.73 | `Init: Power Port (0)` / `battery_charger_detect 5` | 端口上电流程走了 |
| **9594.73** | **`ff580000.usb resume, HPRT0:0x1000`** | **HPRT0 bit0(connect)=0 → 端口没有检测到任何设备** |
| 9625.18 | `[otg id chg] last id 0 current id 1` / `PortPower off` | 拔掉转接头 → 回 Device Mode（第一次尝试 30.6 s） |
| 9633.15 | `[otg id chg] last id 1 current id 0` → Host Mode | 第二次尝试（仅维持 6 s） |
| 9655.50 | `vbus detect` / `soft connect` / `USB_STATE=CONFIGURED` | micro-USB 插回电脑，adb 恢复 |

**结论**：转接头是好的（ID 脚正确接地，驱动层确认真进了 Host Mode 且拉起了端口电源），但 `HPRT0=0x1000` 表示连接位始终为 0 —— **采集卡在这 30 秒里没有拿到可用 VBUS，或转接头只通了电源没通 D+/D-**。RK3288 的 OTG 口没有独立的 VBUS 升压通路可查（16 个 regulator 里没有 usb/vbus 项），无法软件加压。

### 双重死路

即便换一根**带外部供电的 OTG Y 型线**解决了供电、让卡枚举成功，等时通路上仍然是 `dwc_otg`，`uvc_iso` 的哨兵测试结论（payload 不落用户缓冲区）不会改变。**两条路各自独立地堵死了 UVC 采集**。

零成本可自行验证转接头是否为"充电专用"：把转接头插到**手机**上、卡插转接头——手机能出画面说明转接头没问题、是平板 OTG 供电不足；手机也不认说明转接头只通了电。

---

## 五、一句话复述

采集卡没问题、HDMI 源没问题、App 没问题。这台平板的两条可用链路各自独立地堵死了 UVC 采集：

1. **USB-A（`ff540000` / `dwc_otg`）** —— 等时传输的 payload 不落到用户态缓冲区，任何采集卡都只能拿到黑帧；
2. **micro-USB OTG（`ff580000` / 同一个 `dwc_otg`）** —— 转接头虽能切 Host Mode，但采集卡根本不枚举（`HPRT0=0x1000`），且即使枚举成功，等时通路仍是同一个缺陷。

换 HDMI over IP（编码器 + 平板放 RTSP）是唯一不与 USB 等时传输打交道的方案。平板做 SDR 终端不受影响，因为 RTL-SDR 走 **bulk** 传输，实测 3.47 MB/s 完全正常。
