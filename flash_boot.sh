#!/usr/bin/env bash
# ============================================================================
# ZS600B boot 分区刷写 / 备份 / 回滚（走 adb + root dd，不需要进 maskrom）
#
#   bash flash_boot.sh check                # 看设备和当前 boot 分区
#   bash flash_boot.sh dump  [输出文件]      # 备份当前 boot 分区
#   bash flash_boot.sh flash <boot.img>     # 刷入（要手动输入 FLASH 确认）
#   bash flash_boot.sh rollback             # 恢复成 artifacts/boot_device.img
#
# 风险：写错分区会起不来。本脚本只碰 /dev/block/mmcblk0p5（boot），
#       写前自动备份到 out/boot-before-flash-时间戳.img，写后自动回读校验。
# ============================================================================
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BOOT_PART=/dev/block/mmcblk0p5
BOOT_BLOCKS=32768           # 32MiB
BS=4096
STOCK="$HERE/artifacts/boot_device.img"
BACKUP_DIR="$HERE/out"

export MSYS2_ARG_CONV_EXCL='*'   # Git Bash 下别把 /data/... 转换成 Windows 路径

die() { printf '\033[1;31m[!] %s\033[0m\n' "$*" >&2; exit 1; }
msg() { printf '\033[1;36m[*] %s\033[0m\n' "$*"; }

need_adb() {
  command -v adb >/dev/null || die "找不到 adb，请把 platform-tools 加进 PATH"
  adb get-state 2>/dev/null | grep -q device || die "设备未就绪（adb devices 看一下）"
}

cmd_check() {
  need_adb
  msg "设备"
  adb devices -l | sed -n '2p'
  printf '    内核: '; adb shell "cat /proc/version" 2>/dev/null | tr -d '\r'
  msg "boot 分区头部"
  mkdir -p "$BACKUP_DIR"
  local hdr="$BACKUP_DIR/.boot_hdr.bin"
  adb exec-out "su -c 'dd if=$BOOT_PART bs=512 count=4 2>/dev/null'" > "$hdr"
  ( cd "$BACKUP_DIR" && python3 - .boot_hdr.bin <<'PY'
import struct,sys
d=open(sys.argv[1],'rb').read()
print('    magic      :',d[:8])
if d[:8]==b'ANDROID!':
    kz,ka,rz,ra,sz,sa,tg,pg=struct.unpack('<8I',d[8:40])
    print('    page       : %d'%pg)
    print('    kernel     : %d @0x%08x'%(kz,ka))
    print('    ramdisk    : %d @0x%08x'%(rz,ra))
    print('    second     : %d @0x%08x'%(sz,sa))
PY
  ) || echo "    (python3 不可用，跳过解析)"
  rm -f "$hdr"
}

cmd_dump() {
  need_adb
  local out="${1:-$BACKUP_DIR/boot-dump-$(date +%Y%m%d-%H%M%S).img}"
  mkdir -p "$(dirname "$out")"
  msg "导出 $BOOT_PART -> $out"
  adb exec-out "su -c 'dd if=$BOOT_PART bs=$BS 2>/dev/null'" > "$out"
  ls -l "$out"
  printf '    sha256: %s\n' "$(sha256sum "$out" | cut -d' ' -f1)"
}

cmd_flash_file() {
  local img="$1" label="$2"
  [[ -f "$img" ]] || die "文件不存在：$img"
  need_adb
  local size; size=$(stat -c %s "$img" 2>/dev/null || stat -f %z "$img")
  (( size <= BOOT_BLOCKS * BS )) || die "镜像 $size 字节，超过 boot 分区容量 $((BOOT_BLOCKS*BS))"

  msg "刷前备份当前 boot"
  mkdir -p "$BACKUP_DIR"
  local bak="$BACKUP_DIR/boot-before-flash-$(date +%Y%m%d-%H%M%S).img"
  adb exec-out "su -c 'dd if=$BOOT_PART bs=$BS 2>/dev/null'" > "$bak"
  printf '    已备份：%s (%s 字节)\n' "$bak" "$(stat -c %s "$bak" 2>/dev/null || stat -f %z "$bak")"

  echo
  echo "  即将写入 $label"
  echo "  文件    : $img"
  echo "  大小    : $size 字节"
  echo "  目标    : $BOOT_PART（替换 $((size/1024)) KiB）"
  read -r -p "  输入 FLASH 继续： " ans
  [[ "$ans" == "FLASH" ]] || { echo "  已取消"; exit 1; }

  msg "推送镜像"
  adb push "$img" /data/local/tmp/boot-flash.img | tail -1

  msg "写入 boot 分区"
  adb shell "su -c 'dd if=/data/local/tmp/boot-flash.img of=$BOOT_PART bs=$BS conv=fsync'" 2>&1 | tail -3
  adb shell "sync" || true

  msg "回读校验（前 $size 字节 sha256）"
  local local_sha remote_sha
  local_sha=$(head -c "$size" "$img" | sha256sum | cut -d' ' -f1)
  remote_sha=$(adb exec-out "su -c 'dd if=$BOOT_PART bs=$BS count=$(( (size+BS-1)/BS )) 2>/dev/null'" \
               | head -c "$size" | sha256sum | cut -d' ' -f1)
  echo "    本地: $local_sha"
  echo "    设备: $remote_sha"
  if [[ "$local_sha" == "$remote_sha" ]]; then
    printf '\033[1;32m[+] 校验通过\033[0m\n'
  else
    printf '\033[1;31m[!] 校验不一致！立即回滚：bash flash_boot.sh rollback %s\033[0m\n' "$bak"
    exit 1
  fi

  echo
  read -r -p "  现在重启设备？(y/N) " r
  if [[ "$r" == "y" || "$r" == "Y" ]]; then
    adb reboot || true
    echo "  已发送重启。若 3 分钟没起来：长按电源 10 秒强制关机，再开机进 recovery / maskrom 用 $bak 回滚。"
  else
    echo "  未重启。镜像已就位，下次开机生效。"
  fi
}

case "${1:-check}" in
  check)    cmd_check ;;
  dump)     shift; cmd_dump "$@" ;;
  flash)    shift; [[ $# -ge 1 ]] || die "用法：flash <boot.img>"; cmd_flash_file "$1" "新 boot 镜像" ;;
  rollback)
    shift
    local_img="${1:-$STOCK}"
    [[ -f "$local_img" ]] || die "找不到回滚镜像：$local_img"
    cmd_flash_file "$local_img" "回滚镜像（原机可启动状态）"
    ;;
  *) echo "用法：bash flash_boot.sh {check|dump|flash <img>|rollback [img]}"; exit 1 ;;
esac
