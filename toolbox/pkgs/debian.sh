#!/usr/bin/env bash
set -euo pipefail
# Query the linker version
ld -v || true

# Query the (g)libc version
ldd --version || true

# Unattended update, upgrade, and install
export DEBIAN_FRONTEND=noninteractive
export DEBIAN_PRIORITY=critical
apt-get -qy update
#apt-get -qy \
#  -o "Dpkg::Options::=--force-confdef" \
#  -o "Dpkg::Options::=--force-confold" upgrade
apt-get -qy --no-install-recommends install apt-utils
apt-get -qy autoclean
apt-get -qy install \
 meson \
 nbd-client \
 nbdkit \
 pipx \
 sudo \
 xfslibs-dev \
 xfsprogs

pipx install cijoe==v0.9.51 --force --include-deps
pipx ensurepath

# Retrieve, build and install xNVMe from source
git clone https://github.com/xnvme/xnvme.git
cd xnvme
make common install
