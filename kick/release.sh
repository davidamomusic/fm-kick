#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# FM-KICK release build. Needs the JieLi toolchain and the AC79 SDK (see fm1-base's README):
#   JIELI_TOOLCHAIN=... AC79_SDK=... kick/release.sh
#
# The package identity is fixed: FM-1_222, FM-KICK's own number on The Firmware Hub, from 0.5.0 on (0.3.0-0.4.9 took
# APP_VERSION's digits, FM-1_030-FM-1_049, which other firmwares share). The version shows on GLO's SYSTEM page.
set -eu
cd "$(dirname "$0")/.."
ver=$(sed -n 's/^#define APP_VERSION "\(.*\)"/\1/p' kick/app.h)
id="FM-1_222"
echo "FM-KICK $ver, identity $id"
FM1B_ID="$id" python3 tools/build.py kick
mkdir -p kick/releases
cp kick/build/fm-kick.fwsc "kick/releases/fm-kick-$ver.fwsc"
echo "package kick/releases/fm-kick-$ver.fwsc ($id)"
