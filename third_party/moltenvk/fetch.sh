#!/bin/sh
# 拉取 MoltenVK iOS 静态库（官方 release 的 iOS 包只含真机 arm64 slice，
# 无模拟器 slice；模拟器需自行从源码构建 MoltenVK）。
set -eu

version="v1.4.2"
dest=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

echo "downloading MoltenVK ${version} (iOS) ..."
curl -sL -o "$tmp/moltenvk.tar" \
    "https://github.com/KhronosGroup/MoltenVK/releases/download/${version}/MoltenVK-ios.tar"
tar -xf "$tmp/moltenvk.tar" -C "$tmp"

rm -rf "$dest/MoltenVK.xcframework" "$dest/include"
cp -R "$tmp/MoltenVK/MoltenVK/static/MoltenVK.xcframework" "$dest/"
cp -R "$tmp/MoltenVK/MoltenVK/include" "$dest/"
cp "$tmp/MoltenVK/LICENSE" "$dest/LICENSE"
echo "done: $dest/MoltenVK.xcframework"
