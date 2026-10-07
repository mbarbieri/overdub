#!/bin/zsh
# PROTOTYPE — builds the issue #7 spike. Usage: spikes/av-sync/build.sh [Debug|Release]
set -euo pipefail
SPIKE_DIR=${0:A:h}
ROOT=${SPIKE_DIR:h:h}
CONFIG=${1:-Release}
BUILD_DIR=$ROOT/build/spike-av-sync-$CONFIG

cmake -S $SPIKE_DIR -B $BUILD_DIR -G Ninja -DCMAKE_BUILD_TYPE=$CONFIG
cmake --build $BUILD_DIR
echo "build: app at $BUILD_DIR/OverdubAvSpike_artefacts/$CONFIG/Overdub AV Spike.app"
