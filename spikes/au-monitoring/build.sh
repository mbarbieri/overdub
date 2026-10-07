#!/bin/zsh
# PROTOTYPE — builds the issue #6 spike. Usage: spikes/au-monitoring/build.sh [Debug|Release]
set -euo pipefail
SPIKE_DIR=${0:A:h}
ROOT=${SPIKE_DIR:h:h}
CONFIG=${1:-Release}
BUILD_DIR=$ROOT/build/spike-au-monitoring-$CONFIG

# The JUCE patch adds OVERDUB_SPIKE_AU_OOP; apply it once, idempotently.
if git -C $ROOT/external/JUCE apply --check $SPIKE_DIR/juce-force-auv2-oop.patch 2>/dev/null; then
  git -C $ROOT/external/JUCE apply $SPIKE_DIR/juce-force-auv2-oop.patch
  echo "build: applied juce-force-auv2-oop.patch"
fi

cmake -S $SPIKE_DIR -B $BUILD_DIR -G Ninja -DCMAKE_BUILD_TYPE=$CONFIG
cmake --build $BUILD_DIR
echo "build: app at $BUILD_DIR/OverdubAuSpike_artefacts/$CONFIG/Overdub AU Spike.app"
