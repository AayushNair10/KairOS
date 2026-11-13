#!/usr/bin/env bash
# Build the Kairos kernel tree from nothing: Linux at the pinned version
# (kernel/upstream), with our patch series (kernel/patches/) applied on top.
#
# usage: kernel-fetch.sh [--from REPO]
#
#   --from REPO   take Linux from this git repository (a directory or a URL)
#                 instead of the upstream URL. A checkout that is already on
#                 the machine saves the download. REPO is only read.
#
# The tree is written to ~/kairos-linux, or to $KAIROS_LINUX if that is set.
# An existing tree is never overwritten: it may hold work that hasn't been
# exported yet.

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

source_repo=$KERNEL_URL
while [ $# -gt 0 ]; do
	case $1 in
	--from)
		[ $# -ge 2 ] || die "--from needs a repository"
		source_repo=$2
		shift 2
		;;
	-h | --help)
		sed -n '2,/^$/s/^# \{0,1\}//p' "${BASH_SOURCE[0]}"
		exit 0
		;;
	*)
		die "unknown argument: $1"
		;;
	esac
done

[ ! -e "$KAIROS_LINUX" ] ||
	die "$KAIROS_LINUX already exists; check it with scripts/kernel-check.sh, or move it away"

# Work in a directory with another name and rename it at the end, so that a
# run that fails halfway never leaves something that looks like a finished
# tree.
partial=$KAIROS_LINUX.partial
[ ! -e "$partial" ] ||
	die "$partial is left over from a run that failed; look at it, then remove it"

# For a directory, git copies the files as they are and ignores --depth
# unless the source is written as a file:// URL.
case $source_repo in
/*) source_repo=file://$source_repo ;;
esac

echo "getting Linux $KERNEL_TAG from $source_repo"
git init --quiet "$partial"
# Later downloads (a newer release, for example) come from upstream, also
# when this copy comes from somewhere else.
git -C "$partial" remote add origin "$KERNEL_URL"
# --depth 1: only the files of this release, without the history behind it
# (about 280 MB instead of several GB).
git -C "$partial" fetch --quiet --depth 1 "$source_repo" tag "$KERNEL_TAG"

# Check the id before any file is written out.
got=$(git -C "$partial" rev-parse --verify "$KERNEL_TAG^{commit}")
[ "$got" = "$KERNEL_COMMIT" ] ||
	die "$KERNEL_TAG is commit $got there, but kernel/upstream pins $KERNEL_COMMIT"

# Start our branch at the pinned version and write its files out.
git -C "$partial" switch --quiet -c "$BRANCH" "$KERNEL_COMMIT"
if [ ${#PATCHES[@]} -gt 0 ]; then
	# Each patch becomes a commit again. A commit records two dates: when
	# the change was written and when the commit was made. With this
	# option the second is set to the first, so rebuilding the tree gives
	# the same commits every time.
	git -C "$partial" am --quiet --committer-date-is-author-date "${PATCHES[@]}" ||
		die "a patch didn't apply; the tree is in $partial"
fi

mv "$partial" "$KAIROS_LINUX"

echo "kernel tree  $KAIROS_LINUX"
echo "Linux        $KERNEL_TAG (commit ${KERNEL_COMMIT:0:12})"
echo "patches      ${#PATCHES[@]} applied, on branch '$BRANCH'"
echo "files        tree hash $(git -C "$KAIROS_LINUX" rev-parse "$BRANCH^{tree}")"
