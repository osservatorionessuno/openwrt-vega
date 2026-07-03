# FisiLink Production Driver Surface

This document defines the supported production surface for the Milk-V Vega
FSL91030M switch driver. It is not a research log.

Research artifacts, register dumps, vendor comparisons, JTAG notes, devmem
experiments, raw packet crafting, controller scripts, and flash/power
orchestration belong under `runtime-artifacts/` or external lab notes. They must
not be installed into the OpenWrt image as driver APIs, target services, helper
scripts, debug knobs, or module options.

## Production Driver

The production FisiLink driver set currently has two separate drivers:

- `fsl91030m.ko`: FSL91030M L2 switch-fabric driver for the Milk-V Vega RJ45
  G5..G12 datapath.
- `xy1000-pdma.ko`: XY1000 packet-DMA host interface driver exposing the CPU
  packet path as `xy0`.

The old vendor-style packet driver remains excluded from the production
surface:

- no `xy1000_net.ko`;
- no `CONFIG_XY1000_NET`;
- no `kmod-xy1000-net`;
- no switch-side `eth0` netdev;
- no hidden packet-DMA probe writes into switch-fabric policy tables.

The packet-DMA driver owns only the packet-DMA MMIO block, IRQs, fixed packet
buffer, private 8-byte packet header, and standard Ethernet netdev operations.
Switch-fabric CPU-port admission and egress policy remains owned by the
`fsl91030m` fabric driver. The only in-kernel coupling between the two modules
is the GPL-only, in-tree logical-port registration/lifetime API used by
`fsl91030m` to bind RJ45 G5..G12 RX delivery to the packet-DMA engine.

`xy1000-pdma` must be inert at probe: it may register `xy0`, NAPI, and its
resources, but it must not start packet-DMA hardware or unmask packet-DMA IRQs
until `xy0` or a registered front-panel logical port is administratively opened.
When the last user stops, the driver masks the packet-DMA path again. Switchdev
CPU-originated TX through G5 is routed through the packet-DMA CPU path and the
verified IVT[31] egress rule. CPU-originated TX through G6..G12 is dropped
until per-port CPU egress is separately proven; this prevents the previously
observed packet-DMA TX loop/storm from becoming production behavior.

This means LuCI is installed in the OpenWrt image and is reachable through G5.
G6..G12 host management is not yet production surface; those ports are exposed
as switch-fabric bridge ports. The packet-DMA and CPU-port investigation is
documented in `docs/HOST_CPU_PATH.md`.

## Probe Surface

The only supported probes are normal Linux platform-driver lifetime callbacks
selected by devicetree:

- `fsl91030m_probe()`;
- `fsl91030m_remove()`;
- `module_platform_driver(fsl91030m_driver)`;
- `MODULE_DEVICE_TABLE(of, fsl91030m_of_match)`;
- compatible string `milkv,vega-fsl91030m-switch`;
- platform driver name `fsl91030m-switch`;
- `xy1000_pdma_probe()`;
- `xy1000_pdma_remove()`;
- `module_platform_driver(xy1000_pdma_driver)`;
- `MODULE_DEVICE_TABLE(of, xy1000_pdma_of_match)`;
- compatible string `milkv,vega-xy1000-pdma`;
- platform driver name `xy1000-pdma`.

Manual platform-driver bind/unbind is not a supported board operation. Both
drivers set `.suppress_bind_attrs = true`, so the production driver set must
not expose manual sysfs `bind` or `unbind` files.

`dev_err_probe()` is used only for normal resource-acquisition failures during
platform bind: missing or invalid `switch`, `dma`, or `dma-scratch` resources,
missing or invalid packet-DMA `regs` or `buffer` resources, IRQ acquisition, or
failed managed mappings.

No other meaning of "probe" is production driver surface. Register probing,
packet probing, JTAG probing, devmem/raw-MMIO probing, kprobes, trace probes,
vendor-diff probes, and packet mutation are testing/research methods only.

## Driver Surface

The supported production control/data surface is limited to standard kernel and
OpenWrt interfaces:

