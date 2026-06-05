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
 clang \
 libbpf-dev \
 llvm \
 meson \
 nbd-client \
 nbdkit \
 pipx \
 sudo \
 xfslibs-dev \
 xfsprogs

pipx install cijoe==v0.9.51 --force --include-deps
pipx ensurepath

# Retrieve, build and install bpftool from source. Avoids Ubuntu's linux-tools
# split, where /usr/sbin/bpftool is a kernel-version-strict wrapper script and
# the kernel-tools packages don't ship the bpftool binary anyway. xal only
# uses bpftool to dump BTF from a file, which works with any bpftool version.
git clone --recurse-submodules https://github.com/libbpf/bpftool.git
cd bpftool/src
make
make install
cd ../..

# Retrieve, build and install xNVMe from source
git clone https://github.com/xnvme/xnvme.git
cd xnvme
make common install
