#!/bin/bash
# shellcheck disable=SC1091 # The relative paths in this file only become valid at runtime.
# shellcheck disable=SC2034 # Variables are used in scripts called from here
# shellcheck disable=SC2086 # we want word splitting

set -e
set -o xtrace

export DEBIAN_FRONTEND=noninteractive

check_minio()
{
    MINIO_PATH="${MINIO_HOST}/mesa-lava/$1/${DISTRIBUTION_TAG}/${DEBIAN_ARCH}"
    if wget -q --method=HEAD "https://${MINIO_PATH}/done"; then
        exit
    fi
}

# If remote files are up-to-date, skip rebuilding them
check_minio "${FDO_UPSTREAM_REPO}"
check_minio "${CI_PROJECT_PATH}"

. .gitlab-ci/container/container_pre_build.sh

# Install rust, which we'll be using for deqp-runner.  It will be cleaned up at the end.
. .gitlab-ci/container/build-rust.sh

if [[ "$DEBIAN_ARCH" = "arm64" ]]; then
    GCC_ARCH="aarch64-linux-gnu"
    KERNEL_ARCH="arm64"
    SKQP_ARCH="arm64"
    DEFCONFIG="arch/arm64/configs/defconfig"
    DEVICE_TREES="arch/arm64/boot/dts/rockchip/rk3399-gru-kevin.dtb"
    DEVICE_TREES+=" arch/arm64/boot/dts/amlogic/meson-gxl-s805x-libretech-ac.dtb"
    DEVICE_TREES+=" arch/arm64/boot/dts/allwinner/sun50i-h6-pine-h64.dtb"
    DEVICE_TREES+=" arch/arm64/boot/dts/amlogic/meson-gxm-khadas-vim2.dtb"
    DEVICE_TREES+=" arch/arm64/boot/dts/qcom/apq8016-sbc.dtb"
    DEVICE_TREES+=" arch/arm64/boot/dts/qcom/apq8096-db820c.dtb"
    DEVICE_TREES+=" arch/arm64/boot/dts/amlogic/meson-g12b-a311d-khadas-vim3.dtb"
    DEVICE_TREES+=" arch/arm64/boot/dts/mediatek/mt8183-kukui-jacuzzi-juniper-sku16.dtb"
    DEVICE_TREES+=" arch/arm64/boot/dts/nvidia/tegra210-p3450-0000.dtb"
    DEVICE_TREES+=" arch/arm64/boot/dts/qcom/sc7180-trogdor-lazor-limozeen-nots-r5.dtb"
    DEVICE_TREES+=" arch/arm64/boot/dts/qcom/sc7180-trogdor-kingoftown-r1.dtb"
    DEVICE_TREES+=" arch/arm64/boot/dts/freescale/imx8mq-nitrogen.dtb"
    KERNEL_IMAGE_NAME="Image"

elif [[ "$DEBIAN_ARCH" = "armhf" ]]; then
    GCC_ARCH="arm-linux-gnueabihf"
    KERNEL_ARCH="arm"
    SKQP_ARCH="arm"
    DEFCONFIG="arch/arm/configs/multi_v7_defconfig"
    DEVICE_TREES="arch/arm/boot/dts/rk3288-veyron-jaq.dtb"
    DEVICE_TREES+=" arch/arm/boot/dts/sun8i-h3-libretech-all-h3-cc.dtb"
    DEVICE_TREES+=" arch/arm/boot/dts/imx6q-cubox-i.dtb"
    DEVICE_TREES+=" arch/arm/boot/dts/tegra124-jetson-tk1.dtb"
    KERNEL_IMAGE_NAME="zImage"
    . .gitlab-ci/container/create-cross-file.sh armhf
else
    GCC_ARCH="x86_64-linux-gnu"
    KERNEL_ARCH="x86_64"
    SKQP_ARCH="x64"
    DEFCONFIG="arch/x86/configs/x86_64_defconfig"
    DEVICE_TREES=""
    KERNEL_IMAGE_NAME="bzImage"
    ARCH_PACKAGES="libasound2-dev libcap-dev libfdt-dev libva-dev wayland-protocols p7zip"
fi

