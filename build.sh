#!/bin/bash
#
# 自动化内核编译打包脚本
# 支持功能：
# - KernelSU/SUSFS 支持
# - LZ4KD 压缩
# - KPM补丁
# - 风驰调度
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
readonly CUSTOM_SUFFIX=${CUSTOM_SUFFIX:-android14-11-o-v$(date +%Y%m%d)}
readonly USE_PATCH_LINUX=${USE_PATCH_LINUX:-n}
readonly APPLY_LZ4KD=${APPLY_LZ4KD:-n}
readonly APPLY_SCX=${APPLY_SCX:-n}
readonly SKIP_DEPS=${SKIP_DEPS:-y}

# 显示配置信息
echo
echo "============== 配置信息 =============="
echo "适用机型          : $MANIFEST"
echo "自定义内核后缀    : -$CUSTOM_SUFFIX"
echo "使用 patch_linux  : $USE_PATCH_LINUX"
echo "应用 lz4kd 补丁   : $APPLY_LZ4KD"
echo "应用风驰内核驱动  : $APPLY_SCX"
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
  if [[ "$APPLY_SCX" =~ ^[Yy]$ ]]; then
    git checkout scx || git checkout -b scx origin/scx
  else
    git checkout master || git checkout -b master origin/master
  fi
  git pull
  cd ..
else
  if [[ "$APPLY_SCX" =~ ^[Yy]$ ]]; then
    git clone --branch scx \
      https://github.com/ferstar/realme_GT5pro-AndroidV-vendor-source.git \
      vendor
  else
    git clone --branch master \
      https://github.com/ferstar/realme_GT5pro-AndroidV-vendor-source.git \
      vendor
  fi
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
  git fetch origin
  if [[ "$APPLY_SCX" =~ ^[Yy]$ ]]; then
    git checkout scx || git checkout -b scx origin/scx
  else
    git checkout dev || git checkout -b dev origin/dev
  fi
  git pull
  cd ..
else
  if [[ "$APPLY_SCX" =~ ^[Yy]$ ]]; then
    git clone --depth 1 --branch scx \
      https://github.com/ferstar/realme_GT5pro-AndroidV-common-source.git \
      common
  else
    git clone --depth 1 --branch dev \
      https://github.com/ferstar/realme_GT5pro-AndroidV-common-source.git \
      common
  fi
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

# 配置 KernelSU
echo ">>> 拉取 SukiSU-Ultra 并设置版本..."
# 如果 KernelSU 目录存在则删除
[ -d "KernelSU" ] && rm -rf KernelSU
curl -LSs "https://raw.githubusercontent.com/ShirkNeko/SukiSU-Ultra/main/kernel/setup.sh" | bash -s susfs-dev
cd KernelSU
KSU_VERSION="$(expr "$(git rev-list --count main)" "+" 10606)"
export KSU_VERSION
sed -i "s/DKSU_VERSION=12800/DKSU_VERSION=${KSU_VERSION}/" kernel/Makefile

#####################################################################
# 补丁应用
#####################################################################

# 克隆所需补丁仓库
echo ">>> 克隆补丁仓库..."
cd "$WORKDIR/kernel_ws"
# Clone/update susfs4ksu
if [ -d "susfs4ksu" ]; then
  cd susfs4ksu
  git reset --hard HEAD
  git clean -fdx
  git pull
  cd ..
else
  git clone https://github.com/shirkneko/susfs4ksu.git -b gki-android14-6.1 --depth=1
fi

# Clone/update SukiSU_patch
if [ -d "SukiSU_patch" ]; then
  cd SukiSU_patch
  git reset --hard HEAD
  git clean -fdx
  git pull
  cd ..
else
  git clone https://github.com/ShirkNeko/SukiSU_patch.git --depth=1
fi

# 应用 SUSFS 相关补丁
echo ">>> 应用 SUSFS 及 hook 补丁..."
# 复制补丁文件
cp ./susfs4ksu/kernel_patches/50_add_susfs_in_gki-android14-6.1.patch \
   ./SukiSU_patch/hooks/new_hooks.patch \
   ./common/

