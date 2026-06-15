#!/bin/sh
# Install the connect() interposer globally (/etc/ld.so.preload) so ppsapp
# redirects to the local bridge. Because this injects into EVERY dynamically
# linked process (incl. sshd/shell), it (1) pre-flight-tests that the .so loads,
# and (2) arms a dead-man switch that auto-disables the preload after a timeout
# unless you cancel it -- so a bad hook cannot lock you out.
# Run on the device. Usage: install-hook.sh [/abs/path/to/hook.so] [deadman-seconds]
set -e
HOOK="${1:-/mnt/mmc01/testing/hook.so}"
DEADMAN="${2:-180}"
PRE=/etc/ld.so.preload
[ -f "$HOOK" ] || { echo "hook.so not found at $HOOK"; exit 1; }

echo "pre-flight: LD_PRELOAD=$HOOK /bin/true"
if ! LD_PRELOAD="$HOOK" /bin/true; then
    echo "FAILED: the hook does not load (would break every exec). Aborting; nothing changed."
    exit 1
fi
echo "pre-flight OK (the .so loads and a hooked process runs normally)"

( sleep "$DEADMAN"; : > "$PRE" ) &
DM=$!
echo "dead-man switch armed (pid $DM): $PRE will be emptied in ${DEADMAN}s unless you cancel."

touch "$PRE"
grep -qxF "$HOOK" "$PRE" || echo "$HOOK" >> "$PRE"
echo "installed: $HOOK in $PRE"; cat "$PRE"
cat <<EOF

NEXT (in THIS session; if anything misbehaves just wait ${DEADMAN}s and it reverts):
  kill \$(pidof ppsapp)                              # watchdog relaunches it with the hook
  cat /proc/\$(pidof ppsapp)/maps | grep hook.so      # confirm the hook loaded into ppsapp
  cat /proc/net/tcp | grep -i 2BD9                    # ppsapp dest should be 0100007F:2BD9 (127.0.0.1:11225)
  # open a SECOND ssh session and run a command to prove remote access still works
If all good, CANCEL the dead-man switch so the hook persists:
  kill $DM
To revert manually at any time:
  : > $PRE     # disable (empty the preload file), then: kill \$(pidof ppsapp)
EOF