- devicetree node and binding for `milkv,vega-fsl91030m-switch`;
- devicetree node and binding for `milkv,vega-xy1000-pdma`;
- OpenWrt kernel package `kmod-fsl91030m-switch`;
- OpenWrt kernel package `kmod-xy1000-pdma`;
- module autoload for `fsl91030m`;
- module autoload for `xy1000-pdma`;
- Linux-visible switchdev control netdevs `g5` through `g12`;
- Linux-visible packet-DMA netdev `xy0`;
- OpenWrt default LAN bridge membership for `g5 g6 g7 g8 g9 g10 g11 g12`;
- OpenWrt default netdev LED triggers for RJ45 ports `g5` through `g12`
  following the verified sequential front-panel LED map;
- standard rtnetlink/netdev carrier, admin state, and cached stats;
- standard bridge and switchdev notifications for the verified features below.
- standard `tc` qdisc offload for the narrow per-port (`g5`..`g12`) QoS profiles
  listed below (G5/G12 traffic-validated; G6..G11 by symmetry, untested).

Only Linux bridge uppers are supported for these netdevs. Other upper devices,
including VLAN subinterfaces, bonds, teams, macvlan, and similar stacking
devices, are rejected at `NETDEV_PRECHANGEUPPER` before the upper becomes
visible.

The hardware-backed features currently exposed are:

- fixed RJ45 G5..G12 port map, with G6..G11 induced from the verified sequential
  MAC/logical-port/PHY mapping and checked by boot-time readback;
- bridge membership gating for RJ45 G5..G12;
- Linux bridge-only upper-device validation;
- bridge STP port state mapping for RJ45 G5..G12;
- bridge ageing time offload with hardware-range clamp/rounding;
- bridge VLAN table admission for the packet-proven VID1107 G5/G12 row only:
  tagged membership on both ports, `iNetPortSrm.vlanFilterEn` ingress
  filtering on both ports, and per-egress CTAG stripping through the EPF
  `un_ctag` bit;
- hard rejection of unsupported VLAN-table additions while bridge VLAN filtering
  is enabled. With the VID1107 service active, command rejection plus
  `iNetPortSrm.vlanFilterEn` is not sufficient by itself: the datasheet says an
  invalid `iNetVlanSrm` row falls back to `iNetDefVlanCtl`, and runtime confirmed
  that a raw VID200 frame still forwarded G5 to G12 until default VLAN
  membership was suppressed while bridge VLAN filtering was active;
- no hardware claim for arbitrary VIDs, PVID/untagged ingress insertion,
  VLAN-scoped FDB/FID isolation, or complete Linux bridge VLAN filtering;
- rejection of non-default bridge port flag states outside the verified port
  policy;
- fixed MTU at `ETH_DATA_LEN`;
- CMAC counter translation into standard `rtnl_link_stats64` packet, byte, and
  multicast counters;
- packet-DMA lifetime coupling where opened RJ45 switchdev ports hold the shared
  DMA engine active while `xy0` remains administratively down;
- G5 host management through packet-DMA RX/TX, using FEC-only local-MAC CPU
  admission and IVT[31] CPU egress to G5;
- deliberate rejection/drop of unverified CPU injection paths, including
  G6..G12 host management;
- per-port `g5`..`g12` ETS/RED offload through standard `tc`, bounded to the safe profiles in
  the QoS section below.

Unsupported operations must fail through standard Linux error paths or remain
unhandled by switchdev. They must not create raw register, debug, or unbounded
private APIs.

## QoS Surface

The supported QoS surface is the standard Linux traffic-control API on the
copper switchdev netdevs `g5`..`g12`. The driver sets `NETIF_F_HW_TC` and
handles `.ndo_setup_tc`; it does not register a FisiLink-specific QoS
generic-netlink family, private ioctl, debugfs file, sysfs file, or LuCI page.
The egress scheduler and QWRED tables are per-logical-port with a fixed stride,
so the safe profiles below apply uniformly to every copper port. The `g5` and
`g12` rows are runtime traffic-validated (ETS and RED offload accepted, G5/G12
forwarding 15/15 with QoS active on both, taint 0); `g6`..`g11` are enabled by
structural symmetry with those rows and have **not** been individually
traffic-tested.

The currently accepted `tc` offloads are deliberately narrow:

- `TC_SETUP_QDISC_ETS`: per-port root ETS with eight bands, all Linux priorities
  mapped to band 7, and only band 7 using ETS quantum. This programs that
  port's egress scheduler row (`bmp=0x80`, WRR mode, quantum 1,
  weight 1 for each hardware queue). ETS destroy restores the verified default
  scheduler row;
