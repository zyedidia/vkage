#!/bin/sh

DEV=$1

# 1. Per-request overhead: 4K random read, queue depth 1 (the honest number)
fio --name=lat --filename=$DEV --readonly --direct=1 \
  --rw=randread --bs=4k --iodepth=1 --runtime=15 --time_based

# 2. Batching behavior: 4K random read, queue depth 64
fio --name=iops --filename=$DEV --readonly --direct=1 \
  --rw=randread --bs=4k --iodepth=64 --ioengine=libaio --runtime=15 --time_based

# 3. Bandwidth: 1M sequential read, queue depth 8
fio --name=bw --filename=$DEV --readonly --direct=1 \
  --rw=read --bs=1M --iodepth=8 --ioengine=libaio --runtime=15 --time_based
