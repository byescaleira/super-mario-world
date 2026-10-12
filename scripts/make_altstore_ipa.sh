#!/bin/bash
# Builds build/altstore/Mario.ipa for personal sideloading with AltStore Classic
# (AltStore re-signs it with the Apple ID used on the device).
#
# The IPA embeds the ROM assembled by build_rom.sh: it is for your own devices
# only. Never publish it or attach it to a release.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/altstore"
TEAM="Q7QLR2PH2X"
BUNDLE="com.byescaleira.smw"

"$ROOT/scripts/build_rom.sh"
rm -rf "$OUT/Mario.xcarchive" "$OUT/Payload" "$OUT/Mario.ipa"
mkdir -p "$OUT"

xcodebuild -project "$ROOT/SuperMarioWorld.xcodeproj" -scheme SuperMarioWorld -configuration Release \
  -destination 'generic/platform=iOS' -archivePath "$OUT/Mario.xcarchive" \
  -derivedDataPath "$OUT/DerivedData" CODE_SIGNING_ALLOWED=NO archive -quiet

APP="$OUT/Mario.xcarchive/Products/Applications/Mario.app"
[ -f "$APP/smw.sfc" ] || { echo "error: ROM missing from the app bundle" >&2; exit 1; }

# Ad-hoc signature carrying the app's entitlements, so AltStore can see which
# capabilities it asks for (Game Center, SharePlay, iCloud key-value store).
cat > "$OUT/entitlements.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>application-identifier</key>
	<string>$TEAM.$BUNDLE</string>
	<key>com.apple.developer.team-identifier</key>
	<string>$TEAM</string>
	<key>com.apple.developer.game-center</key>
	<true/>
	<key>com.apple.developer.group-session</key>
	<true/>
	<key>com.apple.developer.ubiquity-kvstore-identifier</key>
	<string>$TEAM.$BUNDLE</string>
</dict>
</plist>
EOF
codesign --force --deep --sign - --entitlements "$OUT/entitlements.plist" --timestamp=none "$APP"

mkdir -p "$OUT/Payload"
cp -R "$APP" "$OUT/Payload/"
(cd "$OUT" && zip -qry Mario.ipa Payload)
rm -rf "$OUT/Payload"
echo "IPA: $OUT/Mario.ipa ($(du -h "$OUT/Mario.ipa" | cut -f1))"
