#!/usr/bin/env bash
# ============================================================================
# ZS600B (DJI CrystalSky / RK3288) 内核编译环境一键搭建 —— 在 Ubuntu 上执行
# 用法：  bash setup_ubuntu.sh            # 默认装到 ~/zs600b
#         ROOT=/opt/zs600b bash setup_ubuntu.sh
# 建议系统：Ubuntu 20.04 / 22.04（24.04 见 README 的 host gcc 注意事项）
# ============================================================================
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT="${ROOT:-$HOME/zs600b}"
KSRC="$ROOT/src/kernel"
TCDIR="$ROOT/toolchain"
CACHE="$ROOT/cache"
TC_NAME="x86_64-gcc-4.9.4-nolibc-arm-linux-gnueabi"
TC_TARBALL="$TC_NAME.tar.xz"
KVER="release-3.10"

msg() { printf '\n\033[1;36m[*] %s\033[0m\n' "$*"; }
die() { printf '\n\033[1;31m[!] %s\033[0m\n' "$*" >&2; exit 1; }

[[ $EUID -eq 0 ]] && echo "[!] 建议用普通用户 + sudo 运行，不要直接 root。"

mkdir -p "$ROOT" "$TCDIR" "$CACHE" "$ROOT/src" "$ROOT/out"

# ---------------------------------------------------------------- 1. 依赖包
msg "1/4 安装编译依赖（需要 sudo）"
sudo apt-get update -y
sudo apt-get install -y --no-install-recommends \
  build-essential git curl ca-certificates xz-utils lzop lz4 \
  bison flex libssl-dev libelf-dev bc \
  python3 python3-pip rsync unzip zip \
  libncurses5-dev device-tree-compiler kmod usbutils android-tools-adb \
  || die "依赖安装失败，请检查 apt 源"

for c in make gcc; do
  command -v "$c" >/dev/null || die "缺少 $c"
done
echo "    host gcc: $(gcc -dumpversion)   make: $(make -v | head -1 | awk '{print $3}')"

# ---------------------------------------------------------------- 2. 工具链
msg "2/4 准备 ARM 交叉工具链 gcc 4.9.4 (nolibc, x86_64 host)"
TC_BIN="$TCDIR/gcc-4.9.4-nolibc/arm-linux-gnueabi/bin/arm-linux-gnueabi-gcc"
if [[ -x "$TC_BIN" ]]; then
  echo "    已存在，跳过下载"
else
  # 迁移包自带的工具链先用（省一次下载）
  if [[ ! -s "$CACHE/$TC_TARBALL" && -s "$HERE/cache/$TC_TARBALL" ]]; then
    echo "    使用迁移包内自带的工具链： $HERE/cache/$TC_TARBALL"
    cp "$HERE/cache/$TC_TARBALL" "$CACHE/$TC_TARBALL"
  fi
  if [[ ! -s "$CACHE/$TC_TARBALL" ]]; then
    for U in \
      "https://mirrors.ustc.edu.cn/kernel.org/tools/crosstool/files/bin/x86_64/4.9.4/$TC_TARBALL" \
      "https://mirrors.edge.kernel.org/pub/tools/crosstool/files/bin/x86_64/4.9.4/$TC_TARBALL" ; do
      echo "    下载： $U"
      if curl -fL --retry 3 --connect-timeout 20 -o "$CACHE/$TC_TARBALL.part" "$U"; then
        mv "$CACHE/$TC_TARBALL.part" "$CACHE/$TC_TARBALL"; break
      fi
      rm -f "$CACHE/$TC_TARBALL.part"
    done
  fi
  [[ -s "$CACHE/$TC_TARBALL" ]] || die "工具链下载失败，请手动下载放到 $CACHE/"
  tar -xJf "$CACHE/$TC_TARBALL" -C "$TCDIR"
  [[ -x "$TC_BIN" ]] || die "解压后找不到 $TC_BIN"
fi

# 该工具链的 cc1 动态链接 libmpfr.so.4（MPFR 3.x 的 SONAME）。
# Ubuntu 20.04+ 只提供 libmpfr.so.6，不补这个库会直接报：
#   cc1: error while loading shared libraries: libmpfr.so.4
# libgmp.so.10 / libmpc.so.3 系统自带，无需处理。
msg "2b/4 补齐工具链依赖 libmpfr.so.4"
MPFR_DIR="$TCDIR/libs/usr/lib/x86_64-linux-gnu"
if [[ -f "$MPFR_DIR/libmpfr.so.4" ]]; then
  echo "    已存在，跳过"
