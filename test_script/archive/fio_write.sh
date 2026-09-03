#!/bin/sh

sudo fio --filename=/dev/nvme0n1 \
    --direct=1 \
    --ioengine=libaio \
    --rw=randwrite \
    --bs=4k \
    --size=2800M \
    --io_size=5G \
    --numjobs=1 \
    --norandommap=1 \
    --randrepeat=0 \
    --name true_random_fragmentation
