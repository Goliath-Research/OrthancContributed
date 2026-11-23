#!/bin/bash
# Runs the Weasis launcher code-execution tests.
# These tests fail while the published launcher still fetches code over
# unauthenticated HTTP. They pass once those downloads are rejected.
set -euo pipefail
cd "$(dirname "$0")"
ROOT="$(cd ../.. && pwd)"
mkdir -p lib build
FELIX_JAR="lib/org.apache.felix.framework-4.2.1.jar"
if [[ ! -f "$FELIX_JAR" ]]; then
  curl -fsSL -o "$FELIX_JAR" \
    https://repo1.maven.org/maven2/org/apache/felix/org.apache.felix.framework/4.2.1/org.apache.felix.framework-4.2.1.jar
fi
javac --release 8 -encoding UTF-8 -cp "$FELIX_JAR" -d build WeasisHttpCodeExecutionTest.java
javac --release 8 -encoding UTF-8 -cp "$FELIX_JAR" -d build marker/MarkerActivator.java
jar --create --file build/marker.jar --manifest marker/MANIFEST.MF -C build marker/MarkerActivator.class
java \
  --add-opens java.base/java.net=ALL-UNNAMED \
  --add-opens java.base/java.security=ALL-UNNAMED \
  --add-opens java.base/java.lang=ALL-UNNAMED \
  -cp "build:${FELIX_JAR}" WeasisHttpCodeExecutionTest "$ROOT" build/marker.jar