- `TC_SETUP_QDISC_RED`: RED attached at root or to ETS band 8. This maps to that
  port's queue 7 green QWRED thresholds. ECN, harddrop, nodrop, out-of-range
  thresholds, and non-q7 parents are rejected. RED offload state is tracked
  per port.

`sch_red` does not fail the netlink qdisc change when a driver's RED setup
callback rejects a profile. The driver therefore tracks the exact active RED
handle and parent internally, restores the G12 QWRED row to the verified default
on unsupported RED replaces, and reports RED stats/offload success only for the
active supported profile. Unsupported RED qdiscs may remain installed in Linux
software, but `tc qdisc show` must not report them as offloaded. RED destroy
callbacks from stale qdiscs are ignored unless their handle and parent still
match the active offloaded RED state; this is required because Linux may destroy
the old qdisc after a replacement qdisc has already been installed.

The driver intentionally does not offload priority-to-queue classification,
DSCP classification, queue counter enable, arbitrary WRED colors/queues, or the
raw queue shaper fields. Hardware validation on June 29, 2026 showed that G12
queue-counter enable and G12 priority 7 to queue 7 mutation disrupt G5-to-G12
forwarding until later state changes, while G5 DSCP classification disrupts
both directions and does not recover after the disable sequence. The queue
shaper registers were safe to write/read in isolation, but their units and
token-bucket semantics have not been mapped cleanly to the Linux `tbf` offload
ABI, so they are not production surface yet.

The externally visible production surface is:

- module/package: `fsl91030m.ko`, `kmod-fsl91030m-switch`,
  `xy1000-pdma.ko`, `kmod-xy1000-pdma`, and standard module autoload files;
- QoS control: standard `tc` qdisc operations on `g12` for the supported ETS
  and RED profiles above;
- devicetree: one `switch@60000000` node with compatible
  `milkv,vega-fsl91030m-switch` and `switch`, `dma`, `dma-scratch` resources,
  plus one `packet-dma@67800000` node with compatible
  `milkv,vega-xy1000-pdma` and `regs`, `buffer` resources;
- kernel lifetime: OF platform-driver probe/remove only, with manual
  bind/unbind suppressed;
- netdevs: `g5` through `g12` as fixed-MTU switchdev bridge control ports, all
  reporting the same stable switchdev `phys_switch_id` derived from the
  devicetree-selected switch MMIO resource base, plus `xy0` as the packet-DMA
  CPU netdev;
- netdev operations: open/stop, transmit-drop accounting for unsupported CPU
  injection, fixed-MTU rejection, stats64, address validation, and switchdev
  parent ID;
- bridge/switchdev notifications: bridge join/leave, STP state, bridge ageing,
  narrow VID1107 VLAN-table object handling, and explicit rejection of
  unsupported VLAN additions and bridge port flag states;
- upper-device policy: non-bridge uppers are rejected during
  `NETDEV_PRECHANGEUPPER`;
- OpenWrt board defaults: `br-lan` membership for
  `g5 g6 g7 g8 g9 g10 g11 g12`, plus RJ45 G5..G12 link/activity LEDs mapped to
  standard netdev LED triggers.
- internal module coupling: GPL-only, in-tree `xy1000_pdma_*lport*()` helpers
  used only by `fsl91030m` for logical-port RX registration and packet-DMA
  lifetime management.

No other switch capability is production driver surface until its register model,
rollback behavior, Linux ABI mapping, and traffic behavior are verified.

## Non-Surface

The production driver and target image must not expose any FisiLink-specific:

- debugfs files;
- procfs files;
- custom sysfs attributes;
- module parameters;
- private ioctls;
- devlink regions or health reporters;
- private ethtool hooks, register dumps, or self-tests;
- misc, char, or raw MMIO devices;
- exported-symbol APIs beyond the private in-tree `xy1000_pdma_*lport*()`
  coupling consumed by `fsl91030m`;
- target-side packet generators;
- target-side register read/write helpers;
- JTAG, serial, devmem, rawexec, flash, or power-control hooks;
- feature gates for unverified hardware blocks.

Internal write-verify and rollback helpers are production implementation
details, not user-facing probes. They are allowed only because they protect the
verified switch state from silent hardware-programming failures.

## Testing Surface

Testing is external to the production image. Accepted testing methods are:

