#!/bin/sh
set -eu

NVTOP_REPOSITORY=https://github.com/Syllo/nvtop.git
NVTOP_RELEASE=3.3.2
NVTOP_COMMIT=76890233d759199f50ad3bdb57a0c0988e96fc44

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repository_dir=$(dirname -- "$script_dir")
destination=${1:-"$repository_dir/third_party/nvtop"}

if [ -e "$destination" ]; then
    echo "nvtop checkout already exists: $destination" >&2
    exit 1
fi

destination_parent=$(dirname -- "$destination")
mkdir -p "$destination_parent"

checkout_dir=$(mktemp -d "$destination_parent/.nvtop-checkout.XXXXXX")
trap 'rm -rf "$checkout_dir"' EXIT HUP INT TERM

git clone --quiet --depth 1 --branch "$NVTOP_RELEASE" \
    "$NVTOP_REPOSITORY" "$checkout_dir/repository"

actual_commit=$(git -C "$checkout_dir/repository" rev-parse HEAD)
if [ "$actual_commit" != "$NVTOP_COMMIT" ]; then
    echo "nvtop $NVTOP_RELEASE resolved to unexpected commit $actual_commit" >&2
    exit 1
fi

mv "$checkout_dir/repository" "$destination"
echo "nvtop $NVTOP_RELEASE checked out at $NVTOP_COMMIT"
