#!/bin/bash
# Script to build AURs
echo "Alac"
cd alac-git
makepkg -f
rm -rf src/ pkg/  alac/
echo "Ashuffle"
cd ../ashuffle-git
makepkg -f
rm -rf src/ pkg/ ashuffle/
echo "Bluez-alsa"
cd ../bluez-alsa-git
makepkg -f
rm -rf src/ pkg/ bluez-alsa-git/
echo "Blues-utils-compat"
cd ../bluez-utils-compat
makepkg -f
rm -rf src/ pkg/
echo "HFSprogs"
cd ../hfsprogs
makepkg -f
rm -rf src/ pkg/
echo "libUPnPP"
cd ../libupnpp
makepkg -f
rm -rf src/ pkg/
echo "MPDscribble"
cd ../mpdscribble
makepkg -f
rm -rf src/ pkg/
echo "PHP-event"
cd ../php-event
makepkg -f
#rm -rf src/ pkg/
echo "PHP-phpiredis"
cd ../php-phpiredis
makepkg -f
rm -rf src/ pkg/
echo "pi-bluetooth"
cd ../pi-bluetooth
makepkg -f
rm -rf src/ pkg/
echo "snapcast"
cd ../snapcast
makepkg -f
rm -rf src/ pkg/
echo "Spotifyd"
cd ../spotifyd
makepkg -f
rm -rf src/ pkg/
echo "end"
