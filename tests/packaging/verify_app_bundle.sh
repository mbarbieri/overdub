#!/bin/bash
# Verifies a built Overdub.app against issue #72's acceptance bullets, and the bundled Separation
# helper and ONNX Runtime dylib against issue #113's.
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
helper="${app}/Contents/MacOS/overdub-separate"
ort_dylib="${app}/Contents/Frameworks/libonnxruntime.1.23.2.dylib"
notices="${app}/Contents/Resources/THIRD_PARTY_NOTICES"

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

file_has_only_x86_64 ()
{
    [ "$(lipo -archs "$1")" = "x86_64" ]
}

links_without_build_tree_paths ()
{
    ! otool -L "$1" | grep -E -e "${repo_root}" -e "/build/"
}

# Every LC_RPATH is relative to the executable, so the dylib is found inside the bundle only.
rpaths_are_bundle_relative ()
{
    local rpaths
    rpaths="$(otool -l "$1" | awk '/cmd LC_RPATH/ { getline; getline; print $2 }')"
    [ -n "${rpaths}" ] && ! printf '%s\n' "${rpaths}" | grep -v -E '^@(executable|loader)_path'
}

helper_links_bundled_ort ()
{
    otool -L "${helper}" | grep -q "@rpath/libonnxruntime.1.23.2.dylib"
}

app_binary_does_not_link_ort ()
{
    ! otool -L "${binary}" | grep -q onnxruntime
}

# Running it proves dyld finds the bundled dylib: a missing model is reported as one JSON error line.
helper_runs_from_bundle ()
{
    local scratch err code
    scratch="$(mktemp -d)"
    "${helper}" "${scratch}/missing.onnx" "${scratch}/in.wav" "${scratch}/out" > /dev/null 2> "${scratch}/err"
    code=$?
    err="$(cat "${scratch}/err")"
    rm -rf "${scratch}"
    [ "${code}" -eq 1 ] && printf '%s' "${err}" | grep -q '"error":"separation.model_missing"'
}

notices_list_ort_and_pocketfft ()
{
    grep -q "ONNX Runtime" "${notices}" && grep -q "MIT License" "${notices}" && grep -q "pocketfft" "${notices}" \
        && grep -q "Signalsmith Stretch" "${notices}" && grep -q "Signalsmith Linear" "${notices}" \
        && test -s "${app}/Contents/Resources/onnxruntime-ThirdPartyNotices.txt"
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
check "overdub-separate helper exists" test -x "${helper}"
check "ONNX Runtime dylib is bundled" test -f "${ort_dylib}"
check "helper is x86_64 only" file_has_only_x86_64 "${helper}"
check "ONNX Runtime dylib is x86_64 only" file_has_only_x86_64 "${ort_dylib}"
check "helper links ONNX Runtime through @rpath" helper_links_bundled_ort
check "helper's otool -L shows no build tree or repo paths" links_without_build_tree_paths "${helper}"
check "ONNX Runtime dylib's otool -L shows no build tree or repo paths" links_without_build_tree_paths "${ort_dylib}"
check "helper's rpaths are bundle-relative" rpaths_are_bundle_relative "${helper}"
check "Overdub binary does not link ONNX Runtime" app_binary_does_not_link_ort
check "helper runs from the bundle" helper_runs_from_bundle
check "THIRD_PARTY_NOTICES lists ONNX Runtime, pocketfft and Signalsmith Stretch" notices_list_ort_and_pocketfft
check "codesign --verify --deep --strict passes" codesign --verify --deep --strict "${app}"

if [ "${failures}" -ne 0 ]; then
    echo "${failures} check(s) failed for ${app}"
    exit 1
fi
echo "all checks passed for ${app}"
