#!/bin/bash
# Script to build AURs
# Go to AUR directory
cd ../../AUR/ 
echo "alsa-equal"
cd alsaequal
makepkg -f
rm -rf src/ pkg/
echo "Ashuffle"
cd ../ashuffle
#git pull
makepkg -f
rm -rf src/ pkg/ ashuffle/
echo "Bluez-alsa"
cd ../bluez-alsa-git
#git pull
makepkg -f
rm -rf src/ pkg/ bluez-alsa-git/
echo "Blues-utils-compat"
cd ../bluez-utils-compat
#git pull
makepkg -f
rm -rf src/ pkg/
echo "djmount"
cd ../djmount
makepkg -f
rm -rf src/ pkg/
echo "HFSprogs"
cd ../hfsprogs
#git pull
makepkg -f
rm -rf src/ pkg/
echo "MPDscribble"
cd ../mpdscribble
#git pull
makepkg -f
rm -rf src/ pkg/
echo "pi-bluetooth"
cd ../pi-bluetooth
#git pull 
makepkg -f
rm -rf src/ pkg/
echo "snapcast"
cd ../snapcast
#git pull
makepkg -f
rm -rf src/ pkg/
echo "Spotifyd"
cd ../spotifyd
#git pull
makepkg -f
rm -rf src/ pkg/
echo "Librespot"
cd ../librespot-git
#git pull
makepkg -f
rm -rf src/ pkg/ librespot/
echo "end building AURs"
