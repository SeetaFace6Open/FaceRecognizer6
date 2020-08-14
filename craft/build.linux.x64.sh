#!/usr/bin/env bash

export BUILD_DIR=build.linux.x64
export BUILD_TYPE=Release
export PLATFORM_TARGET=x64

export PLATFORM=x64
export ORZ_HOME=/usr/local

export SSL_HOME=/usr

HOME=$(cd `dirname $0`; pwd)

cd $HOME

mkdir "$BUILD_DIR"

cd "$BUILD_DIR"


cmake "$HOME/.." \
-DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
-DCONFIGURATION="$BUILD_TYPE" \
-DPLATFORM="$PLATFORM_TARGET" \
-DOPENSSL_ROOT_DIR="$SSL_HOME" \
-DORZ_ROOT_DIR="$ORZ_HOME" \
-DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
-DCMAKE_MODULE_PATH="/wqy/seeta_sdk/sdk/sdk6.0/common/tensorstack/cmake;/wqy/seeta_sdk/sdk/sdk6.0/common/seetaauthorize/cmake" \
-DSEETA_AUTHORIZE=OFF \
-DSEETA_MODEL_ENCRYPT=ON

make -j16
#make install

