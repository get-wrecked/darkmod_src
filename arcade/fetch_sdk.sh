#!/bin/sh
# Downloads the pinned arcade SDK release and unpacks what we need:
#   ThirdParty/arcade_sdk/linux_64/libarcade_sdk.so   (shipped next to the executable; not committed)
#   arcade/tools/arcade-sdk, arcade-sdk-debug          (the submission CLI and the debug app; not committed)
# The header, protos and guide in ThirdParty/arcade_sdk ARE committed; when bumping
# SDK_ZIP, diff those against the archive and update them (and the engine, if the
# InitRequest contract changed) in the same commit.
#
# Needs gsutil authenticated to an account that may read the bucket.
set -eu

SDK_ZIP=${SDK_ZIP:-"gs://gi-prod-games-cluster-app/arcade_sdk-linux-fa788829d12ff6034665da29e1f2bff052068806.zip"}

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo "fetching $SDK_ZIP"
gsutil -q cp "$SDK_ZIP" "$TMP/sdk.zip"
unzip -q "$TMP/sdk.zip" -d "$TMP/sdk"

mkdir -p "$REPO/ThirdParty/arcade_sdk/linux_64" "$HERE/tools"
cp "$TMP/sdk/libarcade_sdk.so" "$REPO/ThirdParty/arcade_sdk/linux_64/libarcade_sdk.so"
cp "$TMP/sdk/arcade-sdk" "$TMP/sdk/arcade-sdk-debug" "$HERE/tools/"
chmod +x "$HERE/tools/arcade-sdk" "$HERE/tools/arcade-sdk-debug"

for f in include/arcade_sdk.h proto/arcade_sdk.proto proto/arcade_common.proto; do
	if ! cmp -s "$TMP/sdk/$f" "$REPO/ThirdParty/arcade_sdk/$f"; then
		echo "NOTE: $f differs from the committed copy; review and update ThirdParty/arcade_sdk (and vendor_pb.h / the engine if needed)"
	fi
done
cmp -s "$TMP/sdk/README.md" "$REPO/ThirdParty/arcade_sdk/README_SDK.md" || echo "NOTE: README.md differs from ThirdParty/arcade_sdk/README_SDK.md"

echo "SDK library: $(strings -n 8 "$REPO/ThirdParty/arcade_sdk/linux_64/libarcade_sdk.so" | grep -oE '0\.[0-9]+\.[0-9]+\+[0-9a-f]{12}' | head -1)"
echo "CLI:         $HERE/tools/arcade-sdk"
