# KV CMD

Tool to run individual KV store/retrieve/exist/delete/list commands.

Starting nvmf target with:

```
  sudo build/bin/nvmf_tgt -c examples/kv/local_rocksdb.json -s 256
```

Run the kv\_cmd utility from another shell with:

Store:
```
   python -c "print('test')" | sudo build/examples/kv_cmd -r 'trtype:TCP adrfam:IPv4 traddr:127.0.0.1 trsvcid:4420 subnqn:nqn.2016-06.io.spdk:cnode1' -d 256 -C -k aaaaaaaaaaaaaaaa -c store
```

Retrieve:
```
   sudo build/examples/kv_cmd -r 'trtype:TCP adrfam:IPv4 traddr:127.0.0.1 trsvcid:4420 subnqn:nqn.2016-06.io.spdk:cnode1' -d 256 -C -k aaaaaaaaaaaaaaaa -c retrieve
```

Exist:
```
   sudo build/examples/kv_cmd -r 'trtype:TCP adrfam:IPv4 traddr:127.0.0.1 trsvcid:4420 subnqn:nqn.2016-06.io.spdk:cnode1' -d 256 -C -k aaaaaaaaaaaaaaaa -c exist
```
Response SC will be 0 if the key exists, and 135 if the key does not exist.

Delete:
```
   sudo build/examples/kv_cmd -r 'trtype:TCP adrfam:IPv4 traddr:127.0.0.1 trsvcid:4420 subnqn:nqn.2016-06.io.spdk:cnode1' -d 256 -C -k aaaaaaaaaaaaaaaa -c delete
```

List:
No support yet.
