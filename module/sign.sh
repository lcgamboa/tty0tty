#!/usr/bin/bash -x
#https://wiki.debian.org/SecureBoot
echo -n "Passphrase for the private key: "
read -s KBUILD_SIGN_PIN
export KBUILD_SIGN_PIN

VERSION="$(uname -r)"
#SHORT_VERSION="$(uname -r | cut -d - -f 1-2)"
SHORT_VERSION="$(uname -r | cut -d - -f 1)"
MODULES_DIR=/lib/modules/$VERSION
KBUILD_DIR=/usr/lib/linux-kbuild-$SHORT_VERSION

for i in *.ko ; do sudo --preserve-env=KBUILD_SIGN_PIN "$KBUILD_DIR"/scripts/sign-file sha256 /var/lib/shim-signed/mok/MOK.priv /var/lib/shim-signed/mok/MOK.der "$i" ; done
