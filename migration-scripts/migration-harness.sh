#!/bin/bash

remote_hostname=boslcp5
remote_host=10.33.1.59
remote_user=root
local_hostname=boslcp6
local_host=10.33.3.91
local_user=root

vm_main=vm2
vm_backup=vm3

vm_idx() {
	echo $1 | sed 's/vm//'
}

#launch_script=/root/launch-boslcp3g1.sh
launch_script=/root/launch-boslcp3g1-migtest2.sh

ssh_port_vm_main=$((2230 + $(vm_idx $vm_main)))
migration_port=$((9910 + $(vm_idx $vm_main)))
backup_migration_port=$((9910 + $(vm_idx $vm_backup)))
remote_migration_bind_spec=tcp:0.0.0.0:$migration_port
remote_migration_addr_spec=tcp:$remote_host:$migration_port
local_migration_bind_spec=tcp:0.0.0.0:$migration_port
local_migration_addr_spec=tcp:$local_host:$migration_port
#back target guest up through local socket
backup_migration_bind_spec=unix:/tmp/${vm_main}-backup-migrate.sock
backup_migration_addr_spec=unix:/tmp/${vm_main}-backup-migrate.sock
#back source guest up through remote socket
backup_remote_migration_bind_spec=tcp:0.0.0.0:$backup_migration_port
backup_remote_migration_addr_spec=tcp:$remote_host:$backup_migration_port
backup_local_migration_bind_spec=tcp:0.0.0.0:$backup_migration_port
backup_local_migration_addr_spec=tcp:$local_host:$backup_migration_port

do_remote_quiet() {
	ssh $remote_user@$remote_host sudo $@
}

do_remote() {
	echo "remote cmd: ssh $remote_user@$remote_host sudo $@"
	ssh $remote_user@$remote_host sudo $@
}

hmp() {
        path=/tmp/mdroth-$1-hmp.sock
	shift
        echo "$@" | socat stdio unix-connect:$path
	return $?
}

hmp_remote() {
        path=/tmp/mdroth-$1-hmp.sock
	shift
        echo "$@" | do_remote socat stdio unix-connect:$path
	return $?
}

start_vm() {
	name=$1
        incoming=${2:-"none"}
        cmd="$launch_script $local_hostname $incoming $name"
        echo \'$cmd\'
        screen -x $name -X screen -t console 1 $cmd
	start_status=running
	if [ "$incoming" != "" ]; then
		start_status="paused"
	fi
	while ! hmp $name "info status" | grep "status: $start_status"; do
		sleep 1
	done
	sleep 1
	echo $name started
}

start_vm0() {
	start_vm vm0 $1
}

start_vm1() {
	start_vm vm1 $1
}

start_vm_remote() {
	name=$1
        incoming=${2:-"none"}
        cmd="$launch_script $remote_hostname $incoming $name"
        echo \'$cmd\'
        do_remote screen -x $name -X screen -t console 1 $cmd
	start_status=running
	if [ "$incoming" != "" ]; then
		start_status="paused"
	fi
	while ! hmp_remote $name "info status" | grep "status: $start_status"; do
		hmp_remote $name "info status"
		echo \"$start_status\"
		sleep 1
	done
	sleep 1
	echo $name started
}

start_vm0_remote() {
	start_vm_remote vm0 $1
}

start_vm1_remote() {
	start_vm_remote vm1 $1
}

start_screen() {
        session=$1
        if ! screen -ls | grep -P "\d+.$session" 2>/dev/null; then
                screen -S "$session" -d -m
        fi
}

start_screen_remote() {
        session=$1
        if ! do_remote screen -ls | grep -P "\d+.$session" 2>/dev/null; then
                do_remote screen -S "$session" -d -m
        fi
}

wait_for_migrate() {
	source=$1
	timeout=${2:-9999999}
	while [ $timeout -gt 0 ]; do
		status=$(hmp $source "info migrate" | grep "Migration status:")
		if echo $status | grep failed; then
			return 1
		elif echo $status | grep completed; then
			return 0
		fi
		sleep 5
		timeout=$(($timeout - 5))
	done
	return 2
}