- host-side source scans and generated-patch audits;
- `checkpatch.pl` on driver, binding, DTS, SPL, and U-Boot patches;
- OpenWrt target/package builds;
- direct generated-kernel module rebuilds with `W=1`;
- sparse and Coccinelle checks;
- devicetree schema validation and compiled DTB inspection;
- module/package payload inspection;
- controller-side traffic tests on `enp2s0` and `enp3s0`;
- controller-side packet crafting and capture for G5/G12 validation;
- switch serial logs, power cycle, NAND/NOR flashing, and JTAG for recovery or
  investigation.

These test mechanisms must remain outside the target image and outside the
kernel driver's ABI.

Test-only probes are allowed only as lab procedure, never as target ABI. That
includes register reads through JTAG/devmem during investigation, controller-side
raw packet generation, switch serial observation, vendor-image register diffs,
and temporary host-side source analysis scripts. None of those may appear as a
FisiLink kernel API, target service, target helper, module parameter, sysfs file,
debugfs file, ethtool hook, devlink region, ioctl, or packet generator in the
OpenWrt image.

## Current Evidence

Current switch/fabric and packet-DMA surface evidence is recorded in:

- June 30, 2026 runtime check on rootfs
  `d9d6db734209a383294eb864f187b2c10d2f9b35432fd1266e5fab3b304508d8`:
  default `br-lan` with G5/G12 booted, `xy0` remained down, G5/G12 opened the
  shared packet-DMA engine, clean-boot JTAG readback confirmed local IFWD CAM
  action `0x07c80002 0x0` and broadcast CAM action `0x6 0x0`, G5 to
  `192.168.1.1` passed 5/5 with one ICMP reply per request, LuCI HTTP was
  reachable through G5, G5 to G12 and G12 to G5 forwarding passed 5/5 after the
  host-path test, G12 to `192.168.1.1` remained unclaimed and failed at ARP,
  idle counter deltas stayed zero before and after traffic, serial remained
  responsive after traffic, and no relevant kernel fault messages were present;
- `runtime-artifacts/hostdma-g5-fec-only-20260630T1314Z/`;
- `runtime-artifacts/prod-switch-only-surface-20260623T-current/`;
- `runtime-artifacts/prod-driver-surface-audit-20260623T-current/`;
- `runtime-artifacts/prod-driver-c-audit-20260623T-current/`;
- `runtime-artifacts/prod-probe-driver-testing-surface-20260623T175104Z/`;
- `runtime-artifacts/prod-source-hardening-20260623T-current/`;
- `runtime-artifacts/prod-lifecycle-bridged-runtime-20260623T180846Z/`;
- `runtime-artifacts/prod-source-contract-cleanup-20260623T-current/`;
- `runtime-artifacts/prod-openwrt-integration-surface-20260623T-current/`;
- `runtime-artifacts/prod-lifetime-bounds-hardening-20260623T-current/`;
- `runtime-artifacts/prod-binding-board-init-audit-20260623T-current/`;
- `runtime-artifacts/prod-dead-code-symbol-audit-20260623T-current/`;
- `runtime-artifacts/prod-probe-driver-testing-surface-reaudit-20260623T-current/`;
- `runtime-artifacts/prod-switchdev-netdev-audit-20260623T-current/`;
- `runtime-artifacts/prod-board-init-readability-audit-20260623T-current/`;
- `runtime-artifacts/prod-access-dma-safety-audit-20260623T-current/`;
- `runtime-artifacts/prod-switchdev-lifetime-concurrency-audit-20260623T-current/`;
- `runtime-artifacts/prod-netdev-abi-runtime-20260623T-current/`;
- `runtime-artifacts/prod-source-quality-surface-audit-20260623T-current/`;
- `runtime-artifacts/prod-switchdev-lifetime-prechange-audit-20260623T-current/`;
- `runtime-artifacts/prod-nonbridge-upper-rejection-20260623T-current/`;
- `runtime-artifacts/prod-join-idempotence-hardening-20260623T-current/`;
- `runtime-artifacts/prod-probe-driver-testing-surface-reaudit2-20260623T-current/`;
- `runtime-artifacts/prod-dma-status-hardening-20260623T-current/`;
- `runtime-artifacts/prod-dma-status-runtime-20260623T-current/`;
- `runtime-artifacts/prod-probe-surface-runtime-20260623T-current/`;
- `runtime-artifacts/prod-probe-resource-guard-20260624T-current/`;
- `runtime-artifacts/prod-driver-probe-surface-audit-20260623T234329Z/`.
- `runtime-artifacts/hostdma-production-openwrt-20260629T223649Z/`.

