#!/bin/bash
# Script to build AURs
cd libmatchbox
makepkg -f
rm -rf src/ pkg/
cd ../matchbox
makepkg -f
rm -rf src/ pkg/
cd ../drc
makepkg -f
rm -rf src/ pkg/