wait_for_migrate_remote() {
	source=$1
	timeout=${2:-9999999}
	while [ $timeout -gt 0 ]; do
		status=$(hmp_remote $source "info migrate" | grep "Migration status:")
		if echo $status | grep failed; then
			return 1
		elif echo $status | grep completed; then
			return 0
		fi
		sleep 5
		timeout=$(($timeout - 5))
	done
	return 2
}

migrate_to_remote() {
	source_vm=$1
	target_vm=$2
	timeout=$3
	hmp $source_vm migrate_set_downtime 5
	hmp $source_vm migrate -d $remote_migration_addr_spec
	wait_for_migrate $source_vm $timeout
	ret=$?
	if [ $ret -eq 0 ]; then
		echo "migration completed (no pause needed)"
		return 0
	elif [ $ret -eq 1 ]; then
		echo "migration failed, aborting"
		return 1
	elif [ $ret -eq 2 ]; then
		echo "migration timed out, pausing $source_vm"
		hmp $source_vm stop
	else
		echo "unknown migration status, aborting"
		return 1
	fi
	wait_for_migrate $source_vm
	ret=$?
	if [ $ret -eq 0 ]; then
		echo "migration completed (pause needed)"
		return 0
	elif [ $ret -eq 1 ]; then
		echo "migration failed, aborting"
		return 1
	elif [ $ret -eq 2 ]; then
		echo "migration timed out after pause, aborting"
	else
		echo "unknown migration status, aborting"
		return 1
	fi
}

migrate_from_remote() {
	source_vm=$1
	target_vm=$2
	timeout=$3
	hmp_remote $source_vm migrate_set_downtime 5
	hmp_remote $source_vm migrate -d $local_migration_addr_spec
	wait_for_migrate_remote $source_vm $timeout
	ret=$?
	if [ $ret -eq 0 ]; then
		echo "migration completed (no pause needed)"
		return 0
	elif [ $ret -eq 1 ]; then
		echo "migration failed, aborting"
		return 1
	elif [ $ret -eq 2 ]; then
		echo "migration timed out, pausing $source_vm"
		hmp_remote $source_vm stop
	else
		echo "unknown migration status, aborting"
		return 1
	fi
	wait_for_migrate_remote $source_vm
	ret=$?
	if [ $ret -eq 0 ]; then
		echo "migration completed (pause needed)"
		return 0
	elif [ $ret -eq 1 ]; then
		echo "migration failed, aborting"
		return 1
	elif [ $ret -eq 2 ]; then
		echo "migration timed out after pause, aborting"
		return 1
	else
		echo "unknown migration status, aborting"
		return 1
	fi
}

migrate_remote_to_backup() {
	source_vm=$1
	target_vm=$2
	hmp_remote $source_vm migrate_set_downtime 5
	hmp_remote $source_vm migrate_backup -d $backup_migration_addr_spec
	# TODO: check that source is paused otherwise this is bogus
	wait_for_migrate_remote $source_vm
	ret=$?
	if [ $ret -eq 0 ]; then
		echo "migration completed (pause needed)"
		return 0
	elif [ $ret -eq 1 ]; then
		echo "migration failed, aborting"
		return 1
	elif [ $ret -eq 2 ]; then
		echo "migration timed out after pause, aborting"
		return 1
	else
		echo "unknown migration status, aborting"
		return 1
	fi
}

migrate_local_to_backup() {
	source_vm=$1
	target_vm=$2
	hmp $source_vm migrate_set_downtime 5
	hmp $source_vm migrate_backup -d $backup_migration_addr_spec
	# TODO: check that source is paused otherwise this is bogus
	wait_for_migrate $source_vm
	ret=$?
	if [ $ret -eq 0 ]; then
		echo "migration completed (pause needed)"
		return 0
	elif [ $ret -eq 1 ]; then
		echo "migration failed, aborting"
		return 1
	elif [ $ret -eq 2 ]; then
		echo "migration timed out after pause, aborting"
		return 1
	else
		echo "unknown migration status, aborting"
		return 1
	fi
}

