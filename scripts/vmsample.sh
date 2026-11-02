#!/bin/bash
# vmsample.sh: once a second, print "unix_time k=v ..." for the memory
# counters that matter for slow episodes (RESEARCH_LOG 6.18): swapping,
# major faults, direct reclaim and compaction. Runs until killed.
while true; do
	echo "$(date +%s.%N) $(grep -E '^(pswpin|pswpout|pgmajfault|allocstall_normal|allocstall_movable|compact_stall|pgscan_direct|pgscan_kswapd) ' /proc/vmstat | awk '{printf "%s=%s ", $1, $2}')"
	sleep 1
done
