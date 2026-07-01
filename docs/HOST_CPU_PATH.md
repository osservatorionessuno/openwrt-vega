# Host CPU Packet Path

This note records the verified relationship between the FSL91030M switch fabric
driver and the XY1000 packet-DMA host netdev. The packet-DMA mechanism is now
part of the production OpenWrt build as `xy1000-pdma`. The G5 management path
is proven in the production image; G6..G12 management is not yet proven.

## Conclusion

LuCI or any other OpenWrt service cannot be reached from the RJ45 switch ports
with the switch-fabric driver alone. A host packet path is also required.

The current production image proves that host path for G5 only. G6..G12 remain
pure switch-fabric datapaths in the validated MVP.

The host packet path is not just `xy1000_net`. It has two separate pieces:

- packet-DMA netdev: owns the packet-DMA register window, IRQs 10/11/12, the
  RX/TX scratch buffers, and the 8-byte private packet header;
- fabric CPU-port policy: owns switch tables that decide whether frames from
  external logical ports are admitted to the CPU packet path and whether CPU
  originated frames are released to external ports.

Those pieces stay separated in the production design. The packet-DMA driver
must not secretly mutate switch-fabric tables. The FSL91030M fabric driver owns
CPU-port admission/egress policy and must expose it through normal Linux
networking semantics once the full path is proven.

## Current OpenWrt State

The June 30, 2026 OpenWrt image includes the production packet-DMA module:

```text
kmod-fsl91030m-switch-6.12.89-r1
kmod-xy1000-pdma-6.12.89-r1
luci-26.167.35391~81ce1ad
tc-full-6.18.0-r2
```

The flashed artifacts were:

```text
kernel  cb3743160cde14d0b013923272a1b62c2e14293ab698ae6be4d1bebb3021e1af
rootfs  d9d6db734209a383294eb864f187b2c10d2f9b35432fd1266e5fab3b304508d8
```

Runtime validation on the flashed image:

- `xy1000-pdma 67800000.packet-dma` registered `xy0` with packet buffer
  `0x40000000-0x40007fff`.
- `xy0` starts down by default and is not a default `br-lan` port.
- packet-DMA hardware stays stopped at probe. It is reference-counted and
  starts when either `xy0` or a registered front-panel logical port is
  administratively opened. In the default OpenWrt bridge, `g5` and `g12` hold
  the DMA engine open even while `xy0` remains down.
- if RX_REQ is already pending when the first user opens the DMA engine, the
  driver immediately arms RX instead of waiting for another edge interrupt.
- `xy1000-pdma` no longer performs hidden switch-fabric writes during probe.
- G5 and G12 came up under the default `br-lan` after a cold boot, entered the
  Linux bridge, and serial remained responsive after the bridge-forwarding
  window.
- Clean-boot JTAG readback confirmed the CPU local-MAC IFWD CAM action is
  `0x07c80002 0x0`: static FEC redirect to logical CPU port 31, with
  `DST_TRAP` clear. Broadcast remains `0x6 0x0`: static plus `DST_TRAP`.
- Clean-boot JTAG readback confirmed IVT[31] is programmed with `FWD_VLD=1` and
  `OUT_LPORT=G5`, and the verified ingress/egress loop-control values are
  unchanged.
- G5 to `br-lan` `192.168.1.1` passed 5/5 with zero loss. The paired G5 capture
  contained exactly ten IPv4 ICMP packets, five requests and five replies, plus
  ARP. The paired G12 capture saw only one ARP broadcast and no ICMP leak.
- LuCI is reachable through G5; HTTP on `192.168.1.1` returns the expected
  redirect to `/cgi-bin/luci/`.
- G5 to G12 and G12 to G5 forwarding both passed 5/5 with zero loss after the
  host-path test.
- G12 to `br-lan` `192.168.1.1` is not proven and currently fails. The paired
  capture shows G12 ARP requests being flooded, but OpenWrt does not answer
  them on G12.
- Idle counter deltas were zero before traffic and again after traffic,
  including no new `rx_no_buffer_count` or `rx_missed_errors`.
- dmesg had no FisiLink, packet-DMA, panic, oops, BUG, call-trace, timeout, or
  table-DMA fault messages.

So the packet-DMA driver is production-integrated and mechanically working for
G5 management. G12 remains unclaimed for host management until CPU egress or
per-ingress CPU-port policy for that port is separately proven.

### FEC-only local MAC finding

The IFWD MAC CAM action for the local OpenWrt bridge MAC was tested both by live
JTAG mutation and by a clean boot from the production rootfs:

