# Kernel patch series

Kairos's changes to Linux, one change per file. They are applied in
file-name order on top of the Linux version named in `../upstream`.

Each file is a commit from the working kernel tree, written out by
`scripts/kernel-export.sh`. Don't edit the files by hand: change the commit
in the working tree and export again. `scripts/kernel-check.sh` fails if the
files and the tree disagree.
