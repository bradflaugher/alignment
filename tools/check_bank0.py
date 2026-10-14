#!/usr/bin/env python3
"""Fail the build if anything in the fixed bank runs past 0x4000 (romusage misses it)."""
import re, sys
end = 0
for line in open(sys.argv[1]):
    m = re.match(r'^(_CODE|_HOME|_BASE|_GSINIT|_GSFINAL|_INITIALIZER|_LIT)\s+([0-9A-F]{8})\s+([0-9A-F]{8})', line)
    if m:
        end = max(end, int(m.group(2), 16) + int(m.group(3), 16))
print('bank 0 ends at 0x%04X (%d bytes free)' % (end, 0x4000 - end))
sys.exit(1 if end > 0x4000 else 0)
