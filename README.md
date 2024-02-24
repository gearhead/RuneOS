# Description
Repository of packages and configuration files to build RuneOS

## 1. Recipes
The recipe is in the Scripts folder. The build scripts are also there which will automatically 
build all the packages for RuneOS.

## 2. Overview
the original RuneAudio images and versions 0.4, 0.5, 0.6 were based on Arch. The exception is 0.6 
version for 'all' RPis . The 'all' version is a 32 bit image and will run on all RPis up to the Pi5

RuneOS is designed to run on all RPis as it is light enough to run on all Pis. Arch removed 
support for armv6 (Pi1 and PiZero) in 2022. To maintain this support, we started to port the packages 
over to RPiOS with Bullseye in 2023 for the 0.6b 32 bit 'all' image. 
Bookworm was released in late 2023 and this repo is designed to be built for Bookworm. The armhf and 
arm64 packages are designed for the RPiOS-lite version. The armhf packages are for the 'all' verison 
and are 32 bit. The arm64 packages are built for the 64 bit arm image which runs on all Pis except the 
Pi1/Zero.

The rest of the packages needed for a Rune are the default packages for RPiOS and do everything we need.

There are a couple of notes:

fdkaac is needed for Bluez-alsa and is not built for armhf (32 bit) by RPiOS. This package is needed so 
that bluetooth would work on 32 bit.

iwd exists for RPiOS for both 32 bit and 64 bit images. We patch the iwd-git verison and use it because 
Without the patches, the brcmfmac driver on the Pi continually disconnects and does not allow the user to
actually configure the Rune via AP (Thanks to James Prestwood on the iwd team for these patches).
