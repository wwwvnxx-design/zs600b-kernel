# ZS600B 内核编译包

> 目标回顾：USB-A 口的等时传输（UVC 采集卡）在 `dwc_otg` 驱动下收不到数据。
> 根因在内核驱动层，只能重编内核。本包把「取证结果 + 编译环境 + 打包工具 + 刷写脚本」全带齐了，
> 不需要再摸一遍设备。

**两条路，二选一：**

| 路线 | 适合 | 入口 |
|---|---|---|
| **GitHub Actions 云端编**（当前采用） | 不想装 Linux，出一条 `boot-new.img` 就走 | 第 7 节 / 直接看仓库 Actions 页 |
| 本地 Ubuntu 编 | 需要反复改代码调试、跑得快 | 第 2–3 节 |

---

## 1. 包里有什么

| 文件 | 作用 |
|---|---|
| `.github/workflows/build-kernel.yml` | **云端编译主流程**：拉源码 → 编 zImage → 符号体检 → 打包 boot.img → 出 artifact |
| `setup_ubuntu.sh` | （本机路线）装依赖 → 下 ARM 交叉 gcc 4.9.4 → 下 Rockchip 3.10 源码 → 生成 `~/zs600b/env.sh` |
| `build_kernel.sh` | （本机路线）`rockchip_defconfig` + ZS600B 配置片段 → 编出 `out/arch/arm/boot/zImage` |
| `repack_boot.py` | 把 zImage 塞回 boot.img，保留原 ramdisk / dtb（=保留 root 和 DTS） |
| `verify_kallsyms.py` | 新内核符号 vs 原机 63525 个符号，按子系统列出「缺了什么」 |
| `flash_boot.sh` | adb + dd 刷 p5，含刷前备份、写后回读校验、一键回滚 |
| `artifacts/boot_device.img` | **设备当前 boot 分区的完整 dump**（重打包基底 + 回滚镜像） |
| `artifacts/dji_orig.dts / .dtb` | 反编译出来的完整设备树（6803 行） |
| `artifacts/ks_stock.txt` | 原机 `/proc/kallsyms`（63525 个符号，配置基线） |
| `artifacts/buildinfo.txt` | 设备全部实测事实：分区表、boot 头、USB 控制器、已知缺陷 |
| `refsrc/` | `dwc_otg_310` 核心源文件（离线查阅，不用再联网找） |
| `probe/` | UVC 探针工具 `uvc_cap`（已编译 ARM 版，直接 push 到平板用）与源码 |
| `docs/` | 逆向报告 + 采集诊断两份原始记录（完整证据链） |
| `cache/` | defconfig 参考副本；工具链压缩包不入库，CI 现下 |


---

## 2. Ubuntu 侧准备

| 项 | 要求 |
|---|---|
| 系统 | **Ubuntu 20.04 或 22.04**（推荐）；24.04 能编，但要按第 6 节处理 host gcc |
| 磁盘 | ≥ 20 GB（源码解包 3 GB + 构建产物 2 GB + 余量） |
| 内存 | ≥ 8 GB（`-j4` 时峰值约 4 GB） |
| 网络 | 首次需下载内核源码约 200 MB |
| 权限 | 能用 sudo |

不需要装 Docker，不需要 WSL。

---

## 3. 三步出内核

```bash
# 0) 把整个目录拷到 Ubuntu，比如 ~/zs600b-kit
cd ~/zs600b-kit

# 1) 环境搭建（apt + 工具链 + 源码，10~30 分钟看网速）
bash setup_ubuntu.sh
source ~/zs600b/env.sh        # 之后每个新终端都要 source 一次

# 2) 编译（首次全量约 10~20 分钟，8 核）
bash build_kernel.sh
```

产物：

```
out/arch/arm/boot/zImage      新内核
out/System.map                符号表（下一步用来体检）
out/.config                   最终生效的配置
```

### 体检配置覆盖度（强烈建议，别跳）

```bash
python3 verify_kallsyms.py --map out/System.map --stock artifacts/ks_stock.txt
```

输出会把「原机有、新内核没有」的符号按子系统归类。判断标准：

- **只有「其他」类有缺**（几百个以内）→ 正常，多为编译器差异，可以直接刷。
- **USB / 显示 / eMMC / WIFI 任一类有缺** → 先补配置再刷，否则可能起不来或外设全废。
  按提示在 `build_kernel.sh` 的 `scripts/config` 段补 `-e CONFIG_xxx`，重跑。

