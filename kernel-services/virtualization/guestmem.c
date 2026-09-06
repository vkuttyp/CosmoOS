/*
 * guestmem.c - GuestMemory: regions of pinned, zeroed host pages mapped
 * guest-physical (docs/kernel-services/virtualization/design.md,
 * "Guest memory").
 */

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/page.h>
#include <kernel/pmm.h>
#include <kernel/string.h>

#include "hv_internal.h"

static struct guest_region *find_region(struct vm *vm, uint64_t gpa)
{
    struct list_node *n;
    for (n = vm->regions.next; n != &vm->regions; n = n->next) {
        struct guest_region *r = container_of(n, struct guest_region, link);
        if (gpa >= r->gpa && gpa < r->gpa + r->len)
            return r;
    }
    return NULL;
}

static bool overlaps(struct vm *vm, uint64_t gpa, uint64_t len)
{
    struct list_node *n;
    for (n = vm->regions.next; n != &vm->regions; n = n->next) {
        struct guest_region *r = container_of(n, struct guest_region, link);
        if (gpa < r->gpa + r->len && r->gpa < gpa + len)
            return true;
    }
    return false;
}

static void region_free(struct vm *vm, struct guest_region *r, size_t mapped_pages)
{
    size_t pages = r->len / PAGE_SIZE;
    if (mapped_pages)
        arch_hv_vm_unmap(vm->arch, r->gpa, mapped_pages * PAGE_SIZE);
    for (size_t i = 0; i < pages; i++)
        if (r->pages[i])
            pmm_free_page(r->pages[i]);
    kfree(r->pages);
    kfree(r);
}

int vm_mem_add(struct vm *vm, uint64_t gpa, uint64_t len)
{
    if (len == 0 || (gpa | len) & (PAGE_SIZE - 1))
        return -EINVAL;
    if (gpa >= HV_GPA_LIMIT || len > HV_GPA_LIMIT - gpa)
        return -EINVAL;
    size_t pages = (size_t)(len / PAGE_SIZE);
    struct guest_region *r = kzalloc(sizeof(*r));
    if (r == NULL)
        return -ENOMEM;
    r->pages = kzalloc(pages * sizeof(struct page *));
    if (r->pages == NULL) {
        kfree(r);
        return -ENOMEM;
    }
    r->gpa = gpa;
    r->len = len;
    list_init(&r->link);

    mutex_lock(&vm->lock);
    int rc = 0;
    if (vm->nr_regions >= HV_REGIONS_MAX)
        rc = -ENOSPC;
    else if (vm->mem_bytes + len > vm->mem_limit)   /* the creator's COSMO_RLIMIT_VMEM */
        rc = -ENOMEM;
    else if (overlaps(vm, gpa, len))
        rc = -EINVAL;
    if (rc) {
        mutex_unlock(&vm->lock);
        region_free(vm, r, 0);
        return rc;
    }
    size_t mapped = 0;
    for (size_t i = 0; i < pages; i++) {
        r->pages[i] = pmm_alloc_page(PMM_FLAGS_ZERO);
        if (r->pages[i] == NULL) {
            rc = -ENOMEM;
            break;
        }
        rc = arch_hv_vm_map(vm->arch, gpa + i * PAGE_SIZE, page_to_phys(r->pages[i]), PAGE_SIZE, HV_MAP_RWX);
        if (rc)
            break;
        mapped++;
    }
    if (rc) {
        mutex_unlock(&vm->lock);
        region_free(vm, r, mapped);
        return rc;
    }
    /* Keep the list sorted by gpa. */
    struct list_node *pos = vm->regions.next;
    while (pos != &vm->regions && container_of(pos, struct guest_region, link)->gpa < gpa)
        pos = pos->next;
    list_insert_before(pos, &r->link);
    vm->nr_regions++;
    vm->mem_bytes += len;
    mutex_unlock(&vm->lock);
    return 0;
}

bool vm_mem_lookup(struct vm *vm, uint64_t gpa, struct page **page, size_t *offset)
{
    struct guest_region *r = find_region(vm, gpa);
    if (r == NULL)
        return false;
    if (page)
        *page = r->pages[(gpa - r->gpa) / PAGE_SIZE];
    if (offset)
        *offset = (size_t)(gpa & (PAGE_SIZE - 1));
    return true;
}

/* Regions only grow while a VM lives, so the lookups run under the VM
 * lock for the copies (owner side) and unlocked from the run loop. */
static int copy(struct vm *vm, uint64_t gpa, uint8_t *rd, const uint8_t *wr, size_t len)
{
    if (len == 0)
        return 0;
    if (gpa >= HV_GPA_LIMIT || len > HV_GPA_LIMIT - gpa)
        return -EFAULT;
    mutex_lock(&vm->lock);
    /* The whole range must be backed before any byte moves. */
    for (uint64_t p = gpa & ~(uint64_t)(PAGE_SIZE - 1); p < gpa + len; p += PAGE_SIZE) {
        if (find_region(vm, p) == NULL) {
            mutex_unlock(&vm->lock);
            return -EFAULT;
        }
    }
    size_t done = 0;
    while (done < len) {
        struct page *pg;
        size_t off;
        vm_mem_lookup(vm, gpa + done, &pg, &off);
        size_t n = PAGE_SIZE - off;
        if (n > len - done)
            n = len - done;
        uint8_t *host = (uint8_t *)phys_to_virt(page_to_phys(pg)) + off;
        if (wr)
            memcpy(host, wr + done, n);
        else
            memcpy(rd + done, host, n);
        done += n;
    }
    mutex_unlock(&vm->lock);
    return 0;
}

int vm_mem_read(struct vm *vm, uint64_t gpa, void *buf, size_t len)
{
    return copy(vm, gpa, buf, NULL, len);
}

int vm_mem_write(struct vm *vm, uint64_t gpa, const void *buf, size_t len)
{
    return copy(vm, gpa, NULL, buf, len);
}

void guestmem_release(struct vm *vm)
{
    while (!list_empty(&vm->regions)) {
        struct guest_region *r = container_of(list_pop_front(&vm->regions), struct guest_region, link);
        region_free(vm, r, r->len / PAGE_SIZE);
    }
    vm->nr_regions = 0;
    vm->mem_bytes = 0;
}