migrate_backup_to_remote() {
	source_vm=$1
	target_vm=$2
	hmp $source_vm migrate_set_downtime 5
	hmp $source_vm migrate -d $backup_remote_migration_addr_spec
	# TODO: check that source is paused otherwise this is bogus
	wait_for_migrate $source_vm
	ret=$?
	if [ $ret -eq 0 ]; then
		echo "migration completed (pause needed)"
		return 0
	elif [ $ret -eq 1 ]; then
		echo "migration failed, aborting"
		return 1
	elif [ $ret -eq 2 ]; then
		echo "migration timed out after pause, aborting"
		return 1
	else
		echo "unknown migration status, aborting"
		return 1
	fi
}

migrate_backup_from_remote() {
	source_vm=$1
	target_vm=$2
	hmp_remote $source_vm migrate_set_downtime 5
	hmp_remote $source_vm migrate -d $backup_local_migration_addr_spec
	# TODO: check that source is paused otherwise this is bogus
	wait_for_migrate_remote $source_vm
	ret=$?
	if [ $ret -eq 0 ]; then
		echo "migration completed (pause needed)"
		return 0
	elif [ $ret -eq 1 ]; then
		echo "migration failed, aborting"
		return 1
	elif [ $ret -eq 2 ]; then
		echo "migration timed out after pause, aborting"
		return 1
	else
		echo "unknown migration status, aborting"
		return 1
	fi
}

guest_ssh() {
	ssh -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no $@
}

wait_for_connection() {
	port=$1
	timeout=$2
	echo "waiting for connection to localhost:$port, $timeout seconds till timeout"
	while [ $timeout -gt 0 ]; do
		#if guest_ssh -o ConnectTimeout=1 -p $port root@localhost touch /root/ts/$(date +"%FT%T"); then
		if guest_ssh -o ConnectTimeout=1 -p $port root@localhost touch /root/ts/$(date +"%FT%T") 2>/dev/null; then
			echo "connection established for localhost:$port"
			return 0
		fi
		timeout=$(($timeout - 1))
		sleep 1
	done
	return 1
}

wait_for_connection_remote() {
	port=$1
	timeout=$2
	echo "waiting for connection to localhost:$port, $timeout seconds till timeout"
	while [ $timeout -gt 0 ]; do
		if do_remote ssh -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no -o ConnectTimeout=1 -p $port root@localhost touch /root/ts/$(date +"%FT%T") 2>/dev/null; then
			echo "connection established for localhost:$port"
			return 0
		fi
		timeout=$(($timeout - 1))
		sleep 1
	done
	return 1
}

marker() {
	port=$1
	shift
	msg="$@"
	ssh -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no -p $port root@localhost /root/marker.sh "\"$msg - host time: $(date +'%FT%T') - local\""
}

marker_remote() {
	port=$1
	shift
	msg="$@"
	do_remote ssh -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no -p $port root@localhost /root/marker.sh "\"$msg - host time: $(date +'%FT%T') - remote\""
}

wait_for_unpause() {
	was_paused=0
	while [ -f /tmp/pause_migration ]; do
		if [ $was_paused -eq 0 ]; then
			echo "migration loop paused via /tmp/pause_migration"
		fi
		was_paused=1
		sleep 30
	done
	if [ $was_paused -eq 1 ]; then
		echo "unpaused"
	fi
}

vm_logs=vm_logs_migtest2/$(date +"%s")
vm_dumps=$vm_logs/dumps
mkdir -p $vm_logs
mkdir -p $vm_dumps
do_remote mkdir -p $vm_logs
do_remote mkdir -p $vm_dumps

memory_comparison_local_to_remote() {
	j=$1
	hmp $vm_main pmemsave 0 2147483648 $vm_dumps/0-2.$vm_main.iteration$j
	hmp_remote $vm_main pmemsave 0 2147483648 $vm_dumps/0-2.$vm_main.iteration$j
	#hmp_remote $vm_backup pmemsave 0 2147483648 $vm_dumps/0-2.$vm_backup.iteration$j
	md5_local=$(md5sum $vm_dumps/0-2.$vm_main.iteration$j)
	md5_remote=$(do_remote_quiet md5sum $vm_dumps/0-2.$vm_main.iteration$j)
	#md5_remote_backup=$(do_remote_quiet md5sum $vm_dumps/0-2.$vm_backup.iteration$j)
	md5_local=$(echo $md5_local |  cut -d' ' -f 1)
	md5_remote=$(echo $md5_remote |  cut -d' ' -f 1)
	#md5_remote_backup=$(echo $md5_remote_backup |  cut -d' ' -f 1)
	if [ "$md5_local" != "$md5_remote" ]; then
		echo memory mismatch between guests $md5_local vs. $md5_remote
		return 1
	#elif [ "$md5_local" != "$md5_remote_backup" ]; then
	#	echo memory mismatch between main and backup
	#	return 1
	fi
	# looks good, no reason to keep
	echo "memory between guests matches: $md5_local vs. $md5_remote"
	rm $vm_dumps/0-2.$vm_main.iteration$j
	do_remote rm $vm_dumps/0-2.$vm_main.iteration$j
	#do_remote rm $vm_dumps/0-2.$vm_backup.iteration$j
	return 0
}

