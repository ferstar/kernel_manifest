#!/bin/bash
#
# 自动化内核编译打包脚本
# 支持功能：
# - LZ4 / ZSTD 优化补丁
#

# 严格模式设置
set -euo pipefail
IFS=$'\n\t'

#####################################################################
# 初始化设置
#####################################################################

# 设置工作目录
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKDIR="${SCRIPT_DIR}/workspace"
mkdir -p "$WORKDIR"
cd "$WORKDIR"

#####################################################################
# 配置参数
#####################################################################

# 基础配置参数
readonly MANIFEST=${MANIFEST:-gt5pro}
readonly CUSTOM_SUFFIX=${CUSTOM_SUFFIX:-android16-11-o-v$(date +%Y%m%d)}
readonly APPLY_DROIDSPACES=${APPLY_DROIDSPACES:-y}
readonly COMMON_REV="e76bfb318e6c2abb2a888aa1a8cfa9488715d9b7"
readonly SKIP_DEPS=${SKIP_DEPS:-y}

# 显示配置信息
echo
echo "============== 配置信息 =============="
echo "适用机型          : $MANIFEST"
echo "自定义内核后缀    : -$CUSTOM_SUFFIX"
echo "应用 DroidSpaces  : $APPLY_DROIDSPACES"
echo "跳过依赖安装      : $SKIP_DEPS"
echo "====================================="
echo

# 构建依赖安装函数
install_dependencies() {
  echo ">>> 安装构建依赖..."
  # 基础依赖包
  local base_deps="curl bison flex make binutils dwarves git lld pahole zip perl make gcc python3 python-is-python3 bc libssl-dev libelf-dev ccache"
  
  # 如果 SKIP_DEPS 未设置或为 n，则安装依赖
  if [[ "${SKIP_DEPS:-n}" =~ ^[Nn]$ ]]; then
    sudo apt-get update
    sudo apt-get install -y $base_deps
  else
    echo ">>> 跳过依赖安装"
  fi

  # 检查并安装 Clang 工具链
  if [ ! -d "$WORKDIR/toolchains/clang" ]; then
    echo ">>> 下载 Clang 工具链..."
    mkdir -p "$WORKDIR/toolchains"
    git clone --depth 1 \
      https://gitlab.com/crdroidandroid/android_prebuilts_clang_host_linux-x86_clang-r547379.git \
      "$WORKDIR/toolchains/clang"
  else
    echo ">>> Clang 工具链已存在，跳过下载。"
  fi
}

# 执行依赖安装
install_dependencies
# 设置 Clang 工具链路径
export PATH="$WORKDIR/toolchains/clang/bin:$PATH"
export CLANG_TRIPLE="aarch64-linux-gnu-"
export CROSS_COMPILE="${CLANG_TRIPLE}"
#####################################################################
# 仓库初始化
#####################################################################

echo ">>> 初始化仓库..."

# 创建工作目录
mkdir -p kernel_ws
cd kernel_ws

# 克隆或更新厂商源码
echo ">>> 克隆/更新 vendor 仓库..."
if [ -d "vendor" ]; then
  cd vendor
  git reset --hard HEAD
  git clean -fdx
  git fetch origin
  git checkout master || git checkout -b master origin/master
  git pull
  cd ..
else
  git clone --depth 1 --branch master \
    https://github.com/realme-kernel-opensource/realme_GT5pro-AndroidB-vendor-source.git \
    vendor
