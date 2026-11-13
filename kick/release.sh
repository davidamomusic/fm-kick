#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# FM-KICK release build. Needs the JieLi toolchain and the AC79 SDK (see fm1-base's README):
#   JIELI_TOOLCHAIN=... AC79_SDK=... kick/release.sh
#
# The package identity follows the version: FM-1_ and APP_VERSION's digits (0.3.0 -> FM-1_030), so M-UPGRADE, which
# shows only the identity and accepts one to three digits, tells one version from the next.
set -eu
cd "$(dirname "$0")/.."
ver=$(sed -n 's/^#define APP_VERSION "\(.*\)"/\1/p' kick/app.h)
digits=$(echo "$ver" | tr -d '.')
case "$digits" in
    [0-9]|[0-9][0-9]|[0-9][0-9][0-9]) ;;
    *) echo "version $ver gives no identity of 1-3 digits" >&2; exit 1 ;;
esac
id="FM-1_$digits"
echo "FM-KICK $ver, identity $id"
FM1B_ID="$id" python3 tools/build.py kick
mkdir -p kick/releases
cp kick/build/fm-kick.fwsc "kick/releases/fm-kick-$ver.fwsc"
echo "package kick/releases/fm-kick-$ver.fwsc ($id)"