# 复制文件系统相关文件
cp -r ./susfs4ksu/kernel_patches/fs/* ./common/fs/
cp -r ./susfs4ksu/kernel_patches/include/linux/* ./common/include/linux/

# 应用补丁
cd ./common
patch -p1 < 50_add_susfs_in_gki-android14-6.1.patch || true

# 应用隐藏补丁
cp ../SukiSU_patch/69_hide_stuff.patch ./
patch -p1 -F 3 < 69_hide_stuff.patch
patch -p1 < new_hooks.patch

cd ..

# 选择性应用 LZ4KD 补丁
if [[ "$APPLY_LZ4KD" =~ ^[Yy]$ ]]; then
  echo ">>> 应用 LZ4KD 补丁..."
  cp -r ./SukiSU_patch/other/zram/lz4k/include/linux/* ./common/include/linux/
  cp -r ./SukiSU_patch/other/zram/lz4k/lib/* ./common/lib
  cp -r ./SukiSU_patch/other/zram/lz4k/crypto/* ./common/crypto
  cp ./SukiSU_patch/other/zram/zram_patch/6.1/lz4kd.patch ./common/
  cd "$WORKDIR/kernel_ws/common"
  patch -p1 -F 3 < lz4kd.patch || true
  cd "$WORKDIR/kernel_ws"
else
  echo ">>> 跳过 LZ4KD 补丁应用"
  cd "$WORKDIR/kernel_ws"
fi

#####################################################################
# 内核配置
#####################################################################

echo ">>> 配置内核选项..."
DEFCONFIG_FILE=./common/arch/arm64/configs/gki_defconfig

# 定义基础 SUSFS/KSU 配置
declare -A ksu_configs=(
    ["CONFIG_KSU"]="y"
    ["CONFIG_KSU_SUSFS_SUS_SU"]="n"
    ["CONFIG_KSU_MANUAL_HOOK"]="y"
    ["CONFIG_KSU_SUSFS"]="y"
    ["CONFIG_KSU_SUSFS_HAS_MAGIC_MOUNT"]="y"
    ["CONFIG_KSU_SUSFS_SUS_PATH"]="y"
    ["CONFIG_KSU_SUSFS_SUS_MOUNT"]="y"
    ["CONFIG_KSU_SUSFS_AUTO_ADD_SUS_KSU_DEFAULT_MOUNT"]="y"
    ["CONFIG_KSU_SUSFS_AUTO_ADD_SUS_BIND_MOUNT"]="y"
    ["CONFIG_KSU_SUSFS_SUS_KSTAT"]="y"
    ["CONFIG_KSU_SUSFS_SUS_OVERLAYFS"]="n"
    ["CONFIG_KSU_SUSFS_TRY_UMOUNT"]="y"
    ["CONFIG_KSU_SUSFS_AUTO_ADD_TRY_UMOUNT_FOR_BIND_MOUNT"]="y"
    ["CONFIG_KSU_SUSFS_SPOOF_UNAME"]="y"
    ["CONFIG_KSU_SUSFS_ENABLE_LOG"]="y"
    ["CONFIG_KSU_SUSFS_HIDE_KSU_SUSFS_SYMBOLS"]="y"
    ["CONFIG_KSU_SUSFS_SPOOF_CMDLINE_OR_BOOTCONFIG"]="y"
    ["CONFIG_KSU_SUSFS_OPEN_REDIRECT"]="y"
)

# 写入基础配置
for config in "${!ksu_configs[@]}"; do
    echo "${config}=${ksu_configs[$config]}" >> "$DEFCONFIG_FILE"
done

# 条件配置：KPM 支持
if [[ "$USE_PATCH_LINUX" =~ ^[Yy]$ ]]; then
    echo ">>> 添加 KPM 支持..."
    echo "CONFIG_KPM=y" >> "$DEFCONFIG_FILE"
fi

# 条件配置：LZ4KD 支持
if [[ "$APPLY_LZ4KD" =~ ^[Yy]$ ]]; then
    echo ">>> 添加 LZ4KD 支持..."
    declare -A lz4kd_configs=(
        ["CONFIG_ZSMALLOC"]="y"
        ["CONFIG_CRYPTO_LZ4HC"]="y"
        ["CONFIG_CRYPTO_LZ4K"]="y"
        ["CONFIG_CRYPTO_LZ4KD"]="y"
        ["CONFIG_CRYPTO_842"]="y"
    )
    
    for config in "${!lz4kd_configs[@]}"; do
        echo "${config}=${lz4kd_configs[$config]}" >> "$DEFCONFIG_FILE"
    done
else
    echo ">>> 跳过 LZ4KD 相关配置"
fi

#####################################################################
# 配置 ccache
#####################################################################

# 配置 ccache
echo ">>> 配置 ccache..."
export CCACHE_DIR="${HOME}/.ccache"
export CCACHE_COMPRESS=1     # 启用压缩
export CCACHE_COMPRESSLEVEL=5   # 压缩级别
export CCACHE_MAXSIZE=20G   # 缓存大小上限
mkdir -p "${CCACHE_DIR}"

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
    CC="ccache clang" \
    CXX="ccache clang++" \
    O=out \
    CONFIG_LTO_CLANG=y \
    CONFIG_LTO_CLANG_THIN=y \
    CONFIG_LTO_CLANG_FULL=n \
    CONFIG_LTO_NONE=n \
    gki_defconfig all 2>&1 | tee out/kernel.log

echo ">>> 内核编译成功！"

# 处理 KPM 补丁
OUT_DIR="$WORKDIR/kernel_ws/common/out/arch/arm64/boot"
if [[ "$USE_PATCH_LINUX" =~ ^[Yy]$ ]]; then
    echo ">>> 应用 KPM 补丁..."
    cd "$OUT_DIR"
    [ -f "patch_linux" ] && rm -f patch_linux
    wget https://github.com/ShirkNeko/SukiSU_KernelPatch_patch/releases/download/0.11-beta/patch_linux
    chmod +x patch_linux
    ./patch_linux
    rm -f Image
    mv oImage Image
    echo ">>> KPM 补丁应用成功"
else
    echo ">>> 跳过 KPM 补丁应用"
fi

#####################################################################
# 打包发布
#####################################################################

# 准备 AnyKernel3
cd "$WORKDIR/kernel_ws"
echo ">>> 准备 AnyKernel3 环境..."
# Cleanup existing AnyKernel3 directory if it exists
[ -d "AnyKernel3" ] && rm -rf AnyKernel3
# Clone fresh copy of AnyKernel3
git clone --depth=1 https://github.com/Kernel-SU/AnyKernel3
rm -rf ./AnyKernel3/.git

# 复制内核镜像
echo ">>> 复制内核镜像..."
cp "$OUT_DIR/Image" ./AnyKernel3/

cd "$WORKDIR/kernel_ws/AnyKernel3"
sed -i -E 's/(kernel.string=).*/\1SukiSU by SukiSU Developers, compiled by ferstar with ♥️/' anykernel.sh

