#!/bin/sh
# Xcode Cloud: when an archive is signed for TestFlight, write the "What to
# Test" notes from the commits that went into this build, followed by the
# standing instructions in TestFlight/WhatToTest.*.txt.
set -eu
[ -n "${CI_APP_STORE_SIGNED_APP_PATH:-}" ] || exit 0
cd "$CI_PRIMARY_REPOSITORY_PATH"

CHANGES=$(git log -12 --no-merges --pretty='• %s' 2>/dev/null || true)
for LANG_CODE in pt-BR en-US; do
    FILE="TestFlight/WhatToTest.$LANG_CODE.txt"
    BASE=$(cat "$FILE")
    if [ "$LANG_CODE" = "pt-BR" ]; then TITLE="Mudanças neste build"; else TITLE="Changes in this build"; fi
    {
        echo "$TITLE ($CI_PRODUCT_PLATFORM · build $CI_BUILD_NUMBER · ${CI_COMMIT:-local}):"
        echo "$CHANGES"
        echo
        echo "$BASE"
    } | cut -c1-200 | head -c 3900 > "$FILE"
done