memory_comparison_remote_to_local() {
	j=$1
	hmp_remote $vm_main pmemsave 0 2147483648 $vm_dumps/0-2.$vm_main.iteration$j
	hmp $vm_main pmemsave 0 2147483648 $vm_dumps/0-2.$vm_main.iteration$j
	#hmp $vm_backup pmemsave 0 2147483648 $vm_dumps/0-2.$vm_backup.iteration$j
	md5_remote=$(do_remote_quiet md5sum $vm_dumps/0-2.$vm_main.iteration$j)
	md5_local=$(md5sum $vm_dumps/0-2.$vm_main.iteration$j)
	#md5_local_backup=$(md5sum $vm_dumps/0-2.$vm_backup.iteration$j)
	md5_remote=$(echo $md5_remote |  cut -d' ' -f 1)
	md5_local=$(echo $md5_local |  cut -d' ' -f 1)
	#md5_local_backup=$(echo $md5_local_backup |  cut -d' ' -f 1)
	if [ "$md5_remote" != "$md5_local" ]; then
		echo memory mismatch between guests $md5_remote vs. $md5_local
		return 1
	#elif [ "$md5_remote" != "$md5_local_backup" ]; then
	#	echo memory mismatch between main and backup
	#	return 1
	fi
	# looks good, no reason to keep
	echo "memory between guests matches: $md5_remote vs. $md5_local"
	do_remote rm $vm_dumps/0-2.$vm_main.iteration$j
	rm $vm_dumps/0-2.$vm_main.iteration$j
	#rm $vm_dumps/0-2.$vm_backup.iteration$j
	return 0
}


workload_orig=/home/mdroth/workload-bz166298.sh
workload_nfs_orig=/home/mdroth/nfs/workload-bz166298.sh
workload=/root/startup.sh
time_till_migrate=120

start_screen $vm_main
#start_screen $vm_backup
start_screen_remote $vm_main
#start_screen_remote $vm_backup

#start_vm0
#start prep vm0 manually on local host
#for some reason waitforconnect hangs otherwise. do:
#/root/launch-boslcp3g1.sh boslcp6
echo -n "waiting for initial connection to $vm_main port $ssh_port_vm_main ... "
sleep 10
wait_for_connection $ssh_port_vm_main 100000
echo "done."
sleep 10
#scp -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no -P $ssh_port_vm_main $workload_orig root@localhost:$workload
#guest_ssh -p $port root@localhost /root/startup.sh

#stress coordination
mbase=/tmp/stress
start_marker=$mbase.start
end_marker=$mbase.completed
fail_marker=$mbase.failed

i=1

remote_guest_ssh() {
	port=$1
	shift
	do_remote ssh -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no -o ConnectTimeout=1 -p $port root@localhost $@
}

local_guest_ssh() {
	port=$1
	shift
	ssh -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no -o ConnectTimeout=1 -p $port root@localhost $@
}

local_workload_start() {
	ssh_port=$1
	echo "starting workload -local"
	marker $ssh_port "starting workload -local"
	local_guest_ssh $ssh_port rm -f $end_marker
	local_guest_ssh $ssh_port rm -f $fail_marker
	local_guest_ssh $ssh_port touch $start_marker
}

remote_workload_start() {
	ssh_port=$1
	echo "starting workload -remote"
	marker $ssh_port "starting workload -remote"
	remote_guest_ssh $ssh_port rm -f $end_marker
	remote_guest_ssh $ssh_port rm -f $fail_marker
	remote_guest_ssh $ssh_port touch $start_marker
}