# Determine if we're in a cross build.
if [[ -e /cross_file-$DEBIAN_ARCH.txt ]]; then
    EXTRA_MESON_ARGS="--cross-file /cross_file-$DEBIAN_ARCH.txt"
    EXTRA_CMAKE_ARGS="-DCMAKE_TOOLCHAIN_FILE=/toolchain-$DEBIAN_ARCH.cmake"

    if [ $DEBIAN_ARCH = arm64 ]; then
        RUST_TARGET="aarch64-unknown-linux-gnu"
    elif [ $DEBIAN_ARCH = armhf ]; then
        RUST_TARGET="armv7-unknown-linux-gnueabihf"
    fi
    rustup target add $RUST_TARGET
    export EXTRA_CARGO_ARGS="--target $RUST_TARGET"

    export ARCH=${KERNEL_ARCH}
    export CROSS_COMPILE="${GCC_ARCH}-"
fi

apt-get update
apt-get install -y --no-remove \
                   ${ARCH_PACKAGES} \
                   automake \
                   bc \
                   clang \
                   cmake \
                   debootstrap \
                   git \
                   glslang-tools \
                   libdrm-dev \
                   libegl1-mesa-dev \
                   libxext-dev \
                   libfontconfig-dev \
                   libgbm-dev \
                   libgl-dev \
                   libgles2-mesa-dev \
                   libglu1-mesa-dev \
                   libglx-dev \
                   libpng-dev \
                   libssl-dev \
                   libudev-dev \
                   libvulkan-dev \
                   libwaffle-dev \
                   libwayland-dev \
                   libx11-xcb-dev \
                   libxcb-dri2-0-dev \
                   libxkbcommon-dev \
                   libwayland-dev \
                   ninja-build \
                   patch \
                   protobuf-compiler \
                   python-is-python3 \
                   python3-distutils \
                   python3-mako \
                   python3-numpy \
                   python3-serial \
                   unzip \
                   wget \
                   zstd


if [[ "$DEBIAN_ARCH" = "armhf" ]]; then
    apt-get install -y --no-remove \
                       libegl1-mesa-dev:armhf \
                       libelf-dev:armhf \
                       libgbm-dev:armhf \
                       libgles2-mesa-dev:armhf \
                       libpng-dev:armhf \
                       libudev-dev:armhf \
                       libvulkan-dev:armhf \
                       libwaffle-dev:armhf \
                       libwayland-dev:armhf \
                       libx11-xcb-dev:armhf \
                       libxkbcommon-dev:armhf
fi

mkdir -p "/lava-files/rootfs-${DEBIAN_ARCH}"

############### Setuping
if [ "$DEBIAN_ARCH" = "amd64" ]; then
  . .gitlab-ci/container/setup-wine.sh "/dxvk-wine64"
  . .gitlab-ci/container/install-wine-dxvk.sh
  mv /dxvk-wine64 "/lava-files/rootfs-${DEBIAN_ARCH}/"
fi

############### Installing
. .gitlab-ci/container/install-wine-apitrace.sh
mkdir -p "/lava-files/rootfs-${DEBIAN_ARCH}/apitrace-msvc-win64"
mv /apitrace-msvc-win64/bin "/lava-files/rootfs-${DEBIAN_ARCH}/apitrace-msvc-win64"
rm -rf /apitrace-msvc-win64

############### Building
STRIP_CMD="${GCC_ARCH}-strip"
mkdir -p /lava-files/rootfs-${DEBIAN_ARCH}/usr/lib/$GCC_ARCH


############### Build apitrace
. .gitlab-ci/container/build-apitrace.sh
mkdir -p /lava-files/rootfs-${DEBIAN_ARCH}/apitrace
mv /apitrace/build /lava-files/rootfs-${DEBIAN_ARCH}/apitrace
rm -rf /apitrace


