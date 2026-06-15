#!/bin/sh
set -e
HOOK="${1:-/mnt/mmc01/testing/hook.so}"
PRE=/etc/ld.so.preload
if [ -f "$PRE" ]; then
    # grep -v exits 1 when it filters out every line (hook was the sole entry);
    # `|| true` keeps that case (empty result -> empty file, which disables preload).
    grep -vxF "$HOOK" "$PRE" > "$PRE.tmp" || true
    mv "$PRE.tmp" "$PRE"
fi
echo "removed $HOOK from $PRE; restart ppsapp to drop the hook"
