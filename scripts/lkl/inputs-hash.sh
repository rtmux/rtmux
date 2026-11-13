#!/bin/sh
# Print one hash of everything that changes a built LKL archive besides the
# upstream commit: the build script, the LKL configs and helpers, the CI setup,
# and the runner image (it carries the apt-installed cross compiler and binutils).
# build-lkl.yml keys its archive cache on it and publish-lkl.yml compares it with
# the last release, so the two cannot drift apart.
# SPDX-License-Identifier: Apache-2.0
set -eu

cd "$(dirname "$0")/../.."
{
    git ls-files scripts/build-lkl-rv64.sh 'configs/lkl-*.config' scripts/lkl \
        .github/actions/setup-rtmux/action.yml .github/workflows/build-lkl.yml |
        sort | xargs sha256sum
    echo "image=${ImageVersion:-unknown}"
} | sha256sum | cut -d' ' -f1
