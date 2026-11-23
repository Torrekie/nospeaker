#!/bin/sh
# Builds nospeaker as one program for Apple Silicon and Intel Macs.
#
#   ./build.sh            build ./nospeaker (signed for this Mac only)
#   ./build.sh release    also sign it with your Developer ID, have Apple notarize it, and make
#                         dist/nospeaker.zip, which runs on any Mac without security warnings
set -e
mode="$1"
cd "$(dirname "$0")"

fail() {
    printf '\n  nospeaker: %s\n\n' "$1" >&2
    exit 1
}

[ "$(uname -s)" = "Darwin" ] || fail "this builds on macOS only (it drives a Mac's trackpad)."

sdk="${SDKROOT:-$(xcrun --sdk macosx --show-sdk-path 2>/dev/null)}" || sdk=""
[ -d "$sdk" ] || fail "The macOS SDK wasn't found. Install Apple's Command Line Tools:

    xcode-select --install

  then run ./build.sh again."

mkdir -p build
build_slice() {
    arch="$1"
    minimum="$2"
    echo "building for $arch (macOS $minimum and later)…"
    xcrun clang -std=gnu11 -O2 -ffp-contract=off -isysroot "$sdk" -target "$arch-apple-macos$minimum" \
        -framework CoreFoundation -framework AudioToolbox -framework CoreAudio -framework IOKit \
        nospeaker.c -o "build/nospeaker-$arch" || fail "the $arch build failed (errors above)."
}
build_slice arm64 11.0
build_slice x86_64 10.10.3

lipo -create build/nospeaker-arm64 build/nospeaker-x86_64 -output build/nospeaker
codesign -s - -f build/nospeaker 2>/dev/null
mv -f build/nospeaker nospeaker     # a rename, so a copy that's already running isn't disturbed
echo "built ./nospeaker ($(lipo -archs nospeaker)). Run it with: ./nospeaker"

[ "$mode" = "release" ] || exit 0

# Release: Developer ID signature (with the hardened runtime Apple requires), then notarization.
identity=$(security find-identity -v -p codesigning | sed -n 's/.*"\(Developer ID Application:[^"]*\)".*/\1/p' | head -n 1)
[ -n "$identity" ] || fail "no Developer ID Application certificate in your keychain. Add one in Xcode:
  Settings > Accounts > your team > Manage Certificates > + > Developer ID Application,
  then run ./build.sh release again."

profile="${NOTARY_PROFILE:-nospeaker}"
xcrun notarytool history --keychain-profile "$profile" >/dev/null 2>&1 || fail "no saved notary sign-in called \"$profile\". Save one once (it asks for an app-specific password,
  made at account.apple.com > Sign-In and Security > App-Specific Passwords):

    xcrun notarytool store-credentials $profile --apple-id <your Apple ID> --team-id <your team ID>

  then run ./build.sh release again."

echo "signing with $identity…"
codesign --force --options runtime --timestamp --sign "$identity" nospeaker
codesign --verify --strict nospeaker

mkdir -p dist
rm -f dist/nospeaker.zip
ditto -c -k --norsrc --noextattr nospeaker dist/nospeaker.zip    # the program alone, at the top of the zip

echo "sending to Apple for notarization (usually a minute or two)…"
xcrun notarytool submit dist/nospeaker.zip --keychain-profile "$profile" --wait \
    || fail "notarization didn't pass. See why with: xcrun notarytool log <submission id> --keychain-profile $profile"

codesign --verify --strict -R="notarized" --check-notarization nospeaker \
    || fail "Apple accepted it, but macOS can't confirm the notarization yet. Try again in a minute."
echo "released: dist/nospeaker.zip, signed and notarized. Attach it to the GitHub release."
