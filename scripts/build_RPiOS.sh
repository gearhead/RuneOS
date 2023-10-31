#!/bin/bash
# Script to build PKGs
# Go to packages directory
cd ../rpios_packages
echo "alac"
cd libalac
makedeb -s --no-confirm
rm -rf src/ pkg/ alac/
echo "bluez-alsa"
cd ../bluez-alsa
makedeb -s --no-confirm
rm -rf src/ pkg/ bluez-alsa-git/
echo "bluez-alsa-monitor"
cd ../bluez-alsa-monitor
makedeb -s --no-confirm
rm -rf src/ pkg/ bluealsa-monitor/
echo "ashuffle"
cd ../ashuffle
makedeb -s --no-confirm
rm -rf src/ pkg/ ashuffle/
echo "connman"
cd ../connman
makedeb -s --no-confirm
rm -rf src/ pkg/
echo "ell"
cd ../ell
makedeb -s --no-confirm
rm -rf src/ pkg/
echo "iwd"
cd ../iwd
makedeb -s --no-confirm
rm -rf src/ pkg/
echo "nginx"
cd ../nginx-rune
makedeb -s --no-confirm
rm -rf src/ pkg/
echo "ply-lite"
cd ../ply-lite
makedeb -s --no-confirm
rm -rf src/ pkg/ ply-lite/
echo "rsgain"
cd ../rsgain
makedeb -s --no-confirm
rm -rf src/ pkg/
echo "openaptx"
cd ../openaptx
makedeb -s --no-confirm
rm -rf src/ pkg/ libopenaptx/
#echo "mpd"
#cd ../mpd
#makedeb -s --no-confirm
#rm -rf src/ pkg/
echo "shairport-sync"
cd ../shairport-sync
makedeb -s --no-confirm
rm -rf src/ pkg/
echo "nqptp"
cd ../nqptp
makedeb -s --no-confirm
rm -rf src/ pkg/ nqptp/
echo "librespot"
cd ../librespot
makedeb -s --no-confirm
rm -rf src/ pkg/ librespot/
echo "spotifyd"
cd ../spotifyd
makedeb -s --no-confirm
rm -rf src/ pkg/ spotifyd/
echo "Done with RPiOS RuneAudio packages!"
