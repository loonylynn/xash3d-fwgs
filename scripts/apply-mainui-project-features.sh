#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
MAINUI="$ROOT/3rdparty/mainui"
PATCH="$ROOT/patches/mainui/0001-project-features-menu.patch"

if [ ! -d "$MAINUI" ]; then
    echo "mainui submodule is missing. Run: git submodule update --init 3rdparty/mainui" >&2
    exit 1
fi

if [ ! -f "$PATCH" ]; then
    echo "Project-features patch not found: $PATCH" >&2
    exit 1
fi

if git -C "$MAINUI" apply --check "$PATCH"; then
    git -C "$MAINUI" apply "$PATCH"
    echo "Applied the project-features settings menu to 3rdparty/mainui."
elif [ -f "$MAINUI/menus/ProjectFeatures.cpp" ] &&
     grep -q "UI_ProjectFeatures_Menu" "$MAINUI/menus/Video.cpp"; then
    echo "Project-features settings menu is already applied."
else
    echo "Could not apply the menu patch cleanly. No files were changed." >&2
    echo "Inspect: git -C 3rdparty/mainui status --short" >&2
    exit 1
fi
