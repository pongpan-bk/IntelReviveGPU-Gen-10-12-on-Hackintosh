# MyIntelGPU.kext — Work Memory (25 Jun 2026)

## Project Goal
Fix Intel GPU kext (Coffee Lake / Gen9–12) to work on macOS 15.2 Sequoia
(VMware Fusion VM, VMware20,1). The kext crashed at `start()` Phase 2 with
"Cannot map BAR0 MMIO".

---

## Phase 2 Fix (BAR0 MMIO Mapping) — SOLVED ✅

### Root Cause
`createMappingInTask(kernel_task, ...)` returns `null` on macOS 15+ because
`kernel_task` is restricted. The original code used this API exclusively and
fell through to the error path.

### Fix Applied (commits b01547d, ec42cce)

**BAR0 fetch — 3-strategy fallback chain:**
1. `getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0)` — preferred
2. `getDeviceMemoryWithIndex(0)` — fallback
3. Raw PCI config read (`configRead32(0x10)`) +
   `IOMemoryDescriptor::withPhysicalAddress()` — last resort

**BAR0 mapping — 3-strategy fallback chain:**
1. `IOMemoryDescriptor::map(kIOMapInhibitCache | kIOMapReadOnly)` — UC
2. `IOMemoryDescriptor::map()` — default caching
3. `createMappingInTask(kernel_task)` — legacy (macOS ≤14)

Same 3-strategy pattern applied to **BAR2 (aperture)**.

Build passes clean (0 errors, 0 warnings).
`kextutil -t` no longer shows BAR0 errors.

---

## Current Blocker — Kernel Panic in Interrupt Init 🚫

After `kextload` succeeds past Phase 2, the kext panics with **page fault in
`readReg32`** called from:
`start() → safeInitInterrupts() → initInterrupts() → clearAllInterruptRegisters()`

### Crash Signature (from JE4B's Mac)
```
RAX = 0x2000          ← fRegs virtual address ≈ 0x2000 (nearly NULL!)
RSI = 0x4646c         ← (fRegs + pipe offset + PIPE_IIR_OFFSET)
Offset in binary: 0x19e2  (inside readReg32)
```

### Hypothesis
`IOMemoryDescriptor::map()` returns a non-NULL `IOMemoryMap*`, BUT
`getVirtualAddress()` returns `0x2000` — a kernel-inaccessible address.

Possible causes:
1. **BAR0 physical address from PCI config is 0x2000** — mapping physical
   0x2000 gives a virtual mapping near 0x2000 (bogus/reserved address on
   VMware virtual GPU)
2. **VMware virtual GPU** has no real BAR0 — the address is a placeholder
3. `IOMemoryDescriptor::map()` itself returns a bogus VA on macOS 15.2

---

## System Info
| Field | Value |
|---|---|
| macOS | 15.2 (24C101) |
| VM | VMware Fusion (VMware20,1) |
| GPU | Virtual (no real Intel GPU) |
| `kextutil -t` | Code=27 (not approved → System Settings) |
| After approval | `kextload` → instant reboot (panic) |

---

## Key Files
| File | Purpose |
|---|---|
| `MyIntelGPU.cpp` | BAR0/BAR2 mapping logic, Phase 2 fix |
| `IntelFramebuffer.cpp` | Interrupt init, `clearAllInterruptRegisters()` |
| `MyIntelGPU.hpp` | Class declarations |
| `Makefile` | Build system |

---

## Next Debugging Steps
1. **Log BAR0 physical address** before mapping (from PCI config space)
2. **Add debug logging** for `map()` return and `getVirtualAddress()`
3. **Guard interrupt init** — only run if fRegs is in valid kernel space
   (`> 0xffffff8000000000`)
4. **Or defer interrupt init** entirely until BAR0 is confirmed mapped
5. **Check if VMware virtual GPU even has BAR0** — could need MMIO
   non-existent
