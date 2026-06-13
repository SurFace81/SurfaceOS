#!/bin/bash

OUTPUT="src/kernel/version.h"

# Try to get version from the latest tag
TAG=$(git describe --tags --abbrev=0 2>/dev/null)

if [ -z "$TAG" ]; then
    # No tags exist yet — use default version
    MAJOR=0
    MINOR=0
    PATCH=1
    COMMITS=$(git rev-list --count HEAD 2>/dev/null || echo "0")
else
    # Strip leading 'v' if present (v1.2.3 -> 1.2.3)
    VERSION="${TAG#v}"
    MAJOR=$(echo "$VERSION" | cut -d. -f1)
    MINOR=$(echo "$VERSION" | cut -d. -f2)
    PATCH=$(echo "$VERSION" | cut -d. -f3)
    COMMITS=$(git rev-list --count "${TAG}..HEAD" 2>/dev/null || echo "0")
fi

# Only regenerate if version changed
HEADER_CONTENT="#ifndef VERSION_H
#define VERSION_H

#define VERSION_MAJOR ${MAJOR}
#define VERSION_MINOR ${MINOR}
#define VERSION_PATCH ${PATCH}
#define VERSION_BUILD ${COMMITS}

#define VERSION_STRING \"${MAJOR}.${MINOR}.${PATCH}.${COMMITS}\"

#endif"

# Avoid unnecessary rebuilds
if [ -f "$OUTPUT" ]; then
    EXISTING=$(cat "$OUTPUT")
    if [ "$EXISTING" = "$HEADER_CONTENT" ]; then
        exit 0
    fi
fi

echo "$HEADER_CONTENT" > "$OUTPUT"
