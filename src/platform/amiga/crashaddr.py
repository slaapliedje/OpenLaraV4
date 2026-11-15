#!/usr/bin/env python3
"""Name the function at a crash offset printed by olCrashed ("main+OFFSET"):
    ./crashaddr.py OFFSET BINARY-NAME      e.g. ./crashaddr.py +123456 OpenLaraMag
Uses the unstripped copy the build keeps in build*/NAME.debug."""
import glob, os, subprocess, sys
off = int(sys.argv[1], 0)
name = sys.argv[2] if len(sys.argv) > 2 else 'OpenLara'
dbg = [f for f in glob.glob('build*/%s.debug' % name)][0]
nm = os.path.join(os.environ.get('TC', os.path.expanduser('~/dev/toolchains/amigaos')), 'bin', 'm68k-amigaos-nm')
syms = []
for line in subprocess.run([nm, '-C', '-n', dbg], capture_output=True, text=True).stdout.splitlines():
    parts = line.split(' ', 2)
    if len(parts) == 3 and parts[1] in 'tTwW':
        syms.append((int(parts[0], 16), parts[2]))
main = [a for a, n in syms if n in ('main', '_main')][0]
target = main + off
best = max((s for s in syms if s[0] <= target), key=lambda s: s[0])
print('%08x = %s + 0x%x' % (target, best[1], target - best[0]))