local_workload_collect() {
	ssh_port=$1
	echo "collecting workload results -local"
	marker $ssh_port "collecting workload results -local"
	while true; do
		if local_guest_ssh $ssh_port ls $end_marker; then
			if local_guest_ssh $ssh_port ls $fail_marker; then
				marker $ssh_port "workload failure detected"
				return 1
			else
				marker $ssh_port "workload succeeded"
				return 0
			fi
		fi
		sleep 5
	done
}

remote_workload_collect() {
	ssh_port=$1
	echo "collecting workload results -remote"
	marker $ssh_port "collecting workload results -remote"
	while true; do
		if remote_guest_ssh $ssh_port ls $end_marker; then
			if remote_guest_ssh $ssh_port ls $fail_marker; then
				marker $ssh_port "workload failure detected"
				return 1
			else
				marker $ssh_port "workload succeeded"
				return 0
			fi
		fi
		sleep 5
	done
}

while true; do
	wait_for_unpause
	echo iteration: ${i}a
	start_vm_remote $vm_main $remote_migration_bind_spec
	#start_vm_remote $vm_backup $backup_migration_bind_spec
	if [ $i -eq 1 ]; then
		local_workload_start $ssh_port_vm_main
	fi
	/root/thp-set.sh scan_sleep_millisecs 250
	do_remote /root/thp-set.sh scan_sleep_millisecs 250
	marker $ssh_port_vm_main "migrating to $remote_hostname, iteration: $i"
	migrate_to_remote $vm_main $vm_main 60 || exit 1
	#migrate_remote_to_backup $vm_main $vm_backup || exit 1
	# migrations done, check for memory inconsistency in lower 2G range before resuming
	memory_comparison_local_to_remote ${i}a
	hmp_remote $vm_main cont
	wait_for_connection_remote $ssh_port_vm_main 900 || exit 1
	marker_remote $ssh_port_vm_main "migration to $remote_hostname complete"
	echo "iteration: ${i} migration to $remote_hostname complete"
	remote_workload_collect $ssh_port_vm_main || exit 1
	# success, kill source and remote's backup guest
	wait_for_unpause
	hmp $vm_main quit
	cp ${vm_main}.log $vm_logs/${vm_main}.log.iteration${i}a
	/root/thp-set.sh scan_sleep_millisecs 10000
	do_remote /root/thp-set.sh scan_sleep_millisecs 10000
	#hmp_remote $vm_backup quit
	#do_remote cp ${vm_backup}.log $vm_logs/${vm_backup}.log.iteration${i}a


	remote_workload_start $ssh_port_vm_main
	echo "sleeping for $time_till_migrate seconds"
	sleep $time_till_migrate

	wait_for_unpause
	echo iteration: ${i}b
	start_vm $vm_main $local_migration_bind_spec
	#start_vm $vm_backup $backup_migration_bind_spec
	/root/thp-set.sh scan_sleep_millisecs 250
	do_remote /root/thp-set.sh scan_sleep_millisecs 250
	marker_remote $ssh_port_vm_main "migrating to $local_hostname, iteration: ${i}b"
	migrate_from_remote $vm_main $vm_main 60 || exit 1
	#migrate_local_to_backup $vm_main $vm_backup || exit 1
	# migrations done, check for memory inconsistency in lower 2G range before resuming
	memory_comparison_remote_to_local ${i}b
	hmp $vm_main cont
	wait_for_connection $ssh_port_vm_main 900 || exit 1
	marker $ssh_port_vm_main "migration to $local_hostname complete"
	echo "iteration: ${i}b migration to $local_hostname complete"
	local_workload_collect $ssh_port_vm_main || exit 1
	# success, kill remote source and local's backup guest
	wait_for_unpause
	hmp_remote $vm_main quit
	do_remote cp ${vm_main}.log $vm_logs/${vm_main}.log.iteration${i}b
	/root/thp-set.sh scan_sleep_millisecs 10000
	do_remote /root/thp-set.sh scan_sleep_millisecs 10000
	#hmp $vm_backup quit
	#cp ${vm_backup}.log $vm_logs/${vm_backup}.log.iteration${i}b

	local_workload_start $ssh_port_vm_main
	echo "sleeping for $time_till_migrate seconds"
	sleep $time_till_migrate

	i=$(($i + 1))
done
