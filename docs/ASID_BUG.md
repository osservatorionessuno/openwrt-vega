# The ASID allocator bug on Nuclei UX608

A memory-corruption bug that broke every Linux kernel ≥ 5.12 on
Milk-V Vega until we identified and disabled the offending code path.
Documented here so future maintainers don't go through the same bisect.

## Root cause

Upstream Linux v5.12 added an ASID allocator for RISC-V
(`65d4b9c53017 RISC-V: Implement ASID allocator`). It probes the SATP
CSR's ASID field at boot:

1. Save current SATP.
2. Write all-ones into the ASID bits.
3. Read SATP back.
4. Count the bits that stuck.

Nuclei UX608 retains writes to the ASID bits, but the TLB silently
ignores ASIDs when matching translations.
With the allocator enabled, the kernel stops flushing TLBs across
context switches (it trusts ASIDs to disambiguate translations).

Other RISC-V SoCs upstream-tested at the time (Spike, SiFive Unleashed)
reported 0 ASID bits, so the allocator wasn't even enabled there. UX608
is the first SoC we know of that probes nonzero but doesn't honour ASIDs.

## The fix

Patch `target/linux/milkv_vega/patches-6.12/200-riscv-disable-asid-allocator.patch`
forces `asid_bits = 0` immediately after the probe in
`arch/riscv/mm/context.c::asids_init()`. The allocator path is gated by
`if (num_asids > 2 * num_possible_cpus())`, so this always routes us
through the legacy "local TLB flush on every MM switch" path, the
behaviour vendor's 5.8 kernel relied on.