fi
for dir in vendor/*; do
  if [ -d "../$dir" ]; then
  echo ">>> 清理旧的 $dir 目录..."
    rm -rf "../$dir"
  fi
done
cp -rf vendor/* ..

# 克隆通用源码
echo ">>> 克隆/更新 common 仓库..."
if [ -d "common" ]; then
  cd common
  git reset --hard HEAD
  git clean -fdx
  git fetch --depth 1 origin "$COMMON_REV"
  git checkout "$COMMON_REV"
  cd ..
else
  git clone --depth 1 --branch master \
    https://github.com/realme-kernel-opensource/realme_GT5pro-AndroidB-common-source.git \
    common
  (cd common && git fetch --depth 1 origin "$COMMON_REV" && git checkout "$COMMON_REV")
fi

echo ">>> 初始化仓库完成"

#####################################################################
# 版本设置和 KernelSU 配置
#####################################################################

# 清理 ABI 文件并处理版本信息
echo ">>> 正在清除 ABI 文件及去除 dirty 后缀..."
rm -f common/android/abi_gki_protected_exports_* 

# 版本字符串处理
echo ">>> 处理版本字符串..."
sed -i 's/ -dirty//g' common/scripts/setlocalversion
sed -i '$i res=$(echo "$res" | sed '\''s/-dirty//g'\'')' common/scripts/setlocalversion
sed -i "\$s|echo \"\\\$res\"|echo \"-${CUSTOM_SUFFIX}\"|" common/scripts/setlocalversion

# 补丁应用
echo ">>> 应用 patches 目录下的 lz4 / zstd 补丁..."
cd "$WORKDIR/kernel_ws/common"
git apply "$SCRIPT_DIR/patches/001-lz4.patch"
git apply "$SCRIPT_DIR/patches/002-zstd.patch"
if [[ "$APPLY_DROIDSPACES" =~ ^[Yy]$ ]]; then
  echo ">>> 应用 DroidSpaces kABI 补丁..."
  patch -p1 < "$SCRIPT_DIR/patches/001.GKI-below-6.12-fix_sysvipc_kabi_6_7_8.patch"
fi
cd "$WORKDIR/kernel_ws"

# 内核配置
if [[ "$APPLY_DROIDSPACES" =~ ^[Yy]$ ]]; then
  echo ">>> 配置 DroidSpaces 内核选项..."
  for cfg in CONFIG_SYSVIPC CONFIG_POSIX_MQUEUE CONFIG_IPC_NS CONFIG_PID_NS CONFIG_DEVTMPFS CONFIG_NETFILTER_XT_MATCH_ADDRTYPE; do
    if grep -q "^# ${cfg} is not set" "$WORKDIR/kernel_ws/common/arch/arm64/configs/gki_defconfig"; then
      sed -i "s/^# ${cfg} is not set/${cfg}=y/" "$WORKDIR/kernel_ws/common/arch/arm64/configs/gki_defconfig"
    elif ! grep -q "^${cfg}=" "$WORKDIR/kernel_ws/common/arch/arm64/configs/gki_defconfig"; then
      echo "${cfg}=y" >> "$WORKDIR/kernel_ws/common/arch/arm64/configs/gki_defconfig"
    fi
  done
fi
#####################################################################

echo ">>> 配置内核选项..."
DEFCONFIG_FILE=./common/arch/arm64/configs/gki_defconfig

# 定义基础 SUSFS/KSU 配置
# 配置 ccache
#####################################################################

# 配置 ccache
echo ">>> 配置 ccache..."
if command -v ccache >/dev/null 2>&1; then
  export CCACHE_DIR="${HOME}/.ccache"
  export CCACHE_COMPRESS=1     # 启用压缩
  export CCACHE_COMPRESSLEVEL=5   # 压缩级别
  export CCACHE_MAXSIZE=20G   # 缓存大小上限
  mkdir -p "${CCACHE_DIR}"
  CC_WRAPPER="ccache "
else
  echo ">>> 未检测到 ccache，直接使用 clang"
  CC_WRAPPER=""
fi

#####################################################################
# 内核编译
#####################################################################

# 预处理配置
echo ">>> 禁用 defconfig 检查..."
sed -i 's/check_defconfig//' ./common/build.config.gki

echo ">>> 设置版本后缀..."
sed -i "\$s|echo \"\\\$res\"|echo \"-${CUSTOM_SUFFIX}\"|" "./common/scripts/setlocalversion"

cd "$WORKDIR/kernel_ws/common"
# 检测机器内存是否大于 16GB，如果是，则在`/tmp`目录创建一个`out`目录，软链接到`out`目录
[ -d "./out" ] && rm -rf ./out
if [[ "$(grep MemTotal /proc/meminfo | awk '{print $2}')" -gt 16777216 ]]; then
    echo ">>> 检测到大于 16GB 内存，创建 /tmp/out 软链接..."
    [ -d "/tmp/out" ] && rm -rf /tmp/out
    mkdir -p /tmp/out
    ln -s /tmp/out ./out
else
    echo ">>> 内存小于等于 16GB，使用默认的 out 目录..."
    mkdir -p ./out
fi


# 编译内核
echo ">>> 开始编译内核..."
make -j"$(nproc --all)" \
    LLVM=1 \
    LLVM_IAS=1 \
    ARCH=arm64 \
    SUBARCH=arm64 \
    KBUILD_BUILD_USER=ferstar \
    KBUILD_BUILD_HOST=ferstar.org \
    CC="${CC_WRAPPER}clang" \
    CXX="${CC_WRAPPER}clang++" \
    O=out \
    CONFIG_LTO_CLANG=y \
    CONFIG_LTO_CLANG_THIN=y \
    CONFIG_LTO_CLANG_FULL=n \
    CONFIG_LTO_NONE=n \
    gki_defconfig all 2>&1 | tee out/kernel.log

echo ">>> 内核编译成功！"

OUT_DIR="$WORKDIR/kernel_ws/common/out/arch/arm64/boot"

#####################################################################
# 打包发布
#####################################################################

# 准备 AnyKernel3
cd "$WORKDIR/kernel_ws"
echo ">>> 准备 AnyKernel3 环境..."
if [ -d "AnyKernel3/.git" ]; then
  (cd AnyKernel3 && git reset --hard HEAD && git clean -fdx && git pull)
else
  rm -rf AnyKernel3
  git clone --depth=1 https://github.com/Kernel-SU/AnyKernel3
fi

# 复制内核镜像
echo ">>> 复制内核镜像..."
cp "$OUT_DIR/Image" ./AnyKernel3/

cd "$WORKDIR/kernel_ws/AnyKernel3"
sed -i -E 's/(kernel.string=).*/\1SukiSU by SukiSU Developers, compiled by ferstar with ♥️/' anykernel.sh

# 生成发布包名称
ZIP_NAME="ak3-${MANIFEST}-${CUSTOM_SUFFIX}"

# 添加功能标识
ZIP_NAME="${ZIP_NAME}.zip"
# 打包文件
echo ">>> 创建刷机包: $ZIP_NAME"
zip -r "../$ZIP_NAME" ./* -x ".git/*"

# 输出结果
ZIP_PATH="$(realpath "../$ZIP_NAME")"
echo
echo "============== 构建完成 =============="
echo "刷机包位置: $ZIP_PATH"
echo "====================================="
