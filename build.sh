#!/bin/sh
# Builds FunkinDS.nds with the official devkitPro docker image.
#   ./build.sh path/to/Funkin.assets    convert the assets, then build
#   ./build.sh                          build only (assets already converted)
set -e
if [ -n "$1" ] && [ -d "$1" ]; then
	python3 tools/convert.py "$1"
	shift
fi
docker run --rm -v "$(pwd)":/src -w /src devkitpro/devkitarm make "$@"
