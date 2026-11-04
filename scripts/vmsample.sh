#!/bin/bash
# vmsample.sh: once a second, print "unix_time k=v ..." for the memory
# counters that matter for slow episodes (RESEARCH_LOG 6.18): swapping,
# major faults, direct reclaim and compaction; and the PSI stall totals
# (us), e.g. psi_memory_some, psi_memory_full. Runs until killed.
while true; do
	echo "$(date +%s.%N) $(grep -E '^(pswpin|pswpout|pgmajfault|allocstall_normal|allocstall_movable|compact_stall|pgscan_direct|pgscan_kswapd) ' /proc/vmstat | awk '{printf "%s=%s ", $1, $2}')$(for r in cpu memory io; do awk -v r=$r '{sub("total=", "", $5); printf "psi_%s_%s=%s ", r, $1, $5}' /proc/pressure/$r; done)"
	sleep 1
done
