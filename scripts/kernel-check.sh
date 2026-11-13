#!/usr/bin/env bash
# Prove that the patch series in kernel/patches/ and the working kernel tree
# say the same thing, and run the kernel's style checker on every patch.
#
# usage: kernel-check.sh
#
# The checks, in order:
#   pin      the tree is built on the Linux version named in kernel/upstream
#   clean    nothing in the tree is uncommitted (only commits are exported)
#   rebuild  the pinned version plus the patch files gives exactly the
#            branch's files
#   series   exporting the branch again gives exactly the patch files
#   style    the kernel's scripts/checkpatch.pl accepts every patch
#
# It exits with 0 only if every check passes.

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
need_tree

failed=0
ok() {
	printf '%-8s ok    %s\n' "$1" "$2"
}
bad() {
	printf '%-8s FAIL  %s\n' "$1" "$2"
	failed=1
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# pin
got=$(kgit rev-parse --verify --quiet "$KERNEL_TAG^{commit}" || true)
if [ "$got" != "$KERNEL_COMMIT" ]; then
	bad pin "$KERNEL_TAG is ${got:-missing} in the tree; kernel/upstream pins $KERNEL_COMMIT"
elif ! kgit merge-base --is-ancestor "$KERNEL_COMMIT" "$BRANCH"; then
	bad pin "branch '$BRANCH' isn't built on $KERNEL_TAG"
else
	ok pin "$KERNEL_TAG is commit ${KERNEL_COMMIT:0:12}"
fi

# clean
if [ -n "$(kgit status --porcelain)" ]; then
	bad clean "$KAIROS_LINUX has changes that aren't committed"
else
	ok clean "nothing uncommitted"
fi

# rebuild
#
# Git keeps a list of every file and its content hash, the index, from which
# it computes one hash for the whole directory tree. Here a scratch index is
# filled with the pinned version and each patch is applied to it. Nothing is
# written to the working tree, so this takes seconds, not a second 2 GB
# checkout. The resulting hash must equal the branch's.
index=$tmp/index
GIT_INDEX_FILE=$index kgit read-tree "$KERNEL_COMMIT"
applied=1
for p in "${PATCHES[@]}"; do
	if ! GIT_INDEX_FILE=$index kgit apply --cached "$p" 2>"$tmp/apply.err"; then
		bad rebuild "$(basename "$p") doesn't apply: $(head -n 1 "$tmp/apply.err")"
		applied=0
		break
	fi
done
if [ $applied = 1 ]; then
	rebuilt=$(GIT_INDEX_FILE=$index kgit write-tree)
	branch_tree=$(kgit rev-parse "$BRANCH^{tree}")
	if [ "$rebuilt" = "$branch_tree" ]; then
		ok rebuild "$KERNEL_TAG + ${#PATCHES[@]} patch(es) = the branch's files (tree hash ${rebuilt:0:12})"
	else
		bad rebuild "$KERNEL_TAG + patches gives tree ${rebuilt:0:12}, the branch has ${branch_tree:0:12}"
	fi
fi

# series
#
# "rebuild" compares files only. This compares everything a patch file holds:
# the message, the author, the date and the order. It also catches a commit
# that was never exported and a patch file with no commit.
mkdir "$tmp/stored" "$tmp/again"
if [ ${#PATCHES[@]} -gt 0 ]; then
	cp "${PATCHES[@]}" "$tmp/stored/"
fi
export_series "$tmp/again"
if diff -r "$tmp/stored" "$tmp/again" >"$tmp/series.diff"; then
	ok series "${#PATCHES[@]} patch file(s) match the branch's commits"
else
	bad series "the patch files differ from the branch's commits; run scripts/kernel-export.sh"
	sed -e 's/^/         /' -e "s|$tmp/stored|kernel/patches|; s|$tmp/again|the branch|" \
		"$tmp/series.diff" | head -n 6
fi

# style
if [ ${#PATCHES[@]} -eq 0 ]; then
	ok style "no patches to check"
fi
for p in "${PATCHES[@]}"; do
	# checkpatch.pl has to run from inside a kernel tree. It exits with
	# 0 only when it has no errors and no warnings.
	if out=$(cd "$KAIROS_LINUX" && ./scripts/checkpatch.pl --terse "$p" 2>&1); then
		ok style "$(basename "$p")"
	else
		bad style "$(basename "$p")"
		echo "$out" | sed -e "s|^$PATCH_DIR/||" -e 's/^/         /'
	fi
done

exit $failed
