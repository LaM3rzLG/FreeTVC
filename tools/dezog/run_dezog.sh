#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Opens DeZog (built from source) in a VS Code with its own profile, on a fresh copy of tools/dezog/project.
# usage: tools/dezog/run_dezog.sh <DeZog source dir> <work dir>
set -eu
dezog=$(cd "${1:?DeZog source dir}" && pwd)
work=${2:?work dir}
root=$(cd "$(dirname "$0")/../.." && pwd)

mkdir -p "$work/user/User" "$work/extensions"
work=$(cd "$work" && pwd)
rm -rf "$work/project"
cp -r "$root/tools/dezog/project" "$work/project"
(cd "$work/project" && "${SJASMPLUS:-$(command -v sjasmplus || echo "$root/.tools/sjasmplus-1.24.0/bin/sjasmplus")}" --sld=hello.sld --fullpath hello.asm)

cat > "$work/user/User/settings.json" <<'EOF'
{
    "telemetry.telemetryLevel": "off",
    "update.mode": "none",
    "extensions.autoCheckUpdates": false,
    "extensions.autoUpdate": false,
    "workbench.startupEditor": "none",
    "workbench.enableExperiments": false,
    "security.workspace.trust.enabled": false,
    "chat.disableAIFeatures": true,
    "dezog.log.transport": true,
    "dezog.log.global": true
}
EOF

exec code --new-window --user-data-dir "$work/user" --extensions-dir "$work/extensions" \
    --extensionDevelopmentPath="$dezog" "$work/project"
