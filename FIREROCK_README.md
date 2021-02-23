# NetApp Firerock Development Branch

This is the main NetApp SPDK development branch for Firerock.

  - All modifications made to SPDK by NetApp should be made in a sub-branch off of firerock-dev3.
  - All modifcations made to the SPDK code will be BSD licensed, open source software.
  - Modifications made to this branch may be donated back to the SPDK project.
  - Modifications made to this branch are subject to the NetApp OSRB code review process.

Contact: johnm@netapp.com for more information.

# KV-Store Repository Setup

The new Bitbucket KV-STORE project is located at: https://bitbucket.eng.netapp.com/projects/KV-STORE-BB

Use the new setup-kv-store.sh setup script to set up a local working repository: `/x/eng/site/smokejumper/spdk/autotest/setup-kv-store.sh

To create a working copy of this repository use command:

```
    setup-kv-store.sh -b firerock-dev3 firerock
```

# Dev Build Env Setup

```
   cd firerock/spdk
   checkout -b my_dev_branch
   ./configure
   make
```

# Testbed Setup

```

Check if hugemem is configured
    sudo env NRHUGE=6144 scripts/setup.sh status
    Hugepages
    node     hugesize     free /  total
    node0   1048576kB        0 /      0
    node0      2048kB      306 /   1536
    node1   1048576kB        0 /      0
    node1      2048kB        0 /      0

	Type     BDF             Vendor Device NUMA    Driver           Device     Block devices
	NVMe     0000:83:00.0    8086   0953   1       uio_pci_generic  -          -

To enable hugepages - a necessary prerequisite for running SPDK apps, use:

  sudo env HUGEMEM=2560 scripts/setup.sh config
```

# SPDK KV BDEV examples

After compiling the repository with make, running the nvmf target with:

```
  sudo build/bin/nvmf_tgt -c examples/kv/local_kv.json -s 256
```
Note that the example creates a listener on port 4420 - if this machine is shared with other developers, each should create their copy of
the json file and modify the port in nvmf_subsystem_add_listener

## KV Identify

After starting the nvmf target run the identify script from another shell with:

```
   sudo build/examples/identify -r 'trtype:TCP adrfam:IPv4 traddr:127.0.0.1 trsvcid:4420 subnqn:nqn.2016-06.io.spdk:cnode1' -d 256
```

## KV Perf

After starting nvmf target run the perf utility from another shell with:

```
   sudo build/examples/perf -r 'trtype:TCP adrfam:IPv4 traddr:127.0.0.1 trsvcid:4420 subnqn:nqn.2016-06.io.spdk:cnode1' -o 4096 -w randrw -M 50 -t 10 -q 8 -s 256
```

See the README.md file in examples/kv for more information
