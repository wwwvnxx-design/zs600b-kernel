#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ZS600B (RK3288) boot.img 重打包 —— 只替换 kernel，保留 ramdisk / second(dtb) / header
依赖：仅 Python3 标准库

Android boot image v0 头部布局（小端）：
  0x00 magic[8] "ANDROID!"
  0x08 kernel_size    0x0C kernel_addr
  0x10 ramdisk_size   0x14 ramdisk_addr
  0x18 second_size    0x1C second_addr
  0x20 tags_addr      0x24 page_size
  0x28 dt_size        0x2C unused
  0x30 name[16]       0x40 cmdline[512]
  0x240 id[8]         0x260 extra_cmdline[1024]

本机实测：page_size = 16384，kernel@0x4000，ramdisk@0x4EC000，second@0x634000，
second 段是 RSCE 容器（内部 @0x800 处为 dtb），尾部无 AVB/签名。

用法：
  # 常规：用设备当前 boot 作基底，换成新编 zImage
  python3 repack_boot.py --base artifacts/boot_device.img --kernel out/zImage --out out/boot-new.img

  # 自检：用基底自己的 kernel 回填，输出应与基底前 N 字节完全一致
  python3 repack_boot.py --base artifacts/boot_device.img --kernel out/stock-kernel.bin \
                         --out out/roundtrip.img --check
