#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# FM-KICK's browser player into kick/docs/play: the build (fm1-base's emu with FM-KICK's own page, kick/web/emu/index.html,
# and its own worklet part, app_worklet.js) and Render Audio's files beside it. From the repository's root:
#   WASI_SYSROOT=... PATH=kick/tools:$PATH sh kick/tools/play.sh
set -e
python3 tools/build.py kick --web
dst=kick/docs/play
mkdir -p "$dst"
cp -R kick/build/emu/index.html kick/build/emu/app.wasm kick/build/emu/worklet.js kick/build/emu/app.js \
      kick/build/emu/app_worklet.js kick/build/emu/app_own_worklet.js kick/build/emu/parts "$dst"/
cp kick/web/emu/render.js kick/web/emu/render_worker.js "$dst"/
echo "play     $dst"
