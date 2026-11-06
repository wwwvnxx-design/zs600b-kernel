# ZS600B 内核重建 —— M1 云端构建记录

## 结论

GitHub Actions 云端编译**已跑通**。`rockchip_defconfig` + UVC/USB20 配置补丁，产出的
`zImage`（8,067,272 B）与 `boot-new.img`（32 MiB）结构完整，关键驱动全部在位。
尚未刷入设备（刷写时设备未连接）。

| 项 | 值 |
|---|---|
| 仓库 | https://github.com/wwwvnxx-design/zs600b-kernel （public） |
| 工作流 | `.github/workflows/build-kernel.yml` |
| 成功运行 | run #6 / id `42183572439`，耗时 **4m07s** |
| 运行页 | https://github.com/wwwvnxx-design/zs600b-kernel/actions/runs/42183572439 |
| boot-new.img sha256 | `e6328d89955c0fd3b5741982e026e2e357fb8af9eeefd9510132d2e192536806` |

---

## 一、踩通这条路踩的五个坑

按出现顺序，全部已在仓库里修掉。

| # | 现象 | 根因 | 修法 |
|---|---|---|---|
| 1 | `cc1: error: '-Wno-error=dangling-pointer': no option '-Wdangling-pointer'` | `-Wno-error=<type>` 要求该警告名存在；`dangling-pointer` / `use-after-free` 是 **gcc 12+** 才引入的，runner 是 gcc 11 | 不再罗列具体警告名，只用全局 `-Wno-error` |
| 2 | `LZO arch/arm/boot/compressed/piggy.lzo` 阶段失败 | `CONFIG_KERNEL_LZO=y` 时 `scripts/Makefile.lib` 的 `cmd_lzo` 直接调 **`lzop -9`**，runner 默认没装 | `apt install lzop lz4` |
| 3 | `scripts/gcc-wrapper.py: SyntaxError: Missing parentheses in call to 'print'` | rockchip 3.10 顶层 Makefile 第 356 行：**只要该文件存在**就 `CC = scripts/gcc-wrapper.py $(CROSS_COMPILE)gcc`，而它是 **Python 2** 语法 | 编译前删除该文件。它只解析 stderr 把 warning 升级为错误，不改变编译参数，删除不影响产物 |
| 4 | `cc1: error while loading shared libraries: libmpfr.so.4` | 工具链 cc1 动态链接 MPFR 3.x 的 SONAME，Ubuntu 22.04 只有 `libmpfr.so.6` | 下载 `libmpfr4_3.1.4-1_amd64.deb` 解包后注入 `LD_LIBRARY_PATH`。`libgmp.so.10`/`libmpc.so.3` 系统自带不用管。**`gcc --version` 测不出来**，要真编一个 `.c` |
| 5 | `mali400/mali/Kbuild:35: *** Runtime PM is incompatible with non-GPL license. Stop.` | Mali 的 Kbuild 用 `$(wildcard $(src)/linux/license/gpl/*)` 区分 GPL/proprietary，而 `scripts/Makefile.build` 里 `src := $(obj)` 是**相对路径**；`O=` 外部构建时 cwd 在输出目录，通配符必然落空 → 误判为非 GPL → `$(error)` | **改为源码树内构建，不要 `O=`**（原厂就是这么编的）。同时 cache 改为缓存未解压的 tar.gz，保证每次是干净树 |

第 5 条是最隐蔽的：它只在 `O=` 外部构建时出现，而大多数现代构建习惯都用 `O=`。

---

## 二、产物

| 文件 | 大小 | 说明 |
|---|---|---|
| `zImage` | 8,067,272 B | 新内核（原厂 5,137,568 B，大 57%，因配置更全 + 编译器不同） |
| `boot-new.img` | 33,554,432 B | 完整 boot 分区镜像，已按 32 MiB 补齐 |
| `System.map` | 2,898,820 B | 85,045 个符号 |
| `kernel.config` | 105,328 B | 最终生效配置 |

重打包布局（`repack.log`）：

```
基底        : artifacts/boot_device.img（设备实际 dump，含 root 后的 ramdisk）
page_size   : 16384
kernel      : 8067272 B @ 0x4000   → 加载地址 0x60408000
ramdisk     : 1337812 B @ 0x7b8000 → 加载地址 0x62000000（原样保留，长度与基底一致）
second(dtb): 335872 B  @ 0x900000  → 加载地址 0x60f00000（原样保留）
image end   : ≈ 9.6 MiB（分区 32 MiB，余量充足）
```

