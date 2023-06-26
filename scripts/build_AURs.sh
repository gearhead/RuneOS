#!/bin/bash
# Script to build AURs
# Go to AUR directory
# build all of these regardless
cd ../../AUR/ 
echo "alsa-equal"
cd alsaequal
makepkg -f
rm -rf src/ pkg/
echo "Ashuffle"
cd ../ashuffle-git
#git pull
makepkg -f
rm -rf src/ pkg/ ashuffle/ abseil-cpp/ googletest/
echo "bluez-alsa"
cd ../bluez-alsa-git
makepkg -f
rm -rf src/ pkg/ bluez-alsa-git/
echo "openAPTx"
cd ../libopenaptx-git
makepkg -f
rm -rf src/ pkg/ libopenaptx/
echo " Build Dynamic Room Correction"
cd ../drc
makepkg -f
rm -rf src/ pkg/
echo "HFSprogs"
cd ../hfsprogs
#git pull
makepkg -f
rm -rf src/ pkg/
echo "Librespot"
cd ../librespot-git
#git pull
makepkg -f
rm -rf src/ pkg/ librespot/
echo "MPDscribble"
cd ../mpdscribble
#git pull
makepkg -f
rm -rf src/ pkg/
echo "snapcast"
cd ../snapcast
#git pull
makepkg -f
rm -rf src/ pkg/
echo "loudgain"
cd ../loudgain
#git pull
makepkg -f
rm -rf src/ pkg/
echo "Snapcast"
cd ../snapcast
#git pull
makepkg -f
rm -rf src/ pkg/
echo "llmnrd"
cd ../llmnrd-git
#git pull
makepkg -f
rm -rf src/ pkg/ llmnrd/
echo "Udevil"
cd ../udevil
#git pull
makepkg -f
rm -rf src/ pkg/ udevil-0.4.4/
echo "nqptp"
cd ../nqptp-git
makepkg -f
rm -rf src/ pkg/ nqptp/

# build these for armv7 and aarch64
ARCH=$(pacman -Qi bash | grep 'Architecture' | cut -c 19-)
echo $ARCH
if [ $ARCH == 'armv7h' ] || [ $ARCH == 'aarch64' ]
then
  echo " Build libmatchbox"
  cd ../libmatchbox
  makepkg -f
  rm -rf src/ pkg/
#  echo " Build Matchbox WM"
#  cd ../matchbox-window-manager
#  makepkg -f
#  rm -rf src/ pkg/
  echo " Build Matchbox OSK"
  cd ../matchbox-keyboard
  makepkg -f
  rm -rf src/ pkg/
fi
echo "spotifyd"
cd ../spotifyd-full-git
makepkg -f
rm -rf src/ pkg/ spotifyd/
#echo "fbturbo"
#cd ../xf86-video-fbturbo-git
#makepkg -f
#rm -rf src/ pkg/ xf86-video-fbturbo/
echo "end building AURs"