```text
slot 30 key     0x2206000d 0x5eb
slot 30 action  0x07c80002 0x0
slot 31 key     0xffffffff 0x1ffff
slot 31 action  0x6 0x0
```

Earlier code used `DST_TRAP` plus FEC-to-CPU for the local bridge MAC:

```text
slot 30 action  0x07c80006 0x0
```

That delivered two CPU RX copies. G5 ping to `192.168.1.1` reported duplicate
ICMP replies, and packet captures showed Linux generated two replies with
consecutive IPv4 IDs for the same request. Clearing `DST_TRAP` while keeping the
FEC redirect produced exactly one CPU RX copy and one reply per request.

## Verified Findings

Vendor default boot uses `xy1000_net.ko net_port=40`, but
`config.fhme` has:

```text
cpu_port_enable=0
cpu_port=0
```

In that state, G5 to G12 forwarding works, but ordinary host traffic does not
use the packet-DMA path:

- G5/G12 forwarding test: 20/20 packets forwarded.
- `eth0` and `vir0` RX/TX counters stayed zero during forwarding.
- packet-DMA IRQs stayed zero during forwarding.
- raw broadcast and raw unicast frames from G5 to the vendor `eth0` MAC did not
  increment RX_REQ/RX_END or `eth0` RX counters.
- `eth0` TX completed in the DMA block, but captures on G5 and G12 saw no
  external packets.

Enabling the SDK CPU-port branch with `cpu_port_enable=1 cpu_port=40` ran:

```text
cpu port app cfg enter!
cpu port app cfg exit!
```

but did not make G5 frames reach `eth0`, and `eth0` TX still did not appear on
G5/G12.

Enabling the SDK CPU-port branch with `cpu_port_enable=1 cpu_port=0` did make
raw unicast frames from G5 trigger packet-DMA RX:

- RX_REQ and RX_END increased.
- `eth0` RX packets and bytes increased.
- This effect was not fully undone by later setting `cpu_port_enable=0` and
  rerunning SDK init; a power cycle was needed to return to a clean default.

However, this was still not a usable Linux management data path:

- ARP for `10.13.5.1` remained unresolved on `eth0`.
- valid crafted ARP replies sent from G5 to `eth0` increased RX counters but did
  not populate `/proc/net/arp`.
- `eth0` TX completions occurred, but captures on G5 and G12 remained empty.

This means `cpu_port=0` proves the RX-DMA admission mechanism is real, but does
not prove a bidirectional Ethernet netdev suitable for LuCI.

After the CPU-port experiments, vendor G5/G12 forwarding entered a bad state in
which G5 broadcasts were visible on the controller G5 capture but not on G12.
Rerunning `fhcli`, rerunning `/usr/sbin/rc start`, restoring the saved
`config_usr.db`, and power-cycling did not recover forwarding. Reflashing the
known vendor UBIFS rootfs and cold booting did recover the baseline:

- `vega net ping 20`: 20/20 forwarded.
- Controller namespace pings passed in both directions.
- Raw G5 broadcast forwarding was restored.

The broken and restored states had identical decoded values for the narrow
tables checked in this pass: `I_NET_CTL`, `I_NET_DEF_VLAN_CTL`,
`I_FWD_BRG_CTL`, `I_FWD_LOOP_CTL`, `I_NET_LOOP_CTL`,
`I_NET_PORT_SRM[0/6/13]`, `I_NET_STP_SRM[0/6/13]`, and
`I_NET_VLAN_SRM[0..3]`. The bad state therefore was not explained by those
decoded ingress/FWD/VLAN/STP rows alone. Treat it as sticky state in an
unverified block or an un-dumped table until a smaller reproducer proves
otherwise.

On the restored vendor baseline, the default host-DMA relationship was
rechecked:

- G5/G12 forwarding and raw unicast frames to the vendor `eth0` MAC left
  packet-DMA IRQs 10/11/12 at zero and left `eth0`/`vir0` RX/TX counters at
  zero.
- `ping -I eth0 192.168.16.1` incremented `eth0` TX and `fbhm-mac_tx_end`, but
  G5/G12 captures still saw zero frames.
- G5/G12 forwarding remained healthy after the RX and TX host-DMA probes.

## Vendor Driver Behavior

The vendor packet driver is not fabric-neutral. During probe it calls
`fbhm_setpp()`, which mutates switch tables and loop controls:

- `I_VT_PORT_SRM[0]`: sets `FWD_VLD=1` and `OUT_LPORT=31`;
- `I_NET_PORT_SRM[0]`: clears `STP_CHK_EN` and `BRG_EN`;
- multiple ingress and egress loop-control registers are set to the vendor
  PP/OAM-style values.

