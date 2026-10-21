#!/bin/sh
# Builds one app for the device and packs it into a .mapp.
#
#     tools/package.sh apps/calculator [dist]
#
# The device build needs the pinned cross toolchain, exactly as `make device`
# does. MORPTHIS_MODE can be overridden for a host-only dry run, but a .mapp
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
out=$out_dir/$id-$version.mapp

# Zip the contents of the staging folder at the archive root, so the package is
# exactly Apps/<id>/. Sorted entries keep diffs of two builds stable.
python3 - "$stage_dir" "$out" <<'PY'
import os
import sys
import zipfile

stage, out = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as archive:
    for root, dirs, files in os.walk(stage):
        dirs.sort()
        for name in sorted(files):
            path = os.path.join(root, name)
            archive.write(path, os.path.relpath(path, stage))
print(f"package: wrote {out}")
PY
