#!/usr/bin/env bash
# Write the working kernel tree's commits into kernel/patches/, one patch
# file per commit, replacing the files that are there.
#
# usage: kernel-export.sh
#
# Run it after committing a change in the working tree, then run
# scripts/kernel-check.sh and commit the patch files in this repository.
# Only commits are exported; changes that aren't committed stay behind.

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
need_tree

kgit merge-base --is-ancestor "$KERNEL_COMMIT" "$BRANCH" ||
	die "branch '$BRANCH' isn't built on $KERNEL_TAG (${KERNEL_COMMIT:0:12})"

if [ -n "$(kgit status --porcelain)" ]; then
	echo "warning: $KAIROS_LINUX has changes that aren't committed; they are not exported" >&2
fi

# Export into a scratch directory first, so that a failure leaves the old
# series as it was.
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
export_series "$tmp"
new=("$tmp"/*.patch)

if [ ${#PATCHES[@]} -gt 0 ]; then
	rm -f "${PATCHES[@]}"
fi
if [ ${#new[@]} -gt 0 ]; then
	cp "${new[@]}" "$PATCH_DIR/"
fi

echo "exported ${#new[@]} patch(es) to ${PATCH_DIR#"$REPO_ROOT"/}/"
for p in "${new[@]}"; do
	echo "  $(basename "$p")"
done
