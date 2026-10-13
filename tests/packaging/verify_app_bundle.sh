#!/bin/bash
# Verifies a built Overdub.app against issue #72's acceptance bullets.
# Usage: verify_app_bundle.sh <path to Overdub.app> <repo root>
set -u
app="${1:?usage: verify_app_bundle.sh <Overdub.app> <repo root>}"
repo_root="${2:?usage: verify_app_bundle.sh <Overdub.app> <repo root>}"
failures=0

check ()
{
    local description="$1"
    shift
    if "$@" > /dev/null 2>&1; then
        echo "ok   - ${description}"
    else
        echo "FAIL - ${description}"
        failures=$((failures + 1))
    fi
}

plist="${app}/Contents/Info.plist"
binary="${app}/Contents/MacOS/Overdub"

plist_value_is ()
{
    [ "$(plutil -extract "$1" raw -o - "${plist}")" = "$2" ]
}

plist_value_nonempty ()
{
    [ -n "$(plutil -extract "$1" raw -o - "${plist}")" ]
}

binary_has_only_x86_64 ()
{
    [ "$(lipo -archs "${binary}")" = "x86_64" ]
}

binary_has_no_build_tree_paths ()
{
    ! otool -L "${binary}" | grep -E -e "${repo_root}" -e "/build/"
}

bundle_has_no_build_tree_paths ()
{
    ! grep -rlF --binary-files=text -e "${repo_root}/build" "${app}/Contents/Info.plist" "${app}/Contents/PkgInfo" 2> /dev/null
}

check "bundle exists" test -d "${app}"
check "binary exists" test -x "${binary}"
check "bundle id is com.mbarbieri.overdub" plist_value_is CFBundleIdentifier com.mbarbieri.overdub
check "version is set" plist_value_nonempty CFBundleShortVersionString
check "camera usage description is set" plist_value_nonempty NSCameraUsageDescription
check "microphone usage description is set" plist_value_nonempty NSMicrophoneUsageDescription
check "icon is bundled" test -n "$(ls "${app}"/Contents/Resources/*.icns 2> /dev/null)"
check "binary is x86_64 only" binary_has_only_x86_64
check "otool -L shows no build tree or repo paths" binary_has_no_build_tree_paths
check "Info.plist has no build tree paths" bundle_has_no_build_tree_paths
check "codesign --verify --deep --strict passes" codesign --verify --deep --strict "${app}"

if [ "${failures}" -ne 0 ]; then
    echo "${failures} check(s) failed for ${app}"
    exit 1
fi
echo "all checks passed for ${app}"
