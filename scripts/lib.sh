# Shared settings for the kernel scripts. They source this file; it isn't run
# on its own.
# shellcheck shell=bash

set -euo pipefail
shopt -s nullglob	# a pattern that matches no file expands to nothing
export LC_ALL=C		# the same file order on every machine

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

# The pinned Linux version: KERNEL_TAG, KERNEL_COMMIT and KERNEL_URL.
# shellcheck source=../kernel/upstream
. "$REPO_ROOT/kernel/upstream"

# Our kernel changes, in the order they are applied (file-name order).
PATCH_DIR=$REPO_ROOT/kernel/patches
PATCHES=("$PATCH_DIR"/*.patch)

# The working kernel tree. It lives outside this repository.
KAIROS_LINUX=${KAIROS_LINUX:-$HOME/kairos-linux}

# Our commits sit on this branch of the working tree, on top of the pinned
# version.
BRANCH=kairos

die() {
	echo "error: $*" >&2
	exit 1
}

# git, run inside the working kernel tree.
kgit() {
	git -C "$KAIROS_LINUX" "$@"
}

need_tree() {
	[ -d "$KAIROS_LINUX/.git" ] ||
		die "no kernel tree at $KAIROS_LINUX; run scripts/kernel-fetch.sh"
	kgit rev-parse --verify --quiet "refs/heads/$BRANCH" >/dev/null ||
		die "$KAIROS_LINUX has no branch '$BRANCH'"
}

# Write the branch's commits, from the pinned version up, as patch files into
# the directory $1.
#
# The options make the export repeatable: exporting the same commits twice
# gives byte-identical files, so this repository's history shows only real
# changes.
#   --zero-commit    each file starts with its commit's id, and that id
#                    changes whenever the tree is rebuilt; write zeros
#   --no-signature   leave out the last lines, which hold git's version
#   --no-numbered    "[PATCH]" in every subject, not "[PATCH 2/5]", which
#                    would change every file when a patch is added
#   --full-index     full ids for the files a patch touches; git shortens
#                    them by default, to a length that grows with the size
#                    of the repository
export_series() {
	kgit format-patch --quiet --output-directory "$1" \
		--zero-commit --no-signature --no-numbered --full-index \
		--diff-algorithm=myers \
		"$KERNEL_COMMIT..$BRANCH"
}
