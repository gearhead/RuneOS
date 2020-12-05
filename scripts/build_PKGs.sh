#!/bin/bash
# Script to build PKGs
# Go to packages directory
cd ../packages
echo "alac"
cd libalac-rune
makepkg -f
rm -rf src/ pkg/ alac/
echo "bluez-alsa"
cd ../bluealsa-rune
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
echo "libnpupnp"
cd ../libnpupnp
makepkg -f
rm -rf src/ pkg/
#echo "libUPnPP"
#cd ../libupnpp
#git pull
#makepkg -f
#rm -rf src/ pkg/
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
#echo "upmpdcli"
#cd ../upmpdcli-rune
#makepkg -f
#rm -rf src/ pkg/
ARCH=$(pacman -Qi bash | grep 'Architecture' | cut -c 19-)
echo $ARCH
case $ARCH in
armv6h)
  echo "linux-raspberrypi-armv6"
  cd ../linux-raspberrypi-rune
  makepkg -f
  rm -rf src/ pkg/
  ;;
armv7h)
  echo " Build Matchbox WM"
  cd ../matchbox-window-manager
  makepkg -f
  rm -rf src/ pkg/
  echo "linux-raspberrypi-armv7"
  cd ../linux-raspberrypi4-rune
  makepkg -f
  rm -rf src/ pkg/
  ;;
aarch64)
  echo " Build Matchbox WM"
  cd ../matchbox-window-manager
  makepkg -f
  rm -rf src/ pkg/
  echo " Build WiringPi for aarch64"
  cd ../wiringpi
  makepkg -f
  rm -rf src/ pkg/ WiringPi/
  echo "Make firmware for aarch64"
  cd ../raspberrypi-firmware
  makepkg -f
  rm -rf src/ pkg/
  echo "linux-raspberrypi64"
  cd ../linux-raspberrypi64-rune
  makepkg -f
  rm -rf src/ pkg/
  ;;
esac
#echo "Deleting build folders"
#rm -rf /tmp/makepkg/*
echo "Done with RuneAudio packages!"
