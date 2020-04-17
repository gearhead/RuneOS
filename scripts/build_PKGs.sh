#!/bin/bash
# Script to build PKGs
# Go to packages directory
cd ../packages
echo "alac"
cd libalac-rune
makepkg -f
rm -rf src/ pkg/ alac/
echo "amixer-webui"
cd ../amixer-webui-rune
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
rm -rf src/ pkg/ I-Sabre-K2M/
echo "libnpupnp"
cd ../libnpupnp
makepkg -f
rm -rf src/ pkg/
echo "libUPnPP"
cd ../libupnpp
#git pull
makepkg -f
rm -rf src/ pkg/
echo "nginx"
cd ../nginx-rune
makepkg -f
rm -rf src/ pkg/
echo "ply-lite"
cd ../ply-lite-rune
makepkg -f
rm -rf src/ pkg/ ply-lite/
echo "PHP-phpiredis"
cd ../php-phpiredis-rune
makepkg -f
rm -rf src/ pkg/ phpiredis/
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
#echo "Deleting build folders"
#rm -rf /tmp/makepkg/*
echo "Done with RuneAudio packages!"
