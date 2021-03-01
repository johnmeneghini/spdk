# KV_NULL debugging Examples

After compiling the repository with make, start the nvmf target with examples:

```
  sudo gdb --arg build/bin/nvmf_tgt -c examples/kv/local_kv.json -s 256
```

## KV Identify

```
  sudo  gdb --arg build/examples/identify -r 'trtype:TCP adrfam:IPv4 traddr:127.0.0.1 trsvcid:4420 subnqn:nqn.2016-06.io.spdk:cnode1' -d 256
```

## KV Perf

```
  sudo gdb --args build/examples/perf -r 'trtype:TCP adrfam:IPv4 traddr:127.0.0.1 trsvcid:4420  subnqn:nqn.2016-06.io.spdk:cnode1' -o 4096 -w randrw -M 50 -t 10 -q 8 -s 256
```

# RocksDB

## IO URING

Build with IO URING support in SPDK and RocksDB add '--with-uring'

```
  ./configure --enable-debug --with-rocksdb --with-uring`
```

Note: this is only supported on ssan-rx2560-03, which has a /dev/nvme device owned by the kernel.

## Creating a RocksDB JSON

Creating a rocksdb configuration with rpc commands:

```
  sudo build/bin/nvmf_tgt
```

In a different shell:

## TCP Transport

```
  sudo scripts/rpc.py nvmf_create_transport -t TCP -u 16384 -m 8 -c 8192
  sudo scripts/rpc.py bdev_rocksdb_create KV0 /tmp/rocksdb --backup-path /tmp/rocksdb_backup
  sudo scripts/rpc.py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001 -d SPDK_Controller1
  sudo scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 KV0
  sudo scripts/rpc.py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t tcp -a 127.0.0.1 -s 4420
```

## RDMA Transport

Add the following for RDMA support (only on ssan-rx2560-03)

```
sudo scripts/rpc.py nvmf_create_transport -t RDMA -u 16384 -m 8 -c 8192
sudo scripts/rpc.py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t rdma -a 192.168.101.8 -s 4420
```

Note: `-t rdma -a 192.168.101.8` is specific to ssan-rx2560-03. Your RDMA adapter may have a different address.

## JSON Config Save

Once you've configured your system, you can save the json configuration to a file with:

```
  sudo scripts/rpc.py save_config > ~/rocksdb_bdev.json
```

Then pass the path to the resulting joson to nvmf_tgt with the '-c' option:

```
  sudo build/bin/nvmf_tgt -c ~/rocksdb_bdev.json -s 256
```

# RockDB Database Dump - RDMA

Note: this only works with the nvmf_tgt on ssan-rx2560-03 and the host on ssan-rx2560-02

First compile the same spdk branch on both ssan-rx2560-03 and -02 with:

```
   ./configure --enable-debug --with-rocksdb --with-uring
   time make -j $(nproc)
```

Next start the target on ssan-rx2560-03:

```
  sudo build/bin/nvmf_tgt -m 0xff -c examples/kv/ssan-rx2560-03_rdma_rocksdb_bdev.json -s 512
```

Run the kv-perf utility on ssan-rx2560-02:

```
  sudo build/examples/perf -r 'trtype:RDMA adrfam:IPv4 traddr:192.168.101.8 trsvcid:4420  subnqn:nqn.2016-06.io.spdk:cnode1' -o 4096 -w randrw -M 50 -t 30 -q 8 -s 512
```

After stopping nvmf_tgt on ssan-rx2560-03, you can dump out the DB to txt files in the DB dir to peruse

```
  sudo rocksdb/sst_dump --file=/tmp/rocksdb --show_properties --command=none
  sudo rocksdb/sst_dump --file=/tmp/rocksdb --command=raw
```

End