# 处理 LZ4KD 相关文件
if [[ "$APPLY_LZ4KD" =~ ^[Yy]$ ]]; then
    echo ">>> 下载 LZ4KD zram 配置..."
    wget https://raw.githubusercontent.com/Suxiaoqinx/kernel_manifest_OnePlus_Sukisu_Ultra/main/zram.zip
    echo ">>> zram.zip 下载完成"
fi

# 生成发布包名称
ZIP_NAME="ak3-${MANIFEST}-${CUSTOM_SUFFIX}"

# 添加功能标识
if [[ "$APPLY_SCX" =~ ^[Yy]$ ]]; then
    ZIP_NAME="${ZIP_NAME}-scx"
fi

if [[ "$APPLY_LZ4KD" =~ ^[Yy]$ && "$USE_PATCH_LINUX" =~ ^[Yy]$ ]]; then
    ZIP_NAME="${ZIP_NAME}-lz4kd-kpm"
elif [[ "$APPLY_LZ4KD" =~ ^[Yy]$ ]]; then
    ZIP_NAME="${ZIP_NAME}-lz4kd"
elif [[ "$USE_PATCH_LINUX" =~ ^[Yy]$ ]]; then
    ZIP_NAME="${ZIP_NAME}-kpm"
fi
ZIP_NAME="${ZIP_NAME}-vfs.zip"
# 打包文件
echo ">>> 创建刷机包: $ZIP_NAME"
zip -r "../$ZIP_NAME" ./*

# 输出结果
ZIP_PATH="$(realpath "../$ZIP_NAME")"
echo
echo "============== 构建完成 =============="
echo "刷机包位置: $ZIP_PATH"
echo "====================================="