内核映像 8.07 MB 从 0x60408000 起算，上界约 0x60C08000，未触及 second 的 0x60f00000。

---

## 三、配置覆盖评估

符号数量对比不能只看总数（编译器版本差异会大量制造假缺失）。**规范化符号名**
（剔除 `.part.N` / `.constprop.N` / `.isra.N` / `.cold` 后缀）后：

| 指标 | 数值 |
|---|---|
| 原机符号（规范化后） | 63,070 |
| 新内核（规范化后） | 84,971 |
| 真实缺失 | 8,637（13.7%） |

分子系统真实缺失（已剔除 `.part` 噪音）：

| 子系统 | 缺失 | 判断 |
|---|---|---|
| 音频 | 316 | 原厂编了更多声卡，本机只需 RT/Analog codec |
| GPU Mali | 116 | `kbase` 新内核 699 / 原机 1826，符号数少但驱动在 |
| eMMC/NAND | 73 | 缺的主要是 `rknand_*`，本机是 eMMC，用不到 |
| I2C/SPI/GPIO | 66 | 差异来自 pinctrl 内联 |
| 显示 / HDMI / LCDC | 57 | **新内核 lcdc 符号 204 vs 原机 81**，驱动在且在编，缺失是内联差异 |
| DVFS / 温控 | 42 | — |
| 触摸 / 输入 | 36 | — |
| 电源 / PMIC | 24 | — |
| USB 主机 / OTG | 22 | 主要缺 `dwc_otg_*_suspend/resume`、`ehci_init`，与 PHY 电源管理配置有关 |
| 视频编解码 RGA/VPU | 21 | — |
| USB 设备类 / UVC | 9 | — |
| WIFI / 蓝牙 | 6 | — |

**关键驱动实测均在位**（`kernel.config` + `System.map` 双确认）：

```
CONFIG_USB20_HOST=y          CONFIG_USB20_OTG=y
CONFIG_DWC_OTG_310=y         CONFIG_USB_EHCI_HCD=y
CONFIG_USB_VIDEO_CLASS=y     CONFIG_MEDIA_USB_SUPPORT=y
CONFIG_FB_ROCKCHIP=y         CONFIG_BACKLIGHT_PWM=y
CONFIG_MMC_DW=y              CONFIG_MALI_MIDGARD=y
CONFIG_DEBUG_FS=y            CONFIG_IKCONFIG_PROC=y
```

符号计数（新 / 原机）：lcdc 204/81、hdmi 203/210、dw_mci 114/118、rknand 65/64、
ehci 104/91、dwc_otg 444/443、pwm_backlight 16/24。

**一个此前不知道的事实：原厂内核里 UVC 符号数为 0** —— 原厂根本没有编 UVC 视频类驱动。
这解释了为什么系统相机对采集卡无反应；之前用第三方 App 也黑屏，是因为它走用户态
libuvc + usbfs，同样受 `dwc_otg` 等时传输缺陷影响。新内核首次把 UVC 编了进去（243 个符号）。

---

## 四、下一步

### M1 刷入验证（设备连上即可做）

```bash
bash flash_boot.sh check
bash flash_boot.sh flash output/zs600b-m1/zs600b-boot-m1-run6.img   # 要手打 FLASH
bash flash_boot.sh rollback                                        # 出问题一键回滚
```

通过标准：正常开机、`uname -a` 显示新构建时间、WIFI/触摸/HDMI 正常、**采集卡仍黑屏**（对照，
因为这一版没改驱动）。回滚镜像就是 `artifacts/boot_device.img`。

### M3 修 ISO 调度

刷通 M1 之后再动代码，入口：

- `drivers/usb/dwc_otg_310/dwc_otg_hcd_ddma.c` → `init_isoc_dma_desc()`
- `drivers/usb/dwc_otg_310/dwc_otg_hcd.c` → `dwc_otg_hcd_start_xfer_dma()`
- `drivers/usb/dwc_otg_310/dwc_otg_hcd_intr.c` → ISO 完成回调（`#Iso` 计数为负，疑似重复归还）

改完推仓库，Actions 页点 Run workflow，4 分钟出新 boot.img。刷入后用 `probe/uvc_cap`
（已编译 ARM 版，0xAA 哨兵法）直接验证缓冲区是否被写。

### 已知局限

`verify_kallsyms.py` 的分组是按符号名子串匹配，会出现误分类（例如把
`init_lcdc_device_driver` 归入「USB 设备类驱动」，因为名字里含 `device`）。
看报告时只信**计数趋势**和**具体符号名**，不要信分组标签。
