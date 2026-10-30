#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds DeZog with dezog-dzrp-custom-memory.patch: a git worktree of the pinned clone, without its releases/ folder.
# usage: tools/dezog/build_dezog.sh <DeZog clone> <output dir>   (Node 22 on PATH, or NODE22_BIN)
set -eu
clone=$(cd "${1:?DeZog clone}" && pwd)
out=${2:?output dir}
here=$(cd "$(dirname "$0")" && pwd)
pinned=715f2b35b46e8ef02cffaf65f57d649ba3711a40
PATH=${NODE22_BIN:+$NODE22_BIN:}$PATH

git -C "$clone" worktree add --no-checkout --detach "$out" "$pinned"
git -C "$out" sparse-checkout set --no-cone '/*' '!/releases/'
git -C "$out" checkout --quiet
git -C "$out" apply "$here/dezog-dzrp-custom-memory.patch"
cd "$out"
npm ci --ignore-scripts --no-audit --no-fund
npx tsc -p ./
npx mocha -u tdd out/tests/dzrpgenericremote.tests.js
npm run esbuild-node
npm run esbuild-browser
