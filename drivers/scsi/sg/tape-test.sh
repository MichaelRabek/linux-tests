#!/bin/bash

# Define the number of files N
N=12

while true ; do cat /proc/scsi/sg/debug ; done > sg.log &

mkdir -p tape
ltfs -o devname=1097007216 ./tape
if ! (mount | grep 'ltfs') ; then
	echo "LTFS not mounted yet. Wait a while and try again." >&2
	exit 1
fi
pushd tape

for i in $(seq 0 $N | shuf); do
    echo "Reading file${i}"
    cat file${i} > /dev/null

    should_write=$((RANDOM % 2))
    if [ "$should_write" -eq 1 ]; then
	echo "Writing file${i}"
        dd if=/dev/urandom of=file${i} bs=2048 count=1000 
	echo "Reading file${i}"
        cat file${i} > /dev/null
    fi
done

popd

umount tape
sleep 2
sync
kill %1

# field=3
field=3

for n in $(grep -E "elap|dur" ~/sg.log | cut -d/ -f${field} | sed -e 's/^\([0-9][0-9]*\).*$/\1/') ; do
	[ $n -gt 20000 -o $n -lt 0 ] 2>/dev/null && echo $n
done

