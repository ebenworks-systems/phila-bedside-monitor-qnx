#!/usr/bin/env bash
# qemu-vm.sh - start, stop and reach a QNX 8.0 image built by mkqnximage, without root.
#
# mkqnximage's own runner (mkqnximage --run) attaches the guest to a host network bridge, which needs a
# bridge and a setuid helper set up as root. This script boots the same disk with QEMU's user-mode network
# instead: the guest gets 10.0.2.15 by DHCP and its SSH port is forwarded to 127.0.0.1 only.
#
#   scripts/qemu-vm.sh start  VMDIR     boot in the background; the serial console goes to VMDIR/console.log
#   scripts/qemu-vm.sh wait   VMDIR     wait until the guest answers on SSH
#   scripts/qemu-vm.sh ssh    VMDIR [command...]
#   scripts/qemu-vm.sh put    VMDIR LOCAL REMOTE
#   scripts/qemu-vm.sh get    VMDIR REMOTE LOCAL
#   scripts/qemu-vm.sh stop   VMDIR
#
# Environment: QNX_VM_KEY (private key whose .pub was given to mkqnximage --ssh-ident),
# QNX_VM_SSH_PORT (default 2222), QNX_VM_CPUS (default 2), QNX_VM_RAM (default 1G).
#
# SPDX-License-Identifier: MIT
set -euo pipefail

cmd="${1:-}"; vm="${2:-}"
[ -n "$cmd" ] && [ -n "$vm" ] || { sed -n '2,16p' "$0"; exit 2; }
vm="$(cd "$vm" && pwd)"
port="${QNX_VM_SSH_PORT:-2222}"
key="${QNX_VM_KEY:-$vm/../phila-vm-key}"
ssh_opts=(-i "$key" -p "$port" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
          -o LogLevel=ERROR -o ConnectTimeout=5 -o BatchMode=yes -o IdentitiesOnly=yes)

case "$cmd" in
start)
    [ -f "$vm/output/disk-qemu.vmdk" ] || { echo "no image in $vm/output; run mkqnximage there first" >&2; exit 1; }
    if [ -f "$vm/qemu.pid" ] && kill -0 "$(cat "$vm/qemu.pid")" 2>/dev/null; then
        echo "already running (pid $(cat "$vm/qemu.pid"))"; exit 0
    fi
    accel=(-cpu max)
    [ -w /dev/kvm ] && accel=(-enable-kvm -cpu host)
    : > "$vm/console.log"
    # debug-threads names each virtual CPU's host thread "CPU n/KVM", so
    # latency-runs.sh can read how long the host kept it waiting.
    qemu-system-x86_64 -name phila-qnx,debug-threads=on \
        -smp "${QNX_VM_CPUS:-2}" "${accel[@]}" -m "${QNX_VM_RAM:-1G}" \
        -kernel "$vm/output/ifs.bin" \
        -drive file="$vm/output/disk-qemu.vmdk",if=ide,id=drv0 \
        -netdev user,id=net0,hostfwd=tcp:127.0.0.1:"$port"-:22 -device virtio-net-pci,netdev=net0 \
        -object rng-random,filename=/dev/urandom,id=rng0 -device virtio-rng-pci,rng=rng0 \
        -display none -serial file:"$vm/console.log" \
        -monitor unix:"$vm/monitor.sock",server,nowait \
        -pidfile "$vm/qemu.pid" -daemonize
    echo "started (pid $(cat "$vm/qemu.pid")); console: $vm/console.log; ssh: 127.0.0.1:$port"
    ;;
wait)
    for _ in $(seq 1 90); do
        if ssh "${ssh_opts[@]}" root@127.0.0.1 true 2>/dev/null; then echo "guest is up"; exit 0; fi
        sleep 2
    done
    echo "guest did not answer on ssh within 180 s; see $vm/console.log" >&2; exit 1
    ;;
ssh)
    shift 2
    exec ssh "${ssh_opts[@]}" root@127.0.0.1 "$@"
    ;;
put)
    scp -q -i "$key" -P "$port" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o IdentitiesOnly=yes \
        "$3" root@127.0.0.1:"$4"
    ;;
get)
    scp -q -i "$key" -P "$port" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o IdentitiesOnly=yes \
        root@127.0.0.1:"$3" "$4"
    ;;
stop)
    if [ -S "$vm/monitor.sock" ]; then
        echo quit | socat - UNIX-CONNECT:"$vm/monitor.sock" >/dev/null 2>&1 || true
    fi
    # If the quit did not reach QEMU (no socat, no socket), end it by its process id.
    # Only a process that is still QEMU is signalled: a pid file left by a crash can hold a number
    # that now belongs to something else.
    if [ -f "$vm/qemu.pid" ] && p="$(cat "$vm/qemu.pid")" && grep -qa qemu-system "/proc/$p/cmdline" 2>/dev/null; then
        sleep 2
        if grep -qa qemu-system "/proc/$p/cmdline" 2>/dev/null; then kill "$p" 2>/dev/null || true; fi
    fi
    rm -f "$vm/qemu.pid" "$vm/monitor.sock"
    echo stopped
    ;;
*)
    sed -n '2,16p' "$0"; exit 2
    ;;
esac
