#!/usr/bin/env bash
# shellcheck disable=SC2086 # we want word splitting

# When changing this file, you need to bump the following
# .gitlab-ci/image-tags.yml tags:
# DEBIAN_TEST_ANDROID_TAG
# DEBIAN_TEST_GL_TAG
# DEBIAN_TEST_VK_TAG
# KERNEL_ROOTFS_TAG

set -ex

DEQP_RUNNER_VERSION=0.20.0

DEQP_RUNNER_GIT_URL="${DEQP_RUNNER_GIT_URL:-https://gitlab.freedesktop.org/mesa/deqp-runner.git}"

if [ -n "${DEQP_RUNNER_GIT_TAG}${DEQP_RUNNER_GIT_REV}" ]; then
    # Build and install from source
    DEQP_RUNNER_CARGO_ARGS="--git $DEQP_RUNNER_GIT_URL"

    if [ -n "${DEQP_RUNNER_GIT_TAG}" ]; then
        DEQP_RUNNER_CARGO_ARGS="--tag ${DEQP_RUNNER_GIT_TAG} ${DEQP_RUNNER_CARGO_ARGS}"
        DEQP_RUNNER_GIT_CHECKOUT="$DEQP_RUNNER_GIT_TAG"
    else
        DEQP_RUNNER_CARGO_ARGS="--rev ${DEQP_RUNNER_GIT_REV} ${DEQP_RUNNER_CARGO_ARGS}"
        DEQP_RUNNER_GIT_CHECKOUT="$DEQP_RUNNER_GIT_REV"
    fi

    DEQP_RUNNER_CARGO_ARGS="${DEQP_RUNNER_CARGO_ARGS} ${EXTRA_CARGO_ARGS}"
else
    # Install from package registry
    DEQP_RUNNER_CARGO_ARGS="--version ${DEQP_RUNNER_VERSION} ${EXTRA_CARGO_ARGS} -- deqp-runner"
    DEQP_RUNNER_GIT_CHECKOUT="v$DEQP_RUNNER_VERSION"
fi

if [[ "$RUST_TARGET" != *-android ]]; then
    # When CC (/usr/lib/ccache/gcc) variable is set, the rust compiler uses
    # this variable when cross-compiling arm32 and build fails for zsys-sys.
    # So unset the CC variable when cross-compiling for arm32.
    if [ "$RUST_TARGET" = "armv7-unknown-linux-gnueabihf" ]; then
        unset CC
    fi
    cargo install --locked  \
        -j ${FDO_CI_CONCURRENT:-4} \
        --root /usr/local \
        ${DEQP_RUNNER_CARGO_ARGS}
else
    mkdir -p /deqp-runner
    pushd /deqp-runner
    git clone --branch "$DEQP_RUNNER_GIT_CHECKOUT" --depth 1 "$DEQP_RUNNER_GIT_URL" deqp-runner-git
    pushd deqp-runner-git

    cargo install --locked  \
        -j ${FDO_CI_CONCURRENT:-4} \
        --root /usr/local --version 2.10.0 \
        cargo-ndk

    rustup target add $RUST_TARGET
    RUSTFLAGS='-C target-feature=+crt-static' cargo ndk --target $RUST_TARGET build --release

    mv target/$RUST_TARGET/release/deqp-runner /deqp-runner

    cargo uninstall --locked  \
        --root /usr/local \
        cargo-ndk

    popd
    rm -rf deqp-runner-git
    popd
fi

# remove unused test runners to shrink images for the Mesa CI build (not kernel,
# which chooses its own deqp branch)
if [ -z "${DEQP_RUNNER_GIT_TAG}${DEQP_RUNNER_GIT_REV}" ]; then
    rm -f /usr/local/bin/igt-runner
fi
