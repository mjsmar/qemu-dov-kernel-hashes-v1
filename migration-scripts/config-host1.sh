#!/bin/bash

MT_VM_NAME=vm0
MT_VM_MAC=52:54:00:72:d2:70
MT_HOST_NIC=enP1p5s0f0

# configurable QEMU opts
MT_DISK_PATH_ROOT=/home/mdroth/u1804-root2-qcow2.snap0
MT_QEMU_BUILD_DIR=/home/mdroth/qemu-build-stable-2.11
MT_QEMU_OPTS_MACHINE="
    -machine pseries-2.10,accel=kvm,usb=off,dump-guest-core=off,max-cpu-compat=power9"
MT_QEMU_OPTS_SMP="
    -smp 16,maxcpus=32,sockets=2,cores=8,threads=1"
MT_QEMU_OPTS_MEM="
    -m 8G
	-object memory-backend-ram,id=ram-node0,size=4294967296
	-numa node,nodeid=0,cpus=0-7,memdev=ram-node0
	-object memory-backend-ram,id=ram-node1,size=4294967296
	-numa node,nodeid=1,cpus=8-15,memdev=ram-node1"
MT_QEMU_OPTS_DISKS="
	-device virtio-scsi-pci,id=scsi0,bus=pci.0,addr=0x2
	-device virtio-scsi-pci,id=scsi1,bus=pci.0,addr=0x3
	-drive file=$MT_DISK_PATH_ROOT,format=qcow2,if=none,id=drive-scsi1-0-0-0,cache=none,aio=native
	-device scsi-hd,bus=scsi1.0,channel=0,scsi-id=0,lun=0,drive=drive-scsi1-0-0-0,id=scsi1-0-0-0,bootindex=1"

# scripts rely on some of these parameters so avoid editing directly

MT_USE_EXTERNAL_NET=0
MT_USE_MACVTAP=0
MT_IFUP_SCRIPT=/home/mdroth/qemu-ifup.sh
MT_QEMU_OPTS_EXTERNAL_NET=
if [ $MT_USE_EXTERNAL_NET == 1 ]; then
    MT_QEMU_OPTS_EXTERNAL_NET='
    	-netdev tap,id=hostnet0,vhost=on,script=$MT_IFUP_SCRIPT
    	-device virtio-net-pci,netdev=hostnet0,id=net0,mac=$MT_VM_MAC,bus=pci.0,addr=0x1,bootindex=2'
    if [ $MT_USE_MACVTAP == 1 ]; then
        MT_QEMU_OPTS_EXTERNAL_NET='
        	-netdev tap,id=hostnet0,vhost=on,fd=$vtapfd
        	-device virtio-net-pci,netdev=hostnet0,id=net0,mac=$mac_addr,bus=pci.0,addr=0x1,bootindex=2'
    fi
fi
MT_QEMU_OPTS_LOCAL_NET='
	-netdev user,id=netdev0,hostfwd=tcp:127.0.0.1:$ssh_port-:22
	-device virtio-net-pci,netdev=netdev0,id=vnet0'

MT_QEMU_CMD='$MT_QEMU_BUILD_DIR/ppc64-softmmu/qemu-system-ppc64 \
    $MT_QEMU_OPTS_MACHINE \
    $MT_QEMU_OPTS_SMP \
    $MT_QEMU_OPTS_MEM \
    $MT_QEMU_OPTS_DISKS \
    $MT_QEMU_OPTS_LOCAL_NET \
    $MT_QEMU_OPTS_EXTERNAL_NET \
	-name guest=$MT_VM_NAME,debug-threads=on \
	-realtime mlock=off \
	-rtc base=utc -no-shutdown -boot strict=on -msg timestamp=on \
	-device qemu-xhci,id=usb,bus=pci.0,addr=0x8 \
	-device virtio-balloon-pci,id=balloon0,bus=pci.0 \
	-monitor unix:$monitor_sock,server,nowait \
	-L $MT_QEMU_BUILD_DIR/pc-bios \
	-nographic -vnc none'
