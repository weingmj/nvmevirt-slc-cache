#!/bin/sh

TARGET_DEV=/dev/nvme0n1

echo "Starting Synchronized Hot/Cold Test..."

# 바뀔 수 있는 파라미터 목록
# time_based & runtime은 묶임 (1, true로 설정하면 runtime 지정 필요)
# size / rate_iops
# 나는 600M / 2100M(합 2.7G) 정도로 hot / cold 나눔
# 디스크 총 크기는 3G로 잡았음

sudo fio - <<EOF
[global]
filename=$TARGET_DEV
direct=1
ioengine=libaio
bs=4k
rw=randwrite
norandommap=1
randrepeat=0
numjobs=1
group_reporting
time_based=1
runtime=120

[hot_job]
offset=0
size=600M
rate_iops=8000

[cold_job]
offset=600M
size=2100M
rate_iops=500
EOF