#!/bin/bash
set -e

LIB_VERSION_TAG="v01.08.00.57"

OS=$(uname -s)

CONFIG_DIR="$HOME/Library/Application Support/BambuStudioBeta"
LIB_EXT="dylib"


if [ "Darwin" == "$OS" ]
then
	echo "macos"
elif [ "Linux" == "$OS" ]
then
	echo "linux"
	CONFIG_DIR="$HOME/.config/BambuStudio"
	LIB_EXT="so"
fi

echo "Config Dir = ${CONFIG_DIR}"


echo "setting up build dir..."
mkdir -p build

cp BambuStudioWebHookProxy.cpp build/BambuStudioWebHookProxy.cpp
cp HTTPRequest.hpp build/HTTPRequest.hpp

echo "downloading dependencies..."

if ! test -f build/bambu_networking.hpp; then
	curl "https://raw.githubusercontent.com/bambulab/BambuStudio/${LIB_VERSION_TAG}/src/slic3r/Utils/bambu_networking.hpp" -o build/bambu_networking.hpp
fi
if ! test -f build/ProjectTask.hpp; then
	curl "https://raw.githubusercontent.com/bambulab/BambuStudio/${LIB_VERSION_TAG}/src/libslic3r/ProjectTask.hpp" -o build/ProjectTask.hpp
fi

cd build


echo "building..."
g++ -std=c++17 -shared -fPIC -DCONFIG_DIR="\"$CONFIG_DIR\"" -o "BambuStudioWebHookProxy.$LIB_EXT" BambuStudioWebHookProxy.cpp  

echo "moving original library"
if ! test -f "$CONFIG_DIR/plugins/o_libbambu_networking.$LIB_EXT"; then
	mv "$CONFIG_DIR/plugins/libbambu_networking.$LIB_EXT" "$CONFIG_DIR/plugins/o_libbambu_networking.$LIB_EXT"
	echo "moved original library to $CONFIG_DIR/plugins/o_libbambu_networking.$LIB_EXT"
else
	echo "$CONFIG_DIR/plugins/o_libbambu_networking.$LIB_EXT exists"
fi

echo "installing proxy library"
cp "BambuStudioWebHookProxy.$LIB_EXT" "$CONFIG_DIR/plugins/libbambu_networking.$LIB_EXT"


echo "done"