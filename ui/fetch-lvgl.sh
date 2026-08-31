#!/bin/sh
# Fetches LVGL (pinned version) into ./lvgl - not vendored into git,
# matching this project's existing convention for large fetched trees
# (see ../.gitignore). Run this once before `make`.

set -eu
LVGL_TAG=v9.2.2
cd "$(dirname "$0")"

if [ -d lvgl ]; then
    echo "lvgl/ already exists, skipping fetch (rm -rf lvgl to re-fetch)"
    exit 0
fi

git clone --depth 1 --branch "$LVGL_TAG" https://github.com/lvgl/lvgl.git lvgl
