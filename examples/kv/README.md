# SPDK KV BDEV deguggin examples

Examples:

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

## rocksdb

Creating a rocksdb configuration with rpc commands:

```
  sudo build/bin/nvmf_tgt
```

In a different shell:
```
  sudo scripts/rpc.py nvmf_create_transport -t TCP -u 16384 -m 8 -c 8192
  sudo scripts/rpc.py bdev_rocksdb_create KV0 /tmp/rocksdb -b /tmp/rocksd_backup
  sudo scripts/rpc.py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001 -d SPDK_Controller1
  sudo scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 KV0
  sudo scripts/rpc.py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t tcp -a 127.0.0.1 -s 4420
```

Once you've configured your system, you can save the json configuration to a file with:
```
  sudo scripts/rpc.py save_config > ~/rocksdb_bdev.json
```
Then pass the path to the resulting joson to nvmf_tgt with the '-c' option:
```
  sudo gdb --arg build/bin/nvmf_tgt -c ~/rocksdb_bdev.json -s 256
```

After stopping nvmf_tgt, you can dump out the DB to txt files in the DB dir to peruse
```
rocksdb/sst_dump --file=/tmp/rocksdb --command=raw
```
Build with IO URING support in SPDK and RocksDb add '--with-uring'
`./configure --with-debug --with-uring`
