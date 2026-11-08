#!/bin/sh
# Install devkitPro's Wii toolchain (devkitPPC + libogc) on Arch Linux.
# Official steps from https://devkitpro.org/wiki/devkitPro_pacman
set -e

sudo pacman-key --recv BC26F752D25B92CE272E0F44F7FD5492264BB9D0 --keyserver keyserver.ubuntu.com
sudo pacman-key --lsign BC26F752D25B92CE272E0F44F7FD5492264BB9D0
sudo pacman -U --noconfirm https://pkg.devkitpro.org/devkitpro-keyring.pkg.tar.zst

if ! grep -q '^\[dkp-libs\]' /etc/pacman.conf; then
    printf '\n[dkp-libs]\nServer = https://pkg.devkitpro.org/packages\n\n[dkp-linux]\nServer = https://pkg.devkitpro.org/packages/linux/$arch/\n' \
        | sudo tee -a /etc/pacman.conf
fi

sudo pacman -Syu --needed wii-dev
echo
echo "Done. devkitPro is in /opt/devkitpro."