############### Build dEQP runner
. .gitlab-ci/container/build-deqp-runner.sh
mkdir -p /lava-files/rootfs-${DEBIAN_ARCH}/usr/bin
mv /usr/local/bin/*-runner /lava-files/rootfs-${DEBIAN_ARCH}/usr/bin/.


############### Build dEQP
DEQP_TARGET=surfaceless . .gitlab-ci/container/build-deqp.sh

mv /deqp /lava-files/rootfs-${DEBIAN_ARCH}/.


############### Build SKQP
if [[ "$DEBIAN_ARCH" = "arm64" ]] \
  || [[ "$DEBIAN_ARCH" = "amd64" ]]; then
    . .gitlab-ci/container/build-skqp.sh
    mv /skqp /lava-files/rootfs-${DEBIAN_ARCH}/.
fi

############### Build piglit
PIGLIT_OPTS="-DPIGLIT_BUILD_DMA_BUF_TESTS=ON" . .gitlab-ci/container/build-piglit.sh
mv /piglit /lava-files/rootfs-${DEBIAN_ARCH}/.

############### Build libva tests
if [[ "$DEBIAN_ARCH" = "amd64" ]]; then
    . .gitlab-ci/container/build-va-tools.sh
    mv /va/bin/* /lava-files/rootfs-${DEBIAN_ARCH}/usr/bin/
fi

############### Build Crosvm
if [[ ${DEBIAN_ARCH} = "amd64" ]]; then
    . .gitlab-ci/container/build-crosvm.sh
    mv /usr/local/bin/crosvm /lava-files/rootfs-${DEBIAN_ARCH}/usr/bin/
    mv /usr/local/lib/$GCC_ARCH/libvirglrenderer.* /lava-files/rootfs-${DEBIAN_ARCH}/usr/lib/$GCC_ARCH/
fi

############### Build libdrm
EXTRA_MESON_ARGS+=" -D prefix=/libdrm"
. .gitlab-ci/container/build-libdrm.sh


############### Build local stuff for use by igt and kernel testing, which
############### will reuse most of our container build process from a specific
############### hash of the Mesa tree.
if [[ -e ".gitlab-ci/local/build-rootfs.sh" ]]; then
    . .gitlab-ci/local/build-rootfs.sh
fi


############### Build kernel
. .gitlab-ci/container/build-kernel.sh

############### Delete rust, since the tests won't be compiling anything.
rm -rf /root/.cargo
rm -rf /root/.rustup

############### Create rootfs
set +e
if ! debootstrap \
     --variant=minbase \
     --arch=${DEBIAN_ARCH} \
     --components main,contrib,non-free \
     bullseye \
     /lava-files/rootfs-${DEBIAN_ARCH}/ \
     http://deb.debian.org/debian; then
    cat /lava-files/rootfs-${DEBIAN_ARCH}/debootstrap/debootstrap.log
    exit 1
fi
set -e

cp .gitlab-ci/container/create-rootfs.sh /lava-files/rootfs-${DEBIAN_ARCH}/.
cp .gitlab-ci/container/debian/llvm-snapshot.gpg.key /lava-files/rootfs-${DEBIAN_ARCH}/.
cp .gitlab-ci/container/debian/winehq.gpg.key /lava-files/rootfs-${DEBIAN_ARCH}/.
chroot /lava-files/rootfs-${DEBIAN_ARCH} sh /create-rootfs.sh
rm /lava-files/rootfs-${DEBIAN_ARCH}/{llvm-snapshot,winehq}.gpg.key
rm /lava-files/rootfs-${DEBIAN_ARCH}/create-rootfs.sh


############### Install the built libdrm
# Dependencies pulled during the creation of the rootfs may overwrite
# the built libdrm. Hence, we add it after the rootfs has been already
# created.
find /libdrm/ -name lib\*\.so\* \
  -exec cp -t /lava-files/rootfs-${DEBIAN_ARCH}/usr/lib/$GCC_ARCH/. {} \;
mkdir -p /lava-files/rootfs-${DEBIAN_ARCH}/libdrm/
cp -Rp /libdrm/share /lava-files/rootfs-${DEBIAN_ARCH}/libdrm/share
rm -rf /libdrm


if [ ${DEBIAN_ARCH} = arm64 ]; then
    # Make a gzipped copy of the Image for db410c.
    gzip -k /lava-files/Image
    KERNEL_IMAGE_NAME+=" Image.gz"
fi

du -ah /lava-files/rootfs-${DEBIAN_ARCH} | sort -h | tail -100
pushd /lava-files/rootfs-${DEBIAN_ARCH}
  tar --zstd -cf /lava-files/lava-rootfs.tar.zst .
popd

. .gitlab-ci/container/container_post_build.sh

############### Upload the files!
FILES_TO_UPLOAD="lava-rootfs.tar.zst \
                 $KERNEL_IMAGE_NAME"

if [[ -n $DEVICE_TREES ]]; then
    FILES_TO_UPLOAD="$FILES_TO_UPLOAD $(basename -a $DEVICE_TREES)"
fi

for f in $FILES_TO_UPLOAD; do
    ci-fairy s3cp --token-file "${CI_JOB_JWT_FILE}" /lava-files/$f \
             https://${MINIO_PATH}/$f
done

touch /lava-files/done
ci-fairy s3cp --token-file "${CI_JOB_JWT_FILE}" /lava-files/done https://${MINIO_PATH}/done