The loop-control values match the runtime/vendor snapshots:

```text
I_FWD_LOOP_CTL = 0xbebebe
I_NET_LOOP_CTL = 0xbebe
I_DST_LOOP_CTL = 0x0
E_DST_LOOP_CTL = 0x0
E_ACL_LOOP_CTL = 0x90
E_EE_LOOP_CTL  = 0xb8
E_PF_LOOP_CTL  = 0xb8b8
E_POL_LOOP_CTL = 0xb8
I_ACL_LOOP_CTL = 0x3e
I_POL_LOOP_CTL = 0xbebe
I_PR0_LOOP_CTL = 0x80
```

These writes are not sufficient to provide host management traffic on G5/G12 in
the vendor default image. They appear to support the vendor OAM/PTP packet path,
not a normal DSA-style CPU port.

## Table Rows Observed

Vendor default rows observed across the clean and recovered default states:

```text
I_NET_PORT_SRM[0]  = 0x8040b or 0x8041b
I_NET_PORT_SRM[6]  = 0x8041b
I_NET_PORT_SRM[13] = 0x8041b
```

Both `I_NET_PORT_SRM[0]` values have been seen with ordinary G5/G12 forwarding
working. The G5/G12 datapath depends on logical ports 6 and 13 for the current
controller cabling; do not infer host-path behavior from the port-0 row alone.

After running SDK init with `cpu_port_enable=1 cpu_port=0`:

```text
I_NET_PORT_SRM[0] = 0x80001
```

The CLI decode shows this disables `USE_PKT_SVID`, `VLAN_FILTER_EN`, and clears
`SMV_FLAG` for logical port 0. G5 and G12 stayed at `0x8041b`.

## Future Integration Model

The current OpenWrt production driver set creates switchdev control netdevs for
`g5` through `g12`, and a packet-DMA CPU netdev `xy0`. G5 `.ndo_start_xmit`
uses the packet-DMA CPU path and the verified IVT[31] CPU-egress rule. G6..G12
`.ndo_start_xmit` still drops software TX because CPU egress for those ports is
not a proven data path. `xy0` is the CPU packet-DMA netdev, and the registered
RJ45 switchdev ports now hold the shared DMA engine open so external-to-CPU RX
can be serviced while `xy0` itself remains down. The fabric rules needed to
release CPU-originated frames back to G6..G12 are still not complete.

If LuCI or SSH must be reachable through any RJ45 port, both layers are needed, but
their ownership should stay separate:

- `xy1000` packet-DMA driver: maps only the packet-DMA register window, owns
  IRQs 10/11/12, DMA buffers, packet private-header encode/decode, NAPI or a
  bounded RX path, and a normal Linux netdev contract for packets that really
  enter or leave the CPU path.
- `fsl91030m` fabric driver: owns logical-port policy, bridge/VLAN/FDB/QoS
  tables, and any CPU-port admission or egress rules needed to steer packets
  between the switch fabric and the packet-DMA netdev.

The packet-DMA driver must not call a hidden equivalent of the vendor
`fbhm_setpp()` during probe. A production implementation should instead add an
explicit internal contract between the two drivers, or merge them under one
driver-private switch object, so CPU-port enable/disable, rollback, and traffic
tests are controlled by the fabric side. A DSA-style model may eventually be a
better Linux fit than the current direct switchdev-control-port model, but only
after the bidirectional packet-DMA path is proven.

## Production Implications

To add LuCI reachability through a switch port, implement and validate the
remaining pieces in order:

1. A bounded packet-DMA netdev driver for the XY1000 register window and IRQs.
   This is implemented as `xy1000-pdma` and validated for bounded
   external-to-CPU RX while `xy0` is down.
2. A fabric-owned CPU-port policy API in `fsl91030m`, not private writes inside
   the packet-DMA driver.
3. Proven CPU-to-external TX visible on G5 captures without loops or duplicate
   storms. This is implemented for G5 by IVT[31] egress to G5 plus the FEC-only
   local-MAC IFWD action.
4. Integration with OpenWrt networking as a standard CPU-facing netdev or DSA
   equivalent, with bridge/VLAN behavior tested in combination with the existing
   G5/G12 switchdev features.
5. A separate G12 management design if management must also be reachable from
   G12; the current production image intentionally claims only G5 management.

Evidence for this pass is under:

```text
runtime-artifacts/hostdma-g5-fec-only-20260630T1314Z/
runtime-artifacts/hostdma-vendor-compare-20260629T173919Z/
```