The accepted current finding is, with older artifact-specific bullets retained
only as provenance where noted:

- `fsl91030m` and `xy1000-pdma` are built, packaged, autoloaded, and exposed
  for the FisiLink production target;
- only the devicetree-selected `fsl91030m_probe()`/`fsl91030m_remove()` platform
  and `xy1000_pdma_probe()`/`xy1000_pdma_remove()` lifetimes are probe surface;
- manual platform bind/unbind is suppressed;
- `CONFIG_DEVMEM`, `CONFIG_DEBUG_FS`, and `CONFIG_PROC_KCORE` are disabled in
  the target kernel config, and the FisiLink driver/package/rootfs files expose
  no debugfs, tracefs, devmem, or raw-control path;
- no FisiLink debug, raw-control, private ioctl, devlink, module-parameter, or
  target helper surface is present;
- the staged OpenWrt rootfs contains only the expected FisiLink driver files:
  the `fsl91030m.ko` and `xy1000-pdma.ko` modules, their standard module
  autoload files, boot-autoload symlinks, and package manifests;
- bidirectional G5/G12 bridge traffic, standard bridge mutations, unsupported
  feature rejection, module lifetime, taint, stats, and kernel fault scans have
  been validated through the current artifacts;
- the packet-DMA runtime confirms `xy0` registration, G5/G12 logical-port opens
  starting the shared DMA engine while `xy0` stays down, bounded ARP/ICMP RX
  delivery to Linux, and proven CPU-to-G5 egress when the IFWD local-MAC action
  is FEC-only and IVT[31] targets G5. Older packet-DMA artifacts also validated
  crafted/custom-EtherType RX delivery, TX completion accounting, clean
  unload/reload, and no external G5/G12 egress from `xy0` TX without additional
  fabric CPU-port policy;
- LuCI is packaged and reachable from G5. G12 pings to `br-lan` `192.168.1.1`
  still fail with incomplete ARP, so G12 host management is not yet production
  surface;
- module unload/reload while both ports are actively enslaved to `br-lan`,
  bridge delete/recreate, both-port `nomaster`, admin-state, STP-state
  mutations, final counter deltas, taint, and kernel fault scans have a clean
  end-to-end runtime pass in
  `runtime-artifacts/prod-lifecycle-bridged-runtime-20260623T180846Z/`;
- the current production source has passed canonical/generated parity,
  forbidden-surface scans, duplicate include/declare checks, strict checkpatch,
  local-definition-use scanning, full OpenWrt target compile, forced generated
  `W=1` module rebuild, package compile, and package-payload surface inspection
  in `runtime-artifacts/prod-source-contract-cleanup-20260623T-current/`;
- the probe/remove, switchdev notifier teardown, access-bounds, write-verify,
  rollback, sparse, Coccinelle, generated-source parity, and target/rootfs
  forbidden-surface checks have an additional clean pass in
  `runtime-artifacts/prod-lifetime-bounds-hardening-20260623T-current/`;
- the fixed board-init register programming has been re-audited for removable
  or stale code, the stale `PKT_DMA` address wording has been removed, and the
  switch binding/DTS node validate cleanly in
  `runtime-artifacts/prod-binding-board-init-audit-20260623T-current/`;
- macro/reference counts, compiled module symbol/source-reference counts,
  exported-symbol scans, module-parameter scans, strict checkpatch, duplicate
  include/declare checks, generated `W=1`, sparse, parity, and forbidden-surface
  scans have a clean dead-code/symbol audit in
  `runtime-artifacts/prod-dead-code-symbol-audit-20260623T-current/`;
- the pre-host-DMA probe/surface re-audit confirmed that the image payload then
  contained only `fsl91030m.ko`, standard autoload files, and package metadata
  for the FisiLink switch driver. In the current image, the expected payload is
  `fsl91030m.ko`, `xy1000-pdma.ko`, their standard autoload files, and package
  metadata. The current driver set exposes no FisiLink debugfs, procfs, custom
  sysfs, module-parameter, devlink, private ethtool, ioctl, trace, kprobe,
  raw-control, devmem, JTAG, panic, or `BUG()` surface;
