#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Moves the TVCnet firmware pin in third_party/CMakeLists.txt to a commit (default: the repository's main).
# Usage: tools/tvcnet/update_pin.sh [full commit hash]   (from anywhere in the project)
set -euo pipefail
cd "$(dirname "$0")/../.."
file=third_party/CMakeLists.txt
repo=$(sed -n 's/^set(FREETVC_TVCNET_REPO "\(.*\)")$/\1/p' "$file")
commit=${1:-$(GIT_TERMINAL_PROMPT=0 git ls-remote "$repo" refs/heads/main | cut -f1)}
[[ $commit =~ ^[0-9a-f]{40}$ ]] || { echo "not a full commit hash: '$commit'" >&2; exit 1; }
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
curl -sSfL -o "$work/tvcnet.tar.gz" "$repo/archive/$commit.tar.gz"
sha=$(cmake -E sha256sum "$work/tvcnet.tar.gz" | cut -d' ' -f1)
sed -i.bak -e "s/^set(FREETVC_TVCNET_COMMIT \"[0-9a-f]*\")/set(FREETVC_TVCNET_COMMIT \"$commit\")/" \
    -e "s/^set(FREETVC_TVCNET_PIN_SHA256 \"[0-9a-f]*\")/set(FREETVC_TVCNET_PIN_SHA256 \"$sha\")/" "$file"
rm -f "$file.bak"
grep -q "\"$commit\"" "$file" && grep -q "\"$sha\"" "$file" || { echo "could not update $file" >&2; exit 1; }
echo "TVCnet pinned to $commit (SHA256 $sha); build and run the tests before committing."