elif [[ "$(uname -m)" != "x86_64" ]]; then
  echo "    [!] 主机不是 x86_64（$(uname -m)），本包工具链是 x86_64 host，无法使用"
else
  DEB="$CACHE/libmpfr4_3.1.4-1_amd64.deb"
  if [[ ! -s "$DEB" ]]; then
    for U in \
      "http://archive.ubuntu.com/ubuntu/pool/main/m/mpfr4/libmpfr4_3.1.4-1_amd64.deb" \
      "http://security.ubuntu.com/ubuntu/pool/main/m/mpfr4/libmpfr4_3.1.4-1_amd64.deb" ; do
      echo "    下载： $U"
      if curl -fL --retry 3 --connect-timeout 20 -o "$DEB.part" "$U"; then mv "$DEB.part" "$DEB"; break; fi
      rm -f "$DEB.part"
    done
  fi
  [[ -s "$DEB" ]] || die "libmpfr4 下载失败，请手动放到 $CACHE/"
  mkdir -p "$TCDIR/libs"
  dpkg -x "$DEB" "$TCDIR/libs"
  ls -l "$MPFR_DIR"/libmpfr.so.4* | sed 's/^/    /'
fi

export LD_LIBRARY_PATH="$MPFR_DIR:${LD_LIBRARY_PATH:-}"
"$TC_BIN" --version | head -1
# 真正驱动一次 cc1：--version 只跑 gcc 驱动，碰不到 cc1，测不出库是否齐全
echo 'int main(void){return 0;}' > "$CACHE/.probe.c"
"$TC_BIN" -c "$CACHE/.probe.c" -o "$CACHE/.probe.o" || die "cc1 无法运行，工具链依赖不全"
echo "    cc1 自检通过"
rm -f "$CACHE/.probe.c" "$CACHE/.probe.o"

# ---------------------------------------------------------------- 3. 内核源码
msg "3/4 准备内核源码 $KVER"
if [[ -f "$KSRC/Makefile" ]]; then
  echo "    已存在，跳过： $KSRC"
else
  KTAR="$CACHE/kernel-$KVER.tar.gz"
  if [[ ! -s "$KTAR" ]]; then
    for U in \
      "https://codeload.github.com/rockchip-linux/kernel/tar.gz/refs/heads/$KVER" \
      "https://github.com/rockchip-linux/kernel/archive/refs/heads/$KVER.tar.gz" ; do
      echo "    下载： $U"
      if curl -fL --retry 3 --connect-timeout 20 -o "$KTAR.part" "$U"; then
        mv "$KTAR.part" "$KTAR"; break
      fi
      rm -f "$KTAR.part"
    done
  fi
  [[ -s "$KTAR" ]] || die "内核源码下载失败"
  echo "    解包（约 200MB，稍等）..."
  tar -xzf "$KTAR" -C "$ROOT/src"
  mv "$ROOT/src/kernel-$KVER" "$KSRC"
fi
head -4 "$KSRC/Makefile" | sed 's/^/    /'

# ---------------------------------------------------------------- 4. 环境脚本
msg "4/4 生成环境变量脚本 $ROOT/env.sh"
cat > "$ROOT/env.sh" <<EOF
# 由 setup_ubuntu.sh 生成，用前 source 一下： source $ROOT/env.sh
export ROOT="$ROOT"
export KSRC="$KSRC"
export CROSS_COMPILE="$TCDIR/gcc-4.9.4-nolibc/arm-linux-gnueabi/bin/arm-linux-gnueabi-"
export ARCH=arm
export PATH="\$(dirname "\$CROSS_COMPILE"):\$PATH"
# cc1 需要 MPFR 3.x 的 libmpfr.so.4，系统只有 .so.6，用这里补的
export LD_LIBRARY_PATH="$TCDIR/libs/usr/lib/x86_64-linux-gnu\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
EOF
cat "$ROOT/env.sh" | sed 's/^/    /'

printf '\n\033[1;32m[+] 环境就绪\033[0m\n'
echo "    下一步：  source $ROOT/env.sh"
echo "              bash build_kernel.sh"
