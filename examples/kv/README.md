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
