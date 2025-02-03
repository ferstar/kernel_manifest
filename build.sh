#!/bin/bash
set -e

export CONFIG="oneplus12"
export REPO_PATH="$PWD/git-repo/repo"
export ANYKERNEL_BRANCH="android14-6.1"
export SUSFS_BRANCH="gki-android14-6.1"

sudo apt-get update
sudo apt-get install -y git curl zip perl make gcc python3 python3-pip

mkdir -p ./git-repo
curl -o ./git-repo/repo https://storage.googleapis.com/git-repo-downloads/repo
chmod a+rx ./git-repo/repo

git clone https://github.com/TheWildJames/AnyKernel3.git -b "$ANYKERNEL_BRANCH" --depth=1
git clone https://gitlab.com/simonpunk/susfs4ksu.git -b "$SUSFS_BRANCH" --depth=1
git clone https://github.com/TheWildJames/kernel_patches.git  --depth=1

mkdir -p "$CONFIG"
cd "$CONFIG"
../git-repo/repo init -u https://github.com/ferstar/kernel_manifest.git \
    -b realme/sm8650 -m gt5pro_v.xml --repo-rev=v2.16
../git-repo/repo sync -c -j$(nproc --all) --no-tags --fail-fast

cd kernel_platform
curl -LSs "https://raw.githubusercontent.com/rifsxd/KernelSU-Next/next/kernel/setup.sh" | bash -
cd KernelSU-Next/kernel
sed -i 's/ccflags-y += -DKSU_VERSION=16/ccflags-y += -DKSU_VERSION=12335/' Makefile
cd ../../

cp ../../susfs4ksu/kernel_patches/KernelSU/10_enable_susfs_for_ksu.patch KernelSU-Next/
cp ../../susfs4ksu/kernel_patches/50_add_susfs_in_gki-android14-6.1.patch common/
cp ../../susfs4ksu/kernel_patches/fs/* common/fs/
cp ../../susfs4ksu/kernel_patches/include/linux/* common/include/linux/

cd KernelSU-Next
patch -p1 --forward < 10_enable_susfs_for_ksu.patch || true
cd ../common
patch -p1 < 50_add_susfs_in_gki-android14-6.1.patch || true
cd ../../

cd kernel_platform
cp ../../kernel_patches/apk_sign.c_fix.patch .
patch -p1 -F 3 < apk_sign.c_fix.patch
cp ../../kernel_patches/core_hook.c_fix.patch .
patch -p1 --fuzz=3 < core_hook.c_fix.patch
cp ../../kernel_patches/selinux.c_fix.patch .
patch -p1 -F 3 < selinux.c_fix.patch

cd common
cp ../../../kernel_patches/69_hide_stuff.patch .
patch -p1 -F 3 < 69_hide_stuff.patch || true
cd ../../

cd kernel_platform/common
echo -e "\n# SUSFS Configuration" >> arch/arm64/configs/gki_defconfig
cat <<EOT >> arch/arm64/configs/gki_defconfig
CONFIG_KSU=y
CONFIG_KSU_SUSFS=y
CONFIG_KSU_SUSFS_HAS_MAGIC_MOUNT=y
CONFIG_KSU_SUSFS_SUS_PATH=y
CONFIG_KSU_SUSFS_SUS_MOUNT=y
CONFIG_KSU_SUSFS_AUTO_ADD_SUS_KSU_DEFAULT_MOUNT=y
CONFIG_KSU_SUSFS_AUTO_ADD_SUS_BIND_MOUNT=y
CONFIG_KSU_SUSFS_SUS_KSTAT=y
CONFIG_KSU_SUSFS_SUS_OVERLAYFS=y
CONFIG_KSU_SUSFS_TRY_UMOUNT=y
CONFIG_KSU_SUSFS_AUTO_ADD_TRY_UMOUNT_FOR_BIND_MOUNT=y
CONFIG_KSU_SUSFS_SPOOF_UNAME=y
CONFIG_KSU_SUSFS_ENABLE_LOG=y
CONFIG_KSU_SUSFS_HIDE_KSU_SUSFS_SYMBOLS=y
CONFIG_KSU_SUSFS_SPOOF_CMDLINE_OR_BOOTCONFIG=y
CONFIG_KSU_SUSFS_OPEN_REDIRECT=y
CONFIG_KSU_SUSFS_SUS_SU=y
CONFIG_TMPFS_XATTR=y
EOT

sed -i 's/check_defconfig//' build.config.gki
sed -i '$s|echo "\$res"|echo "-android14-11-o-v$(date +%Y%m%d)"|' scripts/setlocalversion

rm -rf android/abi_gki_protected_exports_*
patch -p1 < ../../.repo/manifests/patches/001-lz4.patch
patch -p1 < ../../.repo/manifests/patches/002-zstd.patch

cd ../
python build_with_bazel.py -t pineapple gki \
  --lto=thin --config=fast --disk_cache=$HOME/.cache/bazel \
  --//msm-kernel:skip_abi=true --//msm-kernel:skip_abl=true -o "$(pwd)/out" || true

cp bazel-out/k8-fastbuild/bin/msm-kernel/pineapple_gki_kbuild_mixed_tree/Image ../../AnyKernel3/Image

cd ../../AnyKernel3
ZIP_NAME="Anykernel3-$CONFIG-android14-11-o-v$(date +%Y%m%d).zip"
zip -r "../$ZIP_NAME" ./*

echo "Done, kernel path: $PWD/../$ZIP_NAME"
