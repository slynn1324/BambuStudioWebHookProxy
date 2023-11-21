#!/bin/bash
set -e

echo "setting up build dir..."
mkdir -p build

cp BambuStudioWebHookProxy.cpp build/BambuStudioWebHookProxy.cpp
cp HTTPRequest.hpp build/HTTPRequest.hpp

echo "downloading dependencies..."

if ! test -f build/bambu_networking.hpp; then
	curl 'https://raw.githubusercontent.com/bambulab/BambuStudio/v01.08.00.57/src/slic3r/Utils/bambu_networking.hpp' -o build/bambu_networking.hpp
fi
if ! test -f build/ProjectTask.hpp; then
	curl 'https://raw.githubusercontent.com/bambulab/BambuStudio/v01.08.00.57/src/libslic3r/ProjectTask.hpp' -o build/ProjectTask.hpp
fi

cd build


echo "building..."
g++ -std=c++17 -shared -o BambuStudioWebHookProxy.dylib BambuStudioWebHookProxy.cpp  

if ! test -f ~/Library/Application\ Support/BambuStudioBeta/plugins/o_libbambu_networking.dylib; then
	echo "moving original library"
	mv ~/Library/Application\ Support/BambuStudioBeta/plugins/libbambu_networking.dylib ~/Library/Application\ Support/BambuStudioBeta/plugins/o_libbambu_networking.dylib
else
	echo "~/Library/Application\ Support/BambuStudioBeta/plugins/o_libbambu_networking.dylib exists"
fi

echo "installing proxy library"
cp BambuStudioWebHookProxy.dylib ~/Library/Application\ Support/BambuStudioBeta/plugins/libbambu_networking.dylib


echo "done"