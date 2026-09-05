/*
 * vmm.c - The VM manager: backend probe, /dev/vmm, limits, the VM list
 * (docs/kernel-services/virtualization/design.md, "The VM manager").
 */

#include <kernel/errno.h>
#include <kernel/hv.h>
#include <kernel/log.h>
#include <kernel/printf.h>
#include <kernel/string.h>
#include <kernel/vfs.h>

#include "hv_internal.h"

static struct hv_caps g_caps = { .present = false, .name = "none" };
static struct mutex g_lock;
static struct list_node g_vms;
static unsigned g_nr_vms;
static unsigned g_next_id;
static struct vnode *g_vmm_vnode;

/* /dev/vmm: one line describing the backend. */
static int64_t vmm_chr_read(struct vnode *vn, uint64_t off, void *buf, size_t len)
{
    (void)vn;
    char line[96];
    int n = ksnprintf(line, sizeof(line), "%s%s asids=%u vms=%u\n", g_caps.name,
                      g_caps.nested_paging ? " npt" : "", g_caps.max_asids, hv_vm_count());
    if (n < 0)
        return -EIO;
    if (off >= (uint64_t)n)
        return 0;
    size_t avail = (size_t)n - (size_t)off;
    if (len > avail)
        len = avail;
    memcpy(buf, line + off, len);
    return (int64_t)len;
}

static const struct chrdev_ops vmm_chr_ops = {
    .read = vmm_chr_read,
    .write = NULL,
};

void hv_init(void)
{
    mutex_init(&g_lock, "hv");
    list_init(&g_vms);
    int rc = arch_hv_probe(&g_caps);
    if (rc == -ENOTSUP)
        kinfo("hv: no hardware virtualization backend on this CPU");
    else if (rc)
        kwarn("hv: backend probe failed (%d)", rc);
    else
        kinfo("hv: backend %s, nested paging %s, %u ASIDs", g_caps.name, g_caps.nested_paging ? "yes" : "no",
              g_caps.max_asids);
    rc = ramfs_mkchr("/dev/vmm", 0600, &vmm_chr_ops, NULL, &g_vmm_vnode);
    if (rc)
        kwarn("hv: cannot create /dev/vmm (%d)", rc);
}

const struct hv_caps *hv_caps(void)
{
    return &g_caps;
}

bool hv_is_vmm_vnode(const struct vnode *vn)
{
    return vn != NULL && vn == g_vmm_vnode;
}

unsigned hv_vm_count(void)
{
    return __atomic_load_n(&g_nr_vms, __ATOMIC_RELAXED);
}

/* Called by vm.c under no locks. */
int hv_register_vm(struct vm *vm)
{
    mutex_lock(&g_lock);
    if (g_nr_vms >= HV_VMS_MAX) {
        mutex_unlock(&g_lock);
        return -ENOSPC;
    }
    vm->id = g_next_id++;
    list_push_back(&g_vms, &vm->link);
    g_nr_vms++;
    mutex_unlock(&g_lock);
    return 0;
}

void hv_unregister_vm(struct vm *vm)
{
    mutex_lock(&g_lock);
    list_remove(&vm->link);
    g_nr_vms--;
    mutex_unlock(&g_lock);
}

void hv_stats(uint64_t *exits, uint64_t *entries, unsigned *vcpus)
{
    uint64_t x = 0, e = 0;
    unsigned c = 0;
    mutex_lock(&g_lock);
    struct list_node *n;
    for (n = g_vms.next; n != &g_vms; n = n->next) {
        struct vm *vm = container_of(n, struct vm, link);
        mutex_lock(&vm->lock);
        for (unsigned i = 0; i < HV_VCPUS_MAX; i++) {
            struct vcpu *v = vm->vcpus[i];
            if (v) {
                c++;
                x += __atomic_load_n(&v->exits, __ATOMIC_RELAXED);
                e += __atomic_load_n(&v->entries, __ATOMIC_RELAXED);
            }
        }
        mutex_unlock(&vm->lock);
    }
    mutex_unlock(&g_lock);
    if (exits)
        *exits = x;
    if (entries)
        *entries = e;
    if (vcpus)
        *vcpus = c;
}

int hv_sysctl(const char *name, char *out, size_t n)
{
    if (strcmp(name, "backend") == 0)
        return ksnprintf(out, n, "%s", g_caps.name);
    if (strcmp(name, "vms") == 0)
        return ksnprintf(out, n, "%u", hv_vm_count());
    if (strcmp(name, "vcpus") == 0 || strcmp(name, "exits") == 0) {
        uint64_t exits;
        unsigned vcpus;
        hv_stats(&exits, NULL, &vcpus);
        if (name[0] == 'v')
            return ksnprintf(out, n, "%u", vcpus);
        return ksnprintf(out, n, "%llu", (unsigned long long)exits);
    }
    return -ENOENT;
}
