#!/bin/sh
set -e
HOOK="${1:-/mnt/mmc01/testing/hook.so}"
PRE=/etc/ld.so.preload
[ -f "$PRE" ] && grep -vxF "$HOOK" "$PRE" > "$PRE.tmp" && mv "$PRE.tmp" "$PRE" || true
echo "removed $HOOK from $PRE; restart ppsapp to drop the hook"
