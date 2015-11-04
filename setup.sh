#!/bin/bash

modprobe pcspkr
cp etc/udev/rules.d/10-local.rules /etc/udev/rules.d/10-local.rules
sleep 1
udevadm control --reload-rules
sleep 1
umount /mnt
sleep 1
dmsetup remove relaydisk
sleep 1
rmmod dm-relay
sleep 1
insmod ./kernel/dm-relay.ko
sleep 1
echo "0 `blockdev --getsize $1` relay $1 10000 6000" | dmsetup create relaydisk
sleep 3
mount /dev/mapper/relaydisk /mnt