"""

import argparse
import hashlib
import os
import struct
import sys

MAGIC = b"ANDROID!"
HDR_FMT = "<8s10I16s512s32s1024s"
HDR_SIZE = struct.calcsize(HDR_FMT)  # 1632


def align(n, page):
    return ((n + page - 1) // page) * page


class BootImage:
    def __init__(self, blob):
        if blob[:8] != MAGIC:
            raise SystemExit("[-] 不是 Android boot image（magic=%r）" % blob[:8])
        (self.magic, self.kernel_size, self.kernel_addr,
         self.ramdisk_size, self.ramdisk_addr,
         self.second_size, self.second_addr,
         self.tags_addr, self.page_size, self.dt_size,
         self.unused, self.name, self.cmdline,
         self.id_hash, self.extra_cmdline) = struct.unpack(HDR_FMT, blob[:HDR_SIZE])

        if self.page_size not in (2048, 4096, 8192, 16384, 32768, 65536):
            raise SystemExit("[-] page_size 异常：%d" % self.page_size)

        # 三段数据的实际切分按「当前头部声明的尺寸」计算（不字面照搬 0x4000 等常量）
        self.o_kernel = self.page_size
        self.o_ramdisk = self.o_kernel + align(self.kernel_size, self.page_size)
        self.o_second = self.o_ramdisk + align(self.ramdisk_size, self.page_size)
        self.o_end = self.o_second + align(self.second_size, self.page_size)

        if self.o_end > len(blob):
            raise SystemExit("[-] 头部声明的尺寸超出文件长度：end=%d file=%d" % (self.o_end, len(blob)))

        self.blob = blob
        self.kernel = blob[self.o_kernel:self.o_kernel + self.kernel_size]
        self.ramdisk = blob[self.o_ramdisk:self.o_ramdisk + self.ramdisk_size]
        self.second = blob[self.o_second:self.o_second + self.second_size]

    def describe(self):
        ps = self.page_size
        print("   magic        : %s" % self.magic.decode("ascii", "replace"))
        print("   page_size    : %d" % ps)
        print("   kernel       : size=%d  addr=0x%08x  off=0x%x" % (self.kernel_size, self.kernel_addr, self.o_kernel))
        print("   ramdisk      : size=%d  addr=0x%08x  off=0x%x" % (self.ramdisk_size, self.ramdisk_addr, self.o_ramdisk))
        print("   second       : size=%d  addr=0x%08x  off=0x%x" % (self.second_size, self.second_addr, self.o_second))
        print("   tags_addr    : 0x%08x" % self.tags_addr)
        print("   dt_size      : %d" % self.dt_size)
        print("   cmdline      : %r" % self.cmdline.split(b"\0")[0])
        print("   image end    : %d (0x%x)" % (self.o_end, self.o_end))


def build(boot, new_kernel, pad_to=0):
    """用新 kernel 重建镜像；ramdisk / second / header 其余字段原样保留。"""
    ps = boot.page_size
    hdr = bytearray(struct.pack(
        HDR_FMT, boot.magic, len(new_kernel), boot.kernel_addr,
        boot.ramdisk_size, boot.ramdisk_addr,
        boot.second_size, boot.second_addr,
        boot.tags_addr, ps, boot.dt_size,
        boot.unused, boot.name, boot.cmdline, boot.id_hash, boot.extra_cmdline))
    assert len(hdr) == HDR_SIZE

    out = bytearray()
    out += hdr
    out += b"\0" * (ps - HDR_SIZE)                      # 补齐到第一个页
    out += new_kernel
    out += b"\0" * (align(len(new_kernel), ps) - len(new_kernel))
    out += boot.ramdisk
    out += b"\0" * (align(boot.ramdisk_size, ps) - boot.ramdisk_size)
    out += boot.second
    out += b"\0" * (align(boot.second_size, ps) - boot.second_size)

    if pad_to and len(out) < pad_to:
        out += b"\0" * (pad_to - len(out))
    return bytes(out)


def sha(b):
    return hashlib.sha256(b).hexdigest()


def main():
    ap = argparse.ArgumentParser(description="ZS600B boot.img 重打包（只换 kernel）")
    ap.add_argument("--base", required=True, help="基底 boot 镜像（用设备当前 boot 分区 dump）")
    ap.add_argument("--kernel", required=True, help="新内核（arch/arm/boot/zImage）")
    ap.add_argument("--out", required=True, help="输出 boot 镜像")
    ap.add_argument("--partition-size", type=int, default=32 * 1024 * 1024,
                    help="boot 分区大小，默认 32MiB（p5=32768 块）")
    ap.add_argument("--no-pad", action="store_true", help="不补齐到分区大小")
    ap.add_argument("--check", action="store_true",
                    help="自检：要求输出前 len(base_有效区) 与基底完全一致")
    args = ap.parse_args()

    base_blob = open(args.base, "rb").read()
    boot = BootImage(base_blob)
    print("[*] 基底：%s" % args.base)
    boot.describe()

    new_kernel = open(args.kernel, "rb").read()
    print("[*] 新内核：%s  %d 字节 (0x%x)" % (args.kernel, len(new_kernel), len(new_kernel)))

    pad = 0 if args.no_pad else args.partition_size
    img = build(boot, new_kernel, pad_to=pad)

    if len(img) > args.partition_size:
        raise SystemExit("[-] 超出分区容量：%d > %d" % (len(img), args.partition_size))

    # 结构回读校验
    chk = BootImage(img)
    assert chk.kernel_size == len(new_kernel)
    assert chk.kernel == new_kernel
    assert chk.ramdisk == boot.ramdisk, "ramdisk 被破坏"
    assert chk.second == boot.second, "second(dtb) 被破坏"

    if args.check:
        n = boot.o_end
        same = img[:n] == base_blob[:n]
        print("[i] 自检 roundtrip：前 %d 字节与基底一致 = %s" % (n, same))
        if not same:
            # 定位第一处差异
            for i in range(n):
                if img[i] != base_blob[i]:
                    print("    首个差异 @0x%x: new=%02x base=%02x" % (i, img[i], base_blob[i]))
                    break
            raise SystemExit(1)

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(img)

    print("[+] 写出：%s  %d 字节" % (args.out, len(img)))
    print("    sha256 = %s" % sha(img))
    print("[*] 布局：kernel@0x%x  ramdisk@0x%x  second@0x%x" %
          (boot.page_size, boot.page_size + align(len(new_kernel), boot.page_size),
           boot.page_size + align(len(new_kernel), boot.page_size) + align(boot.ramdisk_size, boot.page_size)))


if __name__ == "__main__":
    sys.exit(main())
