#include <stdint.h>
#include <sys/types.h>

#include <ps5/kernel.h>
#include <ps5/mdbg.h>

#include "cr_log.h"
#include "cr_mdbg.h"
#include "pt.h"

/* x86-64 page-table entry flags (machine/pmap.h defines these too, but pulls
 * in struct pmap_statistics which isn't visible without extra SDK headers —
 * just the 4 constants we need). */
#define PG_FRAME    (0x000ffffffffff000ul)
#define X86_PG_V    0x001ul
#define X86_PG_PS   0x080ul

static unsigned int
mdbg_fw_version(void) {
  static unsigned int cached = 0;
  if (cached == 0) {
    cached = kernel_get_fw_version() >> 16;
  }
  return cached;
}


int
mdbg_io_copyout(pid_t pid, intptr_t addr, void *buf, size_t len) {
  pt_ucred_lock();
  int rc = mdbg_copyout(pid, addr, buf, len);
  pt_ucred_unlock();
  return rc;
}


/* mdbg_copyin breaks on FW 8.20+ — walk CR3 to the physical page and write
 * through the kernel direct map instead (ported from ps-patch-system). */

static unsigned long
vmspace_pmap_offset(unsigned int fw) {
  if (fw >= 0x100 && fw <= 0x102) return 0x2C0;
  if (fw >= 0x105 && fw <= 0x550) return 0x2E0;
  if (fw >= 0x600 && fw <= 0x1200) return 0x2E8;
  return 0; /* unsupported fw version */
}

static unsigned long g_dmap_base = 0;

static unsigned long
mdbg_get_proc_cr3(pid_t pid) {
  intptr_t proc = kernel_get_proc(pid);
  if (!proc) return 0;

  unsigned long vmspace = (unsigned long)kernel_getlong(proc + KERNEL_OFFSET_PROC_P_VMSPACE);
  if (!vmspace) return 0;

  unsigned long off = vmspace_pmap_offset(mdbg_fw_version());
  if (!off) return 0;

  /* pm_pml4 (KVA) + pm_cr3 (phys) sit next to each other; read both in one
   * shot — free to read the extra 8 bytes and it lets us populate the dmap
   * base cache for virt2phys below. */
  unsigned long data[2];
  if (kernel_copyout((intptr_t)(vmspace + off + 32), data, sizeof(data))) return 0;

  g_dmap_base = data[0] - data[1];
  return data[1];
}

static unsigned long
mdbg_virt2phys(unsigned long cr3, unsigned long va, unsigned long *phys_limit) {
  unsigned long dmap_base = g_dmap_base;
  cr3 &= PG_FRAME;

  /* PML4 (39) -> PDP (30) -> PD (21) -> PT (12) */
  for (int shift = 39; shift >= 12; shift -= 9) {
    unsigned long index = (va >> shift) & 0x1FFul;
    unsigned long entry_off = index * sizeof(unsigned long);
    cr3 = (unsigned long)kernel_getlong((intptr_t)(dmap_base + cr3 + entry_off));

    if (!(cr3 & X86_PG_V)) return (unsigned long)-1; /* not present */
    if ((cr3 & X86_PG_PS) || shift == 12) {
      cr3 &= (1ul << 52) - (1ul << shift);
      cr3 |= va & ((1ul << shift) - 1);
      if (phys_limit) *phys_limit = (cr3 | ((1ul << shift) - 1)) + 1;
      return cr3;
    }
    cr3 &= PG_FRAME;
  }
  return (unsigned long)-1;
}

static int
mdbg_phys_write(unsigned long cr3, const void *src, unsigned long remote_va, unsigned long len) {
  const unsigned char *p = src;
  if (g_dmap_base == 0) return -1;

  while (len) {
    unsigned long phys_end = 0;
    unsigned long phys = mdbg_virt2phys(cr3, remote_va, &phys_end);
    if (phys == (unsigned long)-1) return -1;

    size_t chunk = (size_t)(phys_end - phys);
    if (chunk > len) chunk = len;

    if (kernel_copyin(p, (intptr_t)(g_dmap_base + phys), chunk)) return -1;

    remote_va += chunk;
    p         += chunk;
    len       -= chunk;
  }
  return 0;
}


int
mdbg_io_copyin(pid_t pid, const void *buf, intptr_t addr, size_t len) {
  pt_ucred_lock();

  if (mdbg_fw_version() <= 0x820) {
    int rc = mdbg_copyin(pid, buf, addr, len);
    pt_ucred_unlock();
    return rc;
  }

  unsigned long cr3 = mdbg_get_proc_cr3(pid);
  if (!cr3) {
    cr_log("warn", "mdbg", "cr3_resolve_failed pid=%d fw=0x%x — write blocked", (int)pid, mdbg_fw_version());
    pt_ucred_unlock();
    return -1;
  }

  int rc = mdbg_phys_write(cr3, buf, (unsigned long)addr, len);
  pt_ucred_unlock();
  return rc;
}
