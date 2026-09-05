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
#include <kernel/vmm.h>

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
