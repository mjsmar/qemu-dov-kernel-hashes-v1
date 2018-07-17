#!/bin/bash

die() {
	echo "$@" && exit 1
}

or_die() {
	if [ $? -ne 0 ]; then
		die "$@"
	fi
	return 0
}

check_var() {
    var=$1
    regex=$2
    eval "[ -n \"\$$var\" ]"
    or_die "$var environment variable not set"
    if [ -n "$regex" ]; then
        eval "echo \$$var | grep -P \"$regex\" &>/dev/null"
        or_die "$var must be of form \"$regex\""
    fi
}

config_file=$1
incoming_spec=${2:-none}

if [ ! -e $config_file ]; then
    die $config_file not found
fi

source $config_file

# required env variables 
check_var MT_QEMU_BUILD_DIR
check_var MT_VM_NAME "^vm\d+"
check_var MT_VM_MAC
check_var MT_HOST_NIC
check_var MT_DISK_PATH_ROOT
check_var MT_QEMU_CMD

name=$MT_VM_NAME
ssh_port=$(($(echo $name | sed 's/vm//') + 2230))
monitor_sock=/tmp/mdroth-$name-hmp.sock
vtapfd=8

macvtap_up() {
	id=$1
	macvtap_id=macvtap_$id
	mac=$2
	ip link add link $host_nic name $macvtap_id type macvtap
	or_die "failed to create $macvtap_id"
	ip link set $macvtap_id address $mac up
	or_die "failed to bring up $macvtap_id with mac $mac"
}

macvtap_down() {
	id=$1
	macvtap_id=macvtap_$id
	ip link del link $host_nic dev $macvtap_id
	or_die "failed to remove $macvtap_id"
}

macvtap_path() {
	id=$1
	macvtap_id=macvtap_$id
	mac=$2
	if ip link show $macvtap_id &>/dev/null; then
		macvtap_down $id
	fi
	sleep 1
	macvtap_up $id $mac
    fdn=$(ip link show $macvtap_id | grep $macvtap_id | cut -d ':' -f 1)
	echo "/dev/tap$fdn"
}

start_vm() {
	cmd=$(eval "echo $MT_QEMU_CMD")
	if [ "$incoming_spec" != "none" ]; then
		cmd="$cmd -incoming $incoming_spec -S"
	fi
    if [ $MT_USE_MACVTAP == 1 ]; then
        vtap=$(macvtap_path $name $MT_MAC_ADDR)
        if [ $? != 0 ]; then
            echo "macvtap setup failed: $vtap"
            exit 1
        fi
	    cmd="$cmd $vtapfd<>$vtap 2>/tmp/$name.log"
    else
	    cmd="$cmd 2>/tmp/$name.log"
    fi
	echo \'$cmd\'
	eval "$cmd"
}

start_vm
