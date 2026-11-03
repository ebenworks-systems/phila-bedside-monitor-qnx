#!/usr/bin/env bash
# make-image.sh - build the QNX 8.0 QEMU image the monitor runs on.
#
#   source ~/qnx800/qnxsdp-env.sh
#   scripts/make-image.sh VMDIR [SSH_PUBLIC_KEY]
#
# Uses mkqnximage from the SDP: an x86_64 QEMU image with two CPUs, 1 GB of
# memory, SSH access by key and the snippets in qemu/snippets/ (they add the
# sshd-auth helper that the default image leaves out, without which sshd
# refuses every login). A key pair is made next to VMDIR if none is given.
# SPDX-License-Identifier: MIT
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
vm="${1:?usage: scripts/make-image.sh VMDIR [SSH_PUBLIC_KEY]}"
[ -n "${QNX_HOST:-}" ] || { echo "source qnxsdp-env.sh from your SDP 8.0 first" >&2; exit 1; }
mkdir -p "$vm"
vm="$(cd "$vm" && pwd)"
key="${2:-$vm/../phila-vm-key.pub}"
if [ ! -f "$key" ]; then
    ssh-keygen -q -t ed25519 -N '' -C 'phila-qnx-vm (local only)' -f "${key%.pub}"
    echo "made the key pair ${key%.pub}"
fi
mkdir -p "$vm/local/snippets"
cp "$here"/qemu/snippets/*.custom "$vm/local/snippets/"
cd "$vm"
mkqnximage --type=qemu --arch=x86_64 --hostname=phila-monitor --cpu=2 --ram=1G --ssh-ident="$key" \
    > "$vm/../$(basename "$vm")-mkqnximage.log" 2>&1 || {
    echo "mkqnximage failed; see $vm/../$(basename "$vm")-mkqnximage.log" >&2
    exit 1
}
echo "image ready in $vm/output; start it with scripts/qemu-vm.sh start $vm"
