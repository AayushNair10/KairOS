#!/usr/bin/env python3
"""Drop the page-cache copies of every file under the given directories
(posix_fadvise DONTNEED; no root needed, the files are not changed).
Frees memory for a run without dropping all caches as root would: the
experiment's results are written once and not read back during a batch,
but their cached pages count against the VM's 3.9 GB (RESEARCH_LOG 6.18).

Usage: scripts/evict_cache.py DIR ...
"""
import os
import sys

n = 0
for top in sys.argv[1:]:
    for root, _, files in os.walk(top):
        for name in files:
            try:
                fd = os.open(os.path.join(root, name), os.O_RDONLY)
            except OSError:
                continue
            try:
                os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
                n += 1
            finally:
                os.close(fd)
print(f"evicted {n} files", file=sys.stderr)
