#!/bin/sh
# Clone the two kernel trees VKage builds against.
#
# With --shallow the clones are a few hundred MiB instead of several GiB,
# which is enough to build but not to develop against: a depth-1 clone has
# no upstream history, so it cannot rebase or produce a patch series.

set -eu

REPO=https://github.com/zyedidia/linux
DEPTH=

usage() {
	echo "usage: ${0##*/} [--shallow]" >&2
	exit 1
}

for arg; do
	case $arg in
	--shallow) DEPTH="--depth 1" ;;
	-h|--help)  usage ;;
	*) echo "${0##*/}: unknown option: $arg" >&2; usage ;;
	esac
done

clone() {
	if [ -d "$1/linux" ]; then
		echo "$1/linux already exists, skipping"
		return
	fi
	git clone $DEPTH -b "$2" "$REPO" "$1/linux"
}

clone linux-uml  vkage-uml
clone linux-host vkage-host