- the switchdev/netdev semantic audit confirms the direct-port
  `switchdev_bridge_port_offload()` shape, `NETDEV_CHANGEUPPER` unoffload path,
  unsupported-object handling, and standard probe surface against Linux 6.12
  bridge/switchdev internals and in-tree driver patterns; the same pass includes
  clean surface scans, strict checkpatch, source parity, forced `W=1` module
  rebuild, and a clean full G5/G12 runtime lifecycle regression;
- the board-init readability audit confirms that fixed traffic-map and
  traffic-schedule initialization remains board-baseline convergence state, not
  exposed all-port or QoS surface; it removes stale `24-port fabric map` wording
  and rechecks local macro use, stale/debug strings, source parity, strict
  checkpatch, and forced `W=1` module rebuild;
- the access/DMA safety audit confirms the direct-MMIO and TDMA helper bounds,
  scratch-buffer and DMA-resource validation, runtime `op_lock`/`age_lock`
  serialization, absence of private register/debug/test ABI, strict checkpatch,
  generated-source parity, forced `W=1` rebuild, and forced sparse pass with no
  driver-origin sparse warnings;
- the switchdev lifetime/concurrency audit confirms notifier registration and
  cleanup balance, delayed-stats cancellation before netdev teardown,
  RTNL-grounded bridge offload/unoffload/replay usage, bridge-join rollback
  ordering, in-tree direct-port switchdev comparison, stack scan, source parity,
  and absence of private switchdev debug/test ABI;
- the pre-host-DMA netdev/probe ABI runtime confirmed that only `fsl91030m` was
  present at runtime. In the current image, `fsl91030m` and `xy1000_pdma` are
  both expected. The old `xy1000_net`/`eth0` packet-DMA surface remains absent,
  `/dev/mem`, debugfs, and tracefs are absent, manual bind/unbind remains
  outside the supported surface, fixed-MTU rejection, VLAN-filtering rejection,
  unsupported bridge-port-flag rejection, CPU-injection `tx_dropped`
  accounting, bidirectional forwarding recovery, kernel taint, and fault scans
  all pass in `runtime-artifacts/prod-netdev-abi-runtime-20260623T-current/`;
- the source-quality/surface audit confirms that the current production driver
  has no detected unused register macros or real compiled helper functions, no
  private debug/raw-control/testing ABI, strict checkpatch, duplicate
  include/declaration checks, source parity, forced generated `W=1` rebuild, and
  forced sparse all complete with no driver-origin defects; the only direct
  build caveat is OpenWrt's target-wide `CONFIG_MODULE_STRIPPED=y` policy, which
  strips standard module description and OF alias metadata even though the source
  carries the normal `MODULE_DEVICE_TABLE()`, `MODULE_DESCRIPTION()`, and
  `MODULE_LICENSE()` macros;
- the switchdev lifetime/pre-changeupper audit moves bridge-join validation into
  a shared helper used by both `NETDEV_PRECHANGEUPPER` and the final
  `NETDEV_CHANGEUPPER` bridge-join path, so unsupported bridge VLAN-filtering
  and multi-bridge states are rejected before transient upper-device linkage and
  rechecked before hardware programming; strict checkpatch, source parity,
  forbidden-surface scan, forced generated `W=1` rebuild, and forced sparse pass
  all complete, with no FisiLink source diagnostics and only the already-known
  OpenWrt `CONFIG_MODULE_STRIPPED=y` modpost metadata warning; the same pass
  rebuilt and flashed a full OpenWrt image, then confirmed packaged
  runtime with taint `0`, pre-changeupper VLAN-filtered bridge-join rejection,
  post-join VLAN/bridge-port-flag rejection, bridged `rmmod`/`modprobe`, G5/G12
  forwarding recovery, and clean final dmesg fault scans.
- the non-bridge-upper rejection pass extends the same
  `NETDEV_PRECHANGEUPPER` validation to all linking upper devices, not only
  bridges, so the production surface remains bridge/switchdev-only; the flashed
  packaged image confirms no switch-side `eth0`, no `/dev/mem`, no debugfs or
  tracefs mount, no manual platform-driver `bind`/`unbind`, no module
  parameters, rejected `g5.100` creation with `RTNETLINK answers: Not
  supported`, no residual `g5.100` netdev, post-rejection bidirectional G5/G12
  forwarding recovery, clean bridged `rmmod`/`modprobe`, final taint `0`, and
  clean final kernel fault scans in
  `runtime-artifacts/prod-nonbridge-upper-rejection-20260623T-current/`. The
  OpenWrt BusyBox/ip userspace used in that run did not print the driver's
  extack text, so the runtime proof is the failing syscall and absent upper
  device rather than visible extack text. The first packaged run saw one
  transient 7/8 ping immediately after reconfiguration; an immediate controller
  recheck and a full retry-capable runtime pass both completed with 0% packet
  loss and the final `PRECHANGE_PACKAGED_RUNTIME_OK` marker.
