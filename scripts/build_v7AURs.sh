#!/bin/bash
# Script to build AURs
cd ../../AUR
echo " Build libmatchbox"
cd libmatchbox
makepkg -f
rm -rf src/ pkg/
echo " Build Matchbox WM"
cd ../matchbox-window-manager
makepkg -f
rm -rf src/ pkg/
echo " Build Matchbox OSK"
cd ../matchbox-keyboard
makepkg -f
rm -rf src/ pkg/
echo " Build Dynamic Room Correction"
cd ../drc
makepkg -f
rm -rf src/ pkg/
echo "v7AURs Done!"
