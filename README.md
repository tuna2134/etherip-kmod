# etherip-kmod

Out-of-tree Linux kernel module for RFC3378 EtherIP over IPv6 endpoints.

The module registers a rtnetlink link kind named `etherip6`.  A companion
`etherip6ctl` helper is included because stock iproute2 does not know how to
encode this module's private tunnel attributes.

## Build

On Arch Linux, install headers matching the kernel you will load into:

```sh
sudo pacman -S linux-headers
make
```

If you need to build against a specific headers tree:

```sh
make KDIR=/usr/lib/modules/$(uname -r)/build
```

## Load

```sh
sudo insmod etherip6.ko
```

## Create a Tunnel

```sh
sudo ./etherip6ctl add eip0 local 2001:db8:1::1 remote 2001:db8:1::2
sudo ip link set eip0 up
sudo ip addr add 192.0.2.1/30 dev eip0
```

For link-local IPv6 endpoints, specify the egress interface:

```sh
sudo ./etherip6ctl add eip0 local fe80::1 remote fe80::2 link eth0
```

Delete the device with:

```sh
sudo ./etherip6ctl del eip0
```

## Fragmentation

The transmit path permits local IPv6 fragmentation for encapsulated EtherIP
packets that exceed the outer path MTU.  This lets a larger inner Ethernet
frame be split into multiple outer IPv6 fragments by the local host.

You can still set a smaller tunnel MTU when you want to avoid IPv6 fragments:

```sh
sudo ip link set eip0 mtu 1444
```

For a 1500-byte outer link, 1444 leaves room for the 40-byte IPv6 header,
2-byte EtherIP header, and 14-byte Ethernet header carried inside EtherIP.

## Wire Format

The outer IPv6 packet uses Next Header 97.  The EtherIP header is the RFC3378
two-byte version/reserved field with version 3, followed by the complete
Ethernet frame.
