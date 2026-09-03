#!/bin/sh

sudo fio --filename=/dev/nvme0n1 \
    --direct=1 \
    --ioengine=libaio \
    --rw=randwrite \
    --bs=4k \
    --size=2700M \
    --io_size=30G \
    --numjobs=1 \
    --norandommap=1 \
    --randrepeat=0 \
    --random_distribution=zipf:0.8 \
    --name please