### 打包成 boot.img

```bash
python3 repack_boot.py --base artifacts/boot_device.img \
                       --kernel out/arch/arm/boot/zImage \
                       --out out/boot-new.img
```

脚本会打印新布局并断言 ramdisk、second(dtb) 未被破坏。内核变大导致 ramdisk 偏移变化会自动重排，
不用担心。

---

## 4. 刷进去 / 刷回来

平板上开着 USB 调试，数据线接 Ubuntu（先确认 `adb devices` 能看到 `1TSB3T3IFS`）。

```bash
bash flash_boot.sh check                  # 看设备 + 当前 boot 头
bash flash_boot.sh flash out/boot-new.img # 刷入（要手打 FLASH 确认）
bash flash_boot.sh rollback               # 出问题刷回原厂状态
```

`flash` 子命令做的事：**自动备份当前 p5 → 推送 → dd 到 `/dev/block/mmcblk0p5` → 回读算 sha256 比对 → 通过才问要不要重启**。校验不过会直接把回滚命令打给你。

刷写只碰 p5。p4（kernel 分区）实测全 0，不用动；uboot 带 `verify=n`，boot.img 无签名，所以改了能过。

### 起不来怎么办

1. 长按电源 10 秒强制关机。
2. 开机时**不要**插 USB 线，正常开机等 60 秒 —— 若能进系统，说明只是内核参数问题，`adb push` 回滚镜像再刷。
3. 进不了系统：关机后按住音量键/复位孔进 maskrom（设备管理里会多出一个 Rockusb 设备），用 `upgrade_tool` 或 `rkflashtool` 烧回 `artifacts/boot_device.img`。
4. 极端情况官方完整固件也能救（此前已有完整救砖复盘记录）。

---

## 5. 三个里程碑（按顺序来，别跳级）

| 阶段 | 做什么 | 通过标准 |
|---|---|---|
| **M1 复现** | 原样 `rockchip_defconfig` 编译 + 打包 + 刷入，一行代码不改 | 正常开机、`adb shell uname -a` 是自己的构建时间、WIFI/触摸/HDMI 都正常、采集卡**仍然黑屏**（对照） |
| **M2 对齐** | 按 `verify_kallsyms.py` 报告补齐驱动，直到关键子系统零缺失 | 功能与 M1 完全一致，符号差异收敛到「其他」 |
| **M3 修复** | 改 `drivers/usb/dwc_otg_310/` 的 ISO 调度，验证采集卡出图 | 采集卡有画面，或至少 `0xAA` 哨兵法显示缓冲区被写 |

M1 是最关键的一步：**先证明编译链和刷写链是通的**，再去动驱动。M1 不通就别改代码，否则分不清是新代码的锅还是工具链的锅。

M3 的排查入口（之前已定位到现象，代码级还没落点）：

- `dwc_otg_hcd_ddma.c` 的 `init_isoc_dma_desc()`：等时描述符链构造，重点看 `frames`/`npackets` 与微帧对齐
- `dwc_otg_hcd.c` 的 `dwc_otg_hcd_start_xfer_dma()` / `dwc_otg_hcd_urb_enqueue()`：HCDMA 与 HCTSIZ 下发的缓冲区地址和长度
- `dwc_otg_hcd_intr.c` 的 ISO 完成回调：`#Iso` 计数为负说明 release 多于 alloc，很可能在这里重复归还
- 现场验证工具：`refsrc` 同目录父级的 `hdmi/uvc_cap.c`（0xAA 哨兵探针），刷完内核后直接重跑

---

## 6. 已知坑