- the bridge-join idempotence hardening pass makes duplicate same-bridge upper
  notifications return success after validation instead of attempting a second
  switchdev offload/replay, and reflows the fixed traffic-schedule board-init
  comment so scheduler/shaper baseline programming remains clearly documented as
  board convergence state rather than QoS production API. The pass has clean
  strict checkpatch for the touched source files, canonical/OpenWrt/generated
  source parity for touched files, no forbidden production-surface strings,
  clean generated `W=1` module rebuild except for the known OpenWrt
  `CONFIG_MODULE_STRIPPED=y` metadata warning, and a sparse-wrapper pass with no
  driver-origin diagnostics in
  `runtime-artifacts/prod-join-idempotence-hardening-20260623T-current/`.
- the pre-host-DMA probe/driver/testing surface re-audit confirmed that "probe"
  means only normal OF platform-driver bind/remove for
  `milkv,vega-fsl91030m-switch`; the current host-DMA image extends that normal
  OF platform-driver model to `milkv,vega-xy1000-pdma`. The production tree has
  no FisiLink debug/probe/raw-control/testing ABI; strict checkpatch, source
  parity, generated `W=1`, sparse-wrapper, focused DT binding validation, and
  DTB compilation passed in the recorded audit in
  `runtime-artifacts/prod-probe-driver-testing-surface-reaudit2-20260623T-current/`;
- the DMA status hardening pass maps the datasheet-defined table-DMA response
  error bit (`DMA_CHX_IRQ_STAT/CLR` bit 2) into production access error handling,
  verifies stale completion/error status clears before each table transfer,
  clears status after each transfer, and fails closed on response/access error or
  stuck status. Strict checkpatch, canonical/OpenWrt/generated source parity,
  forbidden-surface scans, generated `W=1`, sparse-wrapper, and stack checks pass
  in `runtime-artifacts/prod-dma-status-hardening-20260623T-current/`;
- the DMA status runtime pass rebuilds the full OpenWrt image from
  the hardened source, flashes NAND kernel/rootfs, boots the packaged image, and
  revalidates G5/G12 bridge forwarding, unsupported-surface rejection, bridged
  module unload/reload, kernel taint, and fault scans. A focused follow-up pass
  verifies bidirectional 12/12 traffic, increasing standard `g5`/`g12` packet and
  byte counters, no `table DMA failed` or status-clear timeout dmesg messages,
  and final taint `0` in
  `runtime-artifacts/prod-dma-status-runtime-20260623T-current/`.
- the pre-host-DMA probe-surface runtime pass makes the switchdev parent identity
  deterministic from the probed switch MMIO resource and revalidates it on a
  freshly flashed NAND image. That runtime confirmed only `fsl91030m` was
  present at the time; in the current image, `fsl91030m` and `xy1000_pdma` are
  both expected. `xy1000_net`/switch-side `eth0` remain absent, manual platform
  `bind`/`unbind`, module parameters, `/dev/mem`, `/dev/port`, debugfs, and
  tracefs are absent, `g5` and `g12` both report
  `phys_switch_id=f591030d0000000060000000` before and after `rmmod`/`modprobe`,
  bidirectional bridge forwarding works, unsupported MTU/VLAN/bridge-port flag
  mutations are rejected, STP/admin/join/leave mutations block and restore
  traffic as expected, standard packet/byte counters increase under
  bidirectional traffic, no table-DMA error messages are logged, final taint is
  `0`, and the final kernel fault scan is clean in
  `runtime-artifacts/prod-probe-surface-runtime-20260623T-current/`.
- the probe resource-guard pass makes the platform probe fail closed if the
  devicetree `switch` resource base does not match the fixed Milk-V Vega
  switch/TDMA aperture used for table-DMA destination addresses. This prevents
  direct MMIO and table-DMA from silently targeting different apertures on a bad
  DT. The pass also adds a compile-time assertion that the fixed aperture plus
  the supported switch window fits in the DMA engine's 32-bit address registers.
  Touched-file checkpatch, source parity, forbidden-surface scan, unused
  register/driver macro scan, generated `W=1` module rebuild, focused DT
  binding check, targeted DTB check, and sparse-wrapper pass complete in
  `runtime-artifacts/prod-probe-resource-guard-20260624T-current/`.
