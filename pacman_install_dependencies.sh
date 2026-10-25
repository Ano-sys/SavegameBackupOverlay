#!/bin/bash

if ! command -v pacman >/dev/null 2>&1; then
    echo "Error: pacman not found."
    exit 1
fi

sudo pacman-key --recv-keys BC26F752D25B92CE272E0F44F7FD5492264BB9D0 --keyserver keyserver.ubuntu.com
sudo pacman-key --lsign-key BC26F752D25B92CE272E0F44F7FD5492264BB9D0

sudo pacman -U --noconfirm https://pkg.devkitpro.org/devkitpro-keyring.pkg.tar.xz

if grep -Fq "[dkp-libs]" /etc/pacman.conf && grep -Fq "[dkp-linux]" /etc/pacman.conf; then
    echo "Already installed"
else
    printf '\n[dkp-libs]\nServer = https://downloads.devkitpro.org/packages\n\n[dkp-linux]\nServer = https://downloads.devkitpro.org/packages/linux/$arch/\n' \
        | sudo tee -a /etc/pacman.conf > /dev/null
fi

sudo pacman -Syu --noconfirm --needed switch-dev switch-curl switch-zlib switch-mbedtls