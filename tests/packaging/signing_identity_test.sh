#!/bin/bash
# Verifies scripts/create_signing_identity.sh (issue #71) against a throwaway keychain, never the login one.
# Usage: signing_identity_test.sh <repo root>
set -u
repo_root="${1:?usage: signing_identity_test.sh <repo root>}"
work_dir="$(mktemp -d)"
keychain="${work_dir}/test.keychain-db"
identity="Overdub Test Identity"
failures=0

# codesign only finds identities in keychains on the user search list, so the throwaway keychain
# joins it for the test and the original list is restored on exit.
original_search_list=()
while IFS= read -r line; do
    line="${line#"${line%%[![:space:]]*}"}"
    original_search_list+=("${line//\"/}")
done < <(security list-keychains -d user)

cleanup ()
{
    security list-keychains -d user -s "${original_search_list[@]}"
    security delete-keychain "${keychain}" > /dev/null 2>&1
    rm -rf "${work_dir}"
}
trap cleanup EXIT

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

security create-keychain -p test "${keychain}"
security unlock-keychain -p test "${keychain}"
security list-keychains -d user -s "${keychain}" "${original_search_list[@]}"

identity_listed ()
{
    security find-identity -p codesigning "${keychain}" | grep -F "\"${identity}\""
}

identity_count_is_one ()
{
    [ "$(security find-identity -p codesigning "${keychain}" | grep -cF "\"${identity}\"")" -eq 1 ]
}

signs_and_verifies ()
{
    cp /usr/bin/true "${work_dir}/probe"
    codesign --force --sign "${identity}" --timestamp=none "${work_dir}/probe" \
        && codesign --verify --strict "${work_dir}/probe" \
        && codesign -dvv "${work_dir}/probe" 2>&1 | grep -F "Authority=${identity}"
}

check "script creates the identity" env OVERDUB_SIGNING_KEYCHAIN="${keychain}" OVERDUB_SIGNING_KEYCHAIN_PASSWORD=test \
    "${repo_root}/scripts/create_signing_identity.sh" "${identity}"
check "identity is listed for code signing" identity_listed
check "codesign signs with it and the signature verifies" signs_and_verifies
# TCC keys camera and microphone grants on the designated requirement: it must name the certificate,
# not the code hash, so a rebuilt binary signed with the same identity keeps the grants.
designated_requirement ()
{
    codesign -d -r- "$1" 2>&1 | grep '^designated =>'
}

requirement_survives_rebuild ()
{
    cp /usr/bin/true "${work_dir}/build1"
    cp /usr/bin/false "${work_dir}/build2"
    codesign --force --sign "${identity}" --identifier com.mbarbieri.overdub --timestamp=none "${work_dir}/build1" || return 1
    codesign --force --sign "${identity}" --identifier com.mbarbieri.overdub --timestamp=none "${work_dir}/build2" || return 1
    local first second
    first="$(designated_requirement "${work_dir}/build1")"
    second="$(designated_requirement "${work_dir}/build2")"
    [ -n "${first}" ] && [ "${first}" = "${second}" ] && ! echo "${first}" | grep -q cdhash
}

check "a rebuilt binary keeps the same designated requirement" requirement_survives_rebuild
check "running the script again succeeds" env OVERDUB_SIGNING_KEYCHAIN="${keychain}" OVERDUB_SIGNING_KEYCHAIN_PASSWORD=test \
    "${repo_root}/scripts/create_signing_identity.sh" "${identity}"
check "running it again keeps one identity" identity_count_is_one

if [ "${failures}" -ne 0 ]; then
    echo "${failures} check(s) failed"
    exit 1
fi
echo "all checks passed"
