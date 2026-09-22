/*
 * uaccesstest.c - The exception fixup path (docs/kernel/memory/design.md §6.1).
 *
 * Runs on a kernel thread with no process: every user address is
 * unmapped from its point of view, so each copy takes a real kernel-mode
 * page fault at a user address that the fault handler must resolve
 * through the exception table into -EFAULT. The user-mode side (a
 * process whose pointers name PROT_NONE, read-only and unmapped pages)
 * is process-efault.
 */

#include <kernel/errno.h>
#include <kernel/extable.h>
#include <kernel/kernel.h>
#include <kernel/log.h>
#include <kernel/process.h>
#include <kernel/selftest.h>
#include <kernel/string.h>
#include <kernel/uaccess.h>
#include <kernel/page.h>
#include <kernel/percpu.h>
#include <arch/mmu.h>
#include <kernel/vmm.h>
#include <arch/user.h>

#define STR_(x) #x
#define STR(x)  STR_(x)
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            *reason = "check failed: " #cond " at line " STR(__LINE__);        \
            return false;                                                      \
        }                                                                      \
    } while (0)

bool selftest_uaccess(const char **reason)
{
    CHECK(process_current() == NULL);

    /* The table exists and every entry names kernel text. */
    unsigned n = extable_count();
    CHECK(n >= 1);
    for (unsigned i = 0; i < n; i++) {
        uintptr_t insn, fixup;
        CHECK(extable_entry(i, &insn, &fixup));
        CHECK(kernel_text_contains(insn) && kernel_text_contains(fixup));
        CHECK(extable_fixup(insn) == fixup);
    }
    CHECK(extable_fixup((uintptr_t)selftest_uaccess) == 0);

    /* Range checks stay in front of the copy. */
    CHECK(user_range_ok(USER_LO, 1));
    CHECK(user_range_ok(USER_HI, 0));
    CHECK(!user_range_ok(USER_HI, 1));
    CHECK(!user_range_ok(USER_LO - 1, 1));
    CHECK(!user_range_ok(USER_HI - 8, 16));
    CHECK(!user_range_ok(0xffffffff80000000ULL, 8));
    CHECK(!user_range_ok(USER_LO, (size_t)-1));

    struct vm_stats s0, s1;
    vm_get_stats(&s0);
    char buf[64];
    memset(buf, 'x', sizeof(buf));

    /* Real faults, resolved by the fixup: -EFAULT, no panic, the kernel
     * buffer untouched where the copy could not start. */
    CHECK(copy_from_user(buf, USER_LO, 16) == -EFAULT);
    CHECK(buf[0] == 'x');
    CHECK(copy_to_user(USER_LO + 4096, buf, 16) == -EFAULT);
    CHECK(copy_from_user(buf, USER_HI - 4096, 4096) == -EFAULT);
    CHECK(strncpy_from_user(buf, USER_LO + 100, sizeof(buf)) == -EFAULT);
    CHECK(buf[0] == '\0');
    CHECK(copy_from_user(buf, USER_LO, 0) == 0);   /* nothing to copy: no access */
    CHECK(copy_from_user(buf, 0xffffffff80000000ULL, 8) == -EFAULT);   /* refused by range, no fault */
    CHECK(strncpy_from_user(buf, USER_LO, 0) == -EINVAL);

    vm_get_stats(&s1);
    CHECK(s1.fixups == s0.fixups + 4);
    kinfo("selftest: uaccess: %u fixup entries; 4 kernel-mode faults at user addresses resumed as -EFAULT", n);
    return true;
}

/*
 * The guard on kernel access to user memory, where the CPU has one
 * (SMAP on x86-64, PAN on AArch64; docs/kernel/security/design.md,
 * "Hardening"). A private space maps one user page; with it active and
 * interrupts off, one byte is read through the raw copy twice: inside
 * arch_user_access_begin/end, which must succeed, and outside it, whose
 * outcome IS the guard -- a fault (the fixup reports the byte not
 * copied) where the guard is live, the byte where the CPU has no guard.
 * The test says which, and the guard boot's harness requires "live".
 * On the default CI models the fault cannot be asserted; the mapping
 * is, and the bracketed read's success is also the proof the space was
 * active (the kernel's own tables have nothing at that address).
 */
bool selftest_uaccess_guard(const char **reason)
{
    CHECK(process_current() == NULL);
    const bool has_guard = arch_user_guard_present();
#if defined(ARCH_X86_64)
    const char *missing = "no smap";
#else
    const char *missing = "no pan";
#endif
    const uint64_t VA = 0x0000300000000000ULL;   /* far from anything a process maps */
    struct vm_space *sp = NULL;
    CHECK(vm_space_create_user(&sp) == 0);
    CHECK(vm_user_map_anon(sp, VA, PAGE_SIZE, VM_PROT_RW, VM_REGION_POPULATED, "guard") == 0);
    paddr_t pa;
    CHECK(arch_mmu_query(&sp->mmu, VA, &pa, NULL, NULL, NULL));
    memset(phys_to_virt(pa), 0x5A, PAGE_SIZE);

    uint8_t in = 0, out = 0;
    arch_irq_state_t s = arch_irq_save();
    struct vm_space *restore = this_cpu()->cur_space ? this_cpu()->cur_space : &kernel_space;   /* read where the switch is made (S25) */
    vm_space_switch(restore, sp);
    arch_user_access_begin();
    size_t left_in = arch_copy_user_raw(&in, (const void *)(uintptr_t)VA, 1);
    arch_user_access_end();
    size_t left_out = arch_copy_user_raw(&out, (const void *)(uintptr_t)VA, 1);
    vm_space_switch(sp, restore);
    arch_irq_restore(s);
    vm_space_destroy(sp);

    CHECK(left_in == 0 && in == 0x5A);   /* the bracket opens, the space was active */
    if (has_guard) {
        CHECK(left_out == 1 && out == 0);   /* the guard denied the unbracketed read */
        kinfo("selftest: uaccess-guard: guard live: an unbracketed kernel read of a user page faulted, the bracketed one did not");
    } else {
        CHECK(left_out == 0 && out == 0x5A);   /* no guard: the page is simply readable */
        kinfo("selftest: uaccess-guard: guard absent (%s): the unbracketed read succeeded; nothing to assert but the mapping", missing);
    }
    return true;
}
