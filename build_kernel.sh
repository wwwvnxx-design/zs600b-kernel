#!/usr/bin/env bash
# ============================================================================
# 编译 ZS600B (RK3288) 3.10 内核 —— 输出 arch/arm/boot/zImage
# 前置：已运行 setup_ubuntu.sh
# 用法：
#   bash build_kernel.sh              # 完整构建（rockchip_defconfig + 片段）
#   bash build_kernel.sh -c           # 只重新编译（沿用 out/.config）
#   bash build_kernel.sh -m           # 打开 menuconfig 手工改配置
# 产物： out/arch/arm/boot/zImage  /  out/System.map
# ============================================================================
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT="${ROOT:-$HOME/zs600b}"
KSRC="${KSRC:-$ROOT/src/kernel}"
OUT="${OUT:-$HERE/out}"
JOBS="${JOBS:-$(nproc)}"

[[ -f "$ROOT/env.sh" ]] && source "$ROOT/env.sh"
: "${CROSS_COMPILE:?未找到 CROSS_COMPILE，请先运行 setup_ubuntu.sh 或 source \$ROOT/env.sh}"
export ARCH=arm

[[ -f "$KSRC/Makefile" ]] || { echo "[!] 找不到内核源码：$KSRC"; exit 1; }
mkdir -p "$OUT"

# 现代 host gcc 编 3.10：主要是 -fno-common 默认开启。
# 不要罗列 -Wno-error=<type>：gcc 对未知警告名会直接报 "no option"，各版本引入的警告名不同。
HOST_CFLAGS="-Wall -Wmissing-prototypes -Wstrict-prototypes -O2 -fomit-frame-pointer -fcommon -Wno-error"
MK=(make -C "$KSRC" O="$OUT" ARCH=arm CROSS_COMPILE="$CROSS_COMPILE"
    "HOSTCFLAGS=$HOST_CFLAGS" "KCFLAGS=-Wno-error -fno-strict-aliasing")

# rockchip 3.10 顶层 Makefile：只要 scripts/gcc-wrapper.py 存在，CC 就被它接管。
# 该脚本是 Python 2 语法，Ubuntu 20.04+ 默认无 python2，会直接 SyntaxError 中断编译。
# 它只解析 stderr 把 warning 当错误，不影响编译参数与产物，直接禁用。
if [[ -f "$KSRC/scripts/gcc-wrapper.py" ]]; then
  mv "$KSRC/scripts/gcc-wrapper.py" "$KSRC/scripts/gcc-wrapper.py.disabled"
fi

msg() { printf '\n\033[1;36m[*] %s\033[0m\n' "$*"; }

MODE="${1:-full}"

case "$MODE" in
  -m|menuconfig)
    "${MK[@]}" menuconfig
    exit 0
    ;;
  -c|continue)
    [[ -f "$OUT/.config" ]] || { echo "[!] $OUT/.config 不存在，先跑完整构建"; exit 1; }
    ;;
  *)
    msg "1/4 rockchip_defconfig"
    "${MK[@]}" rockchip_defconfig

    msg "2/4 叠加 ZS600B 配置片段"
    # 关键项：USB 双控制器 + UVC + 输入设备 + 摄像头，均为本机硬件实测需要
    # 3.10 的 scripts/config 不一定支持 --file，直接追加到 .config（后者覆盖前者）
    {
      echo ""
      echo "CONFIG_USB20_HOST=y"
      echo "CONFIG_USB20_OTG=y"
      echo "CONFIG_DWC_OTG_310=y"
      echo "CONFIG_MEDIA_SUPPORT=y"
      echo "CONFIG_MEDIA_USB_SUPPORT=y"
      echo "CONFIG_MEDIA_CAMERA_SUPPORT=y"
      echo "CONFIG_USB_VIDEO_CLASS=y"
      echo "CONFIG_VIDEO_DEV=y"
      echo "CONFIG_USB_STORAGE=y"
      echo "CONFIG_USB_HID=y"
      echo "CONFIG_SQUASHFS=y"
      echo "CONFIG_EXT4_FS=y"
      echo "CONFIG_IKCONFIG=y"
      echo "CONFIG_IKCONFIG_PROC=y"
      echo "CONFIG_DEBUG_FS=y"
      echo "CONFIG_KERNEL_LZO=y"
    } >> "$OUT/.config"
    "${MK[@]}" olddefconfig
    ;;
esac

msg "3/4 校验关键配置"
for k in CONFIG_USB20_HOST CONFIG_USB20_OTG CONFIG_USB_VIDEO_CLASS CONFIG_ARCH_ROCKCHIP; do
  v=$(grep -E "^$k=|^# $k is not set" "$OUT/.config" | head -1)
  printf '    %-26s %s\n' "$k" "${v:-<缺失>}"
  [[ -n "$v" && "$v" != *"is not set"* ]] || echo "    [!] $k 未启用，可能导致外设不工作"
done

msg "4/4 编译 zImage（$JOBS 线程）"
time "${MK[@]}" -j"$JOBS" zImage

Z="$OUT/arch/arm/boot/zImage"
SM="$OUT/System.map"
[[ -f "$Z" ]] || { echo "[!] 未生成 zImage"; exit 1; }

printf '\n\033[1;32m[+] 构建完成\033[0m\n'
ls -l "$Z" "$SM" | sed 's/^/    /'
printf '    zImage sha256: %s\n' "$(sha256sum "$Z" | cut -d' ' -f1)"
printf '    内核符号数  : %s\n' "$(wc -l < "$SM")"
printf '    原机符号数  : %s（artifacts/ks_stock.txt）\n' "$(wc -l < "$HERE/artifacts/ks_stock.txt" 2>/dev/null || echo '?')"

cat <<EOF

    下一步：
      1) 差异体检：  python3 verify_kallsyms.py --map $SM --stock $HERE/artifacts/ks_stock.txt
      2) 打 boot.img：python3 repack_boot.py --base artifacts/boot_device.img \\
                          --kernel $Z --out out/boot-new.img
      3) 刷入设备：  bash flash_boot.sh flash out/boot-new.img
EOF
