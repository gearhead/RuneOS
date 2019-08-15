#!/bin/bash
# Script to build PKGs
echo "amixer-webui"
cd amixer-webui-rune
makepkg -f
rm -rf src/ pkg/
echo "brutefir"
cd ../brutefir
makepkg -f
rm -rf src/ pkg/
echo "HFSutils"
cd ../hfsutils-rune
makepkg -f
rm -rf src/ pkg/
echo "Sabre K2M"
cd ../i-sabre-k2m-rune
makepkg -f
rm -rf src/ pkg/
echo "nginx"
cd ../nginx-rune
makepkg -f
rm -rf src/ pkg/
echo "ply-lite"
cd ../ply-lite-rune
makepkg -f
rm -rf src/ pkg/
echo "shairport-sync"
cd ../shairport-sync-rune
makepkg -f
rm -rf src/ pkg/
echo "upmpdcli"
cd ../upmpdcli-rune
makepkg -f
rm -rf src/ pkg/
echo "linux"
cd ../linux-raspberrypi-rune
makepkg -f
rm -rf src/ pkg/
echo "done with rune packages"