| 现象 | 原因 | 处理 |
|---|---|---|
| `multiple definition of ...`（编 host 工具时） | host gcc ≥10 默认 `-fno-common` | `build_kernel.sh` 已加 `HOSTCFLAGS=...-fcommon`，若仍报就再补 `-fno-common` 之外的老式声明 |
| `implicit declaration of function` 报错 | gcc 14 起隐式声明为 error | 脚本已加 `-Wno-error=implicit-function-declaration`；24.04 上建议 `sudo apt install gcc-12` 后 `CC=gcc-12 HOSTCC=gcc-12` |
| `make: *** mixed implicit and normal rules` | make ≥4 与老 Makefile 语法冲突 | 极少数文件触发，按报错行把 `a b: c` 拆成两条规则 |
| `No rule to make target 'arch/arm/boot/zImage'` | 忘记 `ARCH=arm` / `CROSS_COMPILE` | 先 `source ~/zs600b/env.sh` |
| 工具链 `cannot execute binary file` | 下错了 32 位或 aarch64 版本 | 必须是 `bin/x86_64/.../x86_64-gcc-4.9.4-nolibc-arm-linux-gnueabi.tar.xz` |
| 编出的 zImage 刷完不开机但能进 recovery | 配置缺显示/存储驱动 | 回到 `verify_kallsyms.py`，把缺失子系统补上 |
| `repack_boot.py` 报「超出分区容量」 | 内核 > 约 32 MB | 关掉 `CONFIG_DEBUG_INFO`、改用 `CONFIG_KERNEL_LZ4` 压得更小 |

编译时间参考：8 核 / SSD 全量约 10–20 分钟；改一个 `.c` 增量约 1–2 分钟。

---

## 7. GitHub Actions 云端编译（主路线）

工作流：`.github/workflows/build-kernel.yml`，运行环境 `ubuntu-22.04`。

### 触发

仓库 → **Actions** → 左侧 `build-zs600b-kernel` → **Run workflow**，三个输入：

| 输入 | 默认 | 说明 |
|---|---|---|
| `defconfig` | `rockchip_defconfig` | 一般不用改 |
| `extra_config` | `true` | `true` = 额外打开 `USB20_HOST/USB20_OTG/USB_VIDEO_CLASS/DEBUG_FS/IKCONFIG`（M2 用）；`false` = 完全原样 defconfig（M1 验证编译链用） |
| `kernel_ref` | `release-3.10` | 内核分支，换版本才动 |

### 它做了什么

1. 装 build-essential / bison / flex / bc / perl 等
2. 下 kernel.org 的 **gcc 4.9.4 nolibc arm-linux-gnueabi** 工具链（三个镜像轮询）
3. 拉 `rockchip-linux/kernel@release-3.10`（用 Actions cache 缓存，第二次起跳过下载）
4. `make <defconfig>` →（可选）追加配置 → `olddefconfig` → `make zImage`
5. `verify_kallsyms.py` 对 63525 个原机符号做缺失体检，报告直接贴到 run 的 Summary
6. `repack_boot.py` 以 `artifacts/boot_device.img` 为基底打包出 `boot-new.img`
7. 上传 artifact，保留 30 天

### 产物

```
zImage               新内核
boot-new.img         可直接刷的 boot 分区镜像
System.map           符号表
kernel.config        最终生效配置（关键项会打印在 Summary 里）
kallsyms-report.txt  缺失符号按子系统归类
```

在 run 页面底部 **Artifacts** 区下载 zip。失败时也会上传 `kernel.config` 便于定位配置问题。

### 拿到产物之后

本机只需要 `adb`：

```bash
bash flash_boot.sh check
bash flash_boot.sh flash boot-new.img    # 要手打 FLASH 确认
bash flash_boot.sh rollback              # 出问题刷回来
```

### 关于云端 gcc 版本

内核本身用随包的 gcc 4.9.4 交叉编译，与源码年代匹配（3.10 有 `compiler-gcc4.h` 分支）；
只有编译 host 工具（`scripts/` 下的 modpost、kallsyms 等）用的是 runner 自带的 gcc 11。
这些地方已通过 `HOSTCFLAGS` 关掉全部 `-Werror` 并加 `-fcommon`。若仍报错，把该条报错贴进 issue，
按第 6 节思路补 `-Wno-error=xxx` 即可。


---

## 8. 事实速查

```
平板型号      ZS600B (DJI CrystalSky)，RK3288，Android 5.1.1，已 root，序列号 1TSB3T3IFS
boot 分区     /dev/block/mmcblk0p5，32 MiB
page_size     16384        kernel 5137568B  ramdisk 1337812B  second 335872B
重打包基底    artifacts/boot_device.img（设备当前状态，含 root 后的 ramdisk）
校验          boot.img 无签名，U-Boot verify=n → 可自由替换
配置基线      rockchip_defconfig（CONFIG_KERNEL_LZO=y 与 zImage 里的 LZO 流吻合）
符号基线      artifacts/ks_stock.txt，63525 个符号
```

更详细的过程与证据链见 `docs/ZS600B_内核逆向报告.md`、`docs/ZS600B_HDMI采集诊断.md`。
