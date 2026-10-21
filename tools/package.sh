#!/bin/sh
# Builds one app for the device and packs it into a .pkgmerp.
#
#     tools/package.sh apps/calculator [dist]
#
# A .pkgmerp is an uncompressed squashfs image with the app folder at its root.
# The player mounts it read-only through a loop device, so there is no install
# step and no extracted copy on the card: the file the user copies is the file
# that runs.
#
# Uncompressed is deliberate. The kernel serves the data straight from the card
# through the page cache, with no decompression workspace; the image is larger,
# but this player has more card than RAM.
#
# The device build needs the pinned cross toolchain, exactly as `make device`
# does. MORPTHIS_MODE can be overridden for a host-only dry run, but a .pkgmerp
# should contain the device binary.

set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
	echo "usage: $0 <app-dir> [out-dir]" >&2
	exit 2
fi

app_dir=$1
out_dir=${2:-dist}
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mode=${MORPTHIS_MODE:-device}

if [ ! -f "$app_dir/app.json" ]; then
	echo "package: $app_dir has no app.json" >&2
	exit 2
fi

if ! command -v mksquashfs >/dev/null 2>&1; then
	echo "package: mksquashfs not found; install squashfs-tools" >&2
	exit 1
fi

python3 "$script_dir/validate-manifest.py" "$app_dir"

read_manifest() {
	python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get(sys.argv[2], ""))' \
		"$app_dir/app.json" "$1"
}

id=$(read_manifest id)
if [ -z "$id" ]; then
	id=$(basename "$app_dir")
fi
version=$(read_manifest version)
entry=$(read_manifest entry)

if [ -z "$version" ]; then
	version=0.0.0
fi

if [ -z "$entry" ]; then
	echo "package: $app_dir/app.json has no entry" >&2
	exit 2
fi

echo "package: building $id $version ($mode)"
make -C "$app_dir" MORPTHIS_MODE="$mode" stage

stage_dir=$app_dir/build/$mode/app/$id
if [ ! -d "$stage_dir" ]; then
	echo "package: $stage_dir was not produced" >&2
	exit 1
fi
if [ ! -f "$stage_dir/$entry" ]; then
	echo "package: $stage_dir/$entry is missing" >&2
	exit 1
fi

mkdir -p "$out_dir"

# The package name and not the version: the card has exactly one package per
# app, at Apps/pkgmerps/<id>.pkgmerp, and replacing it is the update. The
# version stays in app.json, BUILD-INFO and the release tag.
out=$out_dir/$id.pkgmerp
rm -f "$out"

# -noI -noD -noF -noX stores the inode table, data blocks, fragments and xattrs
# without compression, which is what keeps the kernel from decompressing on the
# way to the page cache. -comp gzip names the metadata compressor the device's
# squashfs 4.0 driver is guaranteed to know. -all-root makes every entry
# root-owned, as the player runs; the mode bits the build set are kept.
if ! mksquashfs "$stage_dir" "$out" \
	-noappend -noI -noD -noF -noX -comp gzip -all-root; then
	echo "package: mksquashfs failed for $id" >&2
	rm -f "$out"
	exit 1
fi

# Cheap proof that the image has a superblock the kernel will accept.
if command -v unsquashfs >/dev/null 2>&1; then
	if ! unsquashfs -s "$out" >/dev/null 2>&1; then
		echo "package: $out is not a readable squashfs image" >&2
		rm -f "$out"
		exit 1
	fi
fi

echo "package: wrote $out"
