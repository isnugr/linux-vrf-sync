#!/usr/bin/env bash
# Adds "plugin linux_vrf_sync_plugin.so { enable }" to the existing
# plugins{} stanza in /etc/vpp/startup.conf, after taking a timestamped
# backup. Idempotent: does nothing if the line is already present.
set -euo pipefail

CONF=/etc/vpp/startup.conf

if [ ! -f "$CONF" ]; then
  echo "error: $CONF not found" >&2
  exit 1
fi

if grep -q 'linux_vrf_sync_plugin.so' "$CONF"; then
  echo "already present in $CONF, nothing to do"
  exit 0
fi

BACKUP="${CONF}.bak.$(date +%Y%m%d-%H%M%S)"
cp "$CONF" "$BACKUP"
echo "backed up $CONF -> $BACKUP"

# Insert a new "plugin linux_vrf_sync_plugin.so { enable }" block right
# after the existing "plugin linux_nl_plugin.so { enable }" block, or
# if that anchor isn't found, right after the opening "plugins {" line.
python3 - "$CONF" <<'PYEOF'
import re, sys

path = sys.argv[1]
with open(path) as f:
    text = f.read()

new_block = "\n  plugin linux_vrf_sync_plugin.so {\n    enable\n  }\n"

anchor = re.search(r"plugin linux_nl_plugin\.so\s*\{[^}]*\}\n", text)
if anchor:
    insert_at = anchor.end()
    text = text[:insert_at] + new_block + text[insert_at:]
else:
    anchor = re.search(r"plugins\s*\{\n", text)
    if not anchor:
        raise SystemExit("could not find plugins{} stanza in " + path)
    insert_at = anchor.end()
    text = text[:insert_at] + new_block + text[insert_at:]

with open(path, "w") as f:
    f.write(text)
PYEOF

echo "updated $CONF:"
grep -n "linux_vrf_sync_plugin.so" -A2 -B2 "$CONF"
