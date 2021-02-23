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

# Rocksdb Submodlule

Rocksdb is now a submodule of SPDK. To start using this new submodule:

 - create a new SPDK repository with `setup-kv-store.sh firerock` and
 - configure the submodule with `./configure --enable-debug --with-rocksdb`

Rocksdb support is a configure option that can be enabled with:

```
./configure --enable-debug --with-rocksdb
```

And disabled with:

```
./configure --enable-debug --without-rocksdb
```

Note: in a pinch, use the following command to fix a broken repository:

```
  git config submodule.rocksdb.url https://bitbucket.eng.netapp.com/scm/kv-store-bb/rocksdb.git
  git submodule update --init
```