- the VLAN filtering/table pass implements the datasheet-derived fallback model
  for invalid VLAN rows: while Linux bridge VLAN filtering is active, G5/G12 are
  removed from the default `iNetDefVlanCtl` membership bitmap so packets that do
  not match a programmed `iNetVlanSrm` row cannot fall through to the unfiltered
  bridge domain. The LuCI-enabled image in
  `runtime-artifacts/prod-vlan-filter-default-members-20260629T002540Z/`
  rebuilds, checksum-verifies, flashes, and boots cleanly. Runtime confirms
  unfiltered G5/G12 ping in both directions, `vlan_filtering=1` acceptance,
  VID200 and VID1107-PVID command rejection, VID1107 member admission on G5/G12,
  untagged ping blocked while filtering is active, raw VID200 ingress on G5 not
  forwarded to G12, raw VID1107 forwarding in both directions, deleting G12 from
  VID1107 blocking G5-to-G12 forwarding, re-adding G12 as `Egress Untagged`
  stripping the CTAG on egress, `vlan_filtering=0` restoring ordinary untagged
  G5/G12 forwarding, and a clean final fault scan with no panic, oops, BUG,
  call trace, or driver warning.
- the historical driver/probe surface audit removes an unused named enum tag from the
  production header while keeping the verified port constants, then revalidates
  the complete current production surface: no forbidden debug/raw/vendor/test
  strings in driver, target, or rebuilt module; no private user-facing export or
  control API; no
  unused register/driver macros; no single-use `fsl91030m_*` source symbols;
  canonical/OpenWrt/generated source parity; strict checkpatch over the complete
  driver/binding/DTS/package patch surface; generated-kernel `W=1` module
  rebuild; and sparse-wrapper pass with no driver-origin diagnostics. The only
  rebuild warning is the known OpenWrt `CONFIG_MODULE_STRIPPED=y` modpost
  metadata warning, with source `MODULE_DEVICE_TABLE()`, `MODULE_DESCRIPTION()`,
  and `MODULE_LICENSE()` confirmed in
  `runtime-artifacts/prod-driver-probe-surface-audit-20260623T234329Z/`.
- the PLL-init removal pass removes the previously unverified `pll_pd_ctrl`
  register definition and mode-init write from the production driver. The
  remaining board-init sequence keeps only the verified work-mode, PCS, RGMII,
  reset, XSGMII, fabric-map, scheduler-baseline, and bridge-gate programming
  used by the G5/G12 datapath. Canonical/OpenWrt/generated source parity, a
  no-PLL-init scan over all FisiLink driver copies, forbidden-surface scans,
  strict checkpatch over the complete driver/binding/DTS/package patch surface,
  forced clean generated-kernel `W=1` module rebuild, source module metadata
  confirmation, sparse-wrapper pass with no driver-origin diagnostics, no
  forbidden rebuilt-module strings, no exported module symbols in that
  pre-host-DMA module set, no unused
  register/driver macros, no single-use `fsl91030m_*` source symbols, and the
  scoped target/package surface scan all complete in
  `runtime-artifacts/prod-remove-pll-init-surface-20260624T000714Z/`.
- the QoS production-surface RED lifecycle pass removes the private QoS
  package/LuCI surface and exposes the verified G12 QoS profiles only through
  standard `tc` qdisc offload. The flashed LuCI-enabled image in
  `runtime-artifacts/tc-qos-red-destroy-production-20260629T150637Z/` rebuilds,
  checksum-verifies, flashes, and boots cleanly. Runtime confirms baseline
  G5/G12 forwarding, root ETS offload plus traffic, RED under ETS band 8
  offload plus traffic, unsupported ECN/harddrop RED remaining software-only,
  supported RED replacements regaining `offloaded` after both unsupported cases,
  standalone root RED offload plus traffic, unsupported root ECN remaining
  software-only, supported root RED replacement regaining `offloaded`, final
  cleanup to `qdisc noqueue`, final bidirectional forwarding, 1G/full controller
  links with zero sampled controller NIC errors, and no FSL driver fault log
  entries.
