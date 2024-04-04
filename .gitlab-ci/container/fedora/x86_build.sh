#!/bin/bash
# shellcheck disable=SC2086 # we want word splitting

set -e
set -o xtrace


EPHEMERAL="
        autoconf
        automake
        bzip2
        cmake
        git
        libtool
        pkgconfig(epoxy)
        pkgconfig(gbm)
        pkgconfig(openssl)
        unzip
        wget
        xz
        "

dnf install -y --setopt=install_weak_deps=False \
    bison \
    ccache \
    clang-devel \
    flex \
    gcc \
    gcc-c++ \
    gettext \
    glslang \
    kernel-headers \
    llvm-devel \
    meson \
    "pkgconfig(dri2proto)" \
    "pkgconfig(expat)" \
    "pkgconfig(glproto)" \
    "pkgconfig(libclc)" \
    "pkgconfig(libelf)" \
    "pkgconfig(libglvnd)" \
    "pkgconfig(libomxil-bellagio)" \
    "pkgconfig(libselinux)" \
    "pkgconfig(libva)" \
    "pkgconfig(pciaccess)" \
    "pkgconfig(vdpau)" \
    "pkgconfig(vulkan)" \
    "pkgconfig(x11)" \
    "pkgconfig(x11-xcb)" \
    "pkgconfig(xcb)" \
    "pkgconfig(xcb-dri2)" \
    "pkgconfig(xcb-dri3)" \
    "pkgconfig(xcb-glx)" \
    "pkgconfig(xcb-present)" \
    "pkgconfig(xcb-randr)" \
    "pkgconfig(xcb-sync)" \
    "pkgconfig(xcb-xfixes)" \
    "pkgconfig(xdamage)" \
    "pkgconfig(xext)" \
    "pkgconfig(xfixes)" \
    "pkgconfig(xrandr)" \
    "pkgconfig(xshmfence)" \
    "pkgconfig(xxf86vm)" \
    "pkgconfig(zlib)" \
    python-unversioned-command \
    python3-devel \
    python3-mako \
    python3-ply \
    vulkan-headers \
    spirv-tools-devel \
    spirv-llvm-translator-devel \
    $EPHEMERAL


. .gitlab-ci/container/container_pre_build.sh


# dependencies where we want a specific version
export              XORG_RELEASES=https://xorg.freedesktop.org/releases/individual

export         XORGMACROS_VERSION=util-macros-1.19.0

wget $XORG_RELEASES/util/$XORGMACROS_VERSION.tar.bz2
tar -xvf $XORGMACROS_VERSION.tar.bz2 && rm $XORGMACROS_VERSION.tar.bz2
cd $XORGMACROS_VERSION; ./configure; make install; cd ..
rm -rf $XORGMACROS_VERSION

. .gitlab-ci/container/build-mold.sh

. .gitlab-ci/container/build-libdrm.sh

. .gitlab-ci/container/build-wayland.sh

pushd /usr/local
git clone https://gitlab.freedesktop.org/mesa/shader-db.git --depth 1
rm -rf shader-db/.git
cd shader-db
make
popd


############### Uninstall the build software

dnf remove -y $EPHEMERAL

. .gitlab-ci/container/container_post_build.sh
