#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
新内核 vs 原机内核 符号体检

原机 kallsyms（artifacts/ks_stock.txt）是设备上 /proc/kallsyms 的完整导出，
等于「原厂内核编译时启用了哪些驱动/子系统」的白名单。
新编内核的 System.map 里若缺了某类符号，说明对应驱动没编进去 —— 刷机后轻则外设失灵，
重则起不来。上机前先把这个差异压到最小。

用法：
  python3 verify_kallsyms.py --map out/System.map --stock artifacts/ks_stock.txt
  python3 verify_kallsyms.py --map out/System.map --stock artifacts/ks_stock.txt --all
"""

import argparse
import re
import sys
from collections import Counter, OrderedDict

# 子系统关键字 -> 缺失符号归类（顺序即报告顺序，越靠前越致命）
CATEGORIES = OrderedDict([
    ("USB 主机/OTG",      r"dwc_otg|usb20|usb_hcd|ehci|ohci|xhci|dwc2"),
    ("USB 设备类驱动",     r"uvc|uvcvideo|usb_storage|usbhid|usb_serial|usbnet|rtl8|rtl28|cdc_"),
    ("V4L2 / 摄像头",      r"v4l2|videobuf|vb2_|media_|soc_camera"),
    ("显示 / HDMI",        r"rockchip_fb|rk_fb|hdmi|edid|drm_|lcdc|vop|backlight|panel"),
    ("GPU (Mali)",         r"kbase|mali|kds_"),
    ("WIFI / 蓝牙",        r"rk_wifi|rkwifi|bcmdhd|ap6|wlan|cfg80211|bluez|rfkill"),
    ("eMMC / 存储",        r"dw_mmc|mmc_|sdhci|nand|flash"),
    ("触摸 / 输入",        r"gt9|goodix|ft5|rockchip_i2c|input_|ts_|touch"),
    ("音频",              r"rt56|es8|codec|i2s|pcm|snd_|spdif"),
    ("电源 / 充电 / PMIC", r"rk8|regulator|charger|pmu|act8|fan53"),
    ("I2C / SPI / GPIO",   r"i2c_|spi_|gpio|pinctrl|pwm"),
    ("DVFS / 温控",        r"dvfs|thermal|cpu_freq|rockchip_cpufreq|opp"),
    ("传感器",             r"accel|gyro|light_|sensor|mxc"),
    ("视频编解码",         r"vpu|hevc|h264|vdec|venc|rga|iep"),
    ("文件系统",           r"ext4|f2fs|squashfs|vfat|fuse|exfat"),
    ("网络",              r"tcp_|ip_|net_|sk_|eth|phy_"),
])


def load_map(path):
    """解析 System.map / kallsyms：每行 '地址 类型 名字'"""
    syms = {}
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            parts = line.split()
            if len(parts) < 3:
                continue
            addr, typ, name = parts[0], parts[1], parts[2]
            syms[name] = (addr, typ)
    return syms


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--map", required=True, help="新内核 out/System.map")
    ap.add_argument("--stock", required=True, help="原机 kallsyms 导出")
    ap.add_argument("--all", action="store_true", help="列出全部缺失符号（默认只列每类前 8 个）")
    ap.add_argument("--limit", type=int, default=8)
    args = ap.parse_args()

    new = load_map(args.map)
    stock = load_map(args.stock)
    print("[i] 新内核符号 %d 个；原机符号 %d 个" % (len(new), len(stock)))

    missing = [n for n in stock if n not in new]
    extra = [n for n in new if n not in stock]
    print("[i] 原机有、新内核没有：%d 个（%.1f%%）" % (len(missing), 100.0 * len(missing) / max(1, len(stock))))
    print("[i] 新内核有、原机没有：%d 个" % len(extra))

    if not missing:
        print("\n[+] 零缺失：配置与驱动覆盖度与原厂一致。")
        return 0

    # 归类
    buckets = OrderedDict((k, []) for k in CATEGORIES)
    buckets["其他"] = []
    for name in missing:
        low = name.lower()
        for cat, pat in CATEGORIES.items():
            if re.search(pat, low):
                buckets[cat].append(name)
                break
        else:
            buckets["其他"].append(name)

    print("\n=== 缺失符号按子系统归类 ===")
    print("%-20s %6s" % ("子系统", "缺失数"))
    print("-" * 28)
    for cat, names in buckets.items():
        if names:
            print("%-20s %6d" % (cat, len(names)))

    print("\n=== 重点：非「其他」类明细 ===")
    fatal = 0
    for cat, names in buckets.items():
        if not names or cat == "其他":
            continue
        show = names if args.all else names[:args.limit]
        print("\n[%s] 共 %d 个%s" % (cat, len(names), "" if args.all or len(names) <= args.limit else "，列前 %d 个" % args.limit))
        for n in sorted(show):
            print("    - %s" % n)
        fatal += len(names)

    if fatal == 0:
        print("\n[+] 关键子系统没有缺失，缺的都在「其他」里（多为编译器/内核小版本差异）。")

    if args.all and buckets["其他"]:
        print("\n[其他] 共 %d 个：" % len(buckets["其他"]))
        for n in sorted(buckets["其他"]):
            print("    %s" % n)

    print("\n提示：缺失集中在某子系统时，去 arch/arm/configs/rockchip_defconfig 里搜对应 CONFIG_，"
          "\n      在 build_kernel.sh 的 scripts/config 片段里补上 -e，再重新构建。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
