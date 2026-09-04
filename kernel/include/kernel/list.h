/*
 * list.h - Intrusive circular doubly linked list.
 *
 * A `struct list_node` embedded in an object links it into a list headed
 * by another `struct list_node`. No allocation, no locking: the caller
 * owns synchronization. An initialised empty head points at itself, so
 * every operation is branch-free and a node removed from a list is
 * re-initialised to empty.
 */

#ifndef KERNEL_LIST_H
#define KERNEL_LIST_H

#include <kernel/compiler.h>

struct list_node {
    struct list_node *next;
    struct list_node *prev;
};

#define LIST_HEAD_INIT(name) { &(name), &(name) }
#define LIST_HEAD(name) struct list_node name = LIST_HEAD_INIT(name)

static inline void list_init(struct list_node *head)
{
    head->next = head;
    head->prev = head;
}

static inline bool list_empty(const struct list_node *head)
{
    return head->next == head;
}

static inline void list_insert_between(struct list_node *node, struct list_node *prev,
                                       struct list_node *next)
{
    node->next = next;
    node->prev = prev;
    prev->next = node;
    next->prev = node;
}

/* Insert `node` right after `at`. */
static inline void list_insert_after(struct list_node *at, struct list_node *node)
{
    list_insert_between(node, at, at->next);
}

/* Insert `node` right before `at`. */
static inline void list_insert_before(struct list_node *at, struct list_node *node)
{
    list_insert_between(node, at->prev, at);
}

static inline void list_push_front(struct list_node *head, struct list_node *node)
{
    list_insert_after(head, node);
}

static inline void list_push_back(struct list_node *head, struct list_node *node)
{
    list_insert_before(head, node);
}

static inline void list_remove(struct list_node *node)
{
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->next = node;
    node->prev = node;
}

/* Remove and return the first node, or NULL if empty. */
static inline struct list_node *list_pop_front(struct list_node *head)
{
    if (list_empty(head))
        return NULL;
    struct list_node *n = head->next;
    list_remove(n);
    return n;
}

#define list_entry(ptr, type, member) container_of(ptr, type, member)

#define list_first_entry(head, type, member) \
    list_entry((head)->next, type, member)

#define list_for_each(pos, head) \
    for ((pos) = (head)->next; (pos) != (head); (pos) = (pos)->next)

/* Safe against removal of the current node. */
#define list_for_each_safe(pos, tmp, head) \
    for ((pos) = (head)->next, (tmp) = (pos)->next; (pos) != (head); \
         (pos) = (tmp), (tmp) = (pos)->next)

#define list_for_each_entry(pos, head, member)                                 \
    for ((pos) = list_entry((head)->next, __typeof__(*(pos)), member);         \
         &(pos)->member != (head);                                             \
         (pos) = list_entry((pos)->member.next, __typeof__(*(pos)), member))

#define list_for_each_entry_safe(pos, tmp, head, member)                       \
    for ((pos) = list_entry((head)->next, __typeof__(*(pos)), member),         \
         (tmp) = list_entry((pos)->member.next, __typeof__(*(pos)), member);   \
         &(pos)->member != (head);                                             \
         (pos) = (tmp), (tmp) = list_entry((tmp)->member.next, __typeof__(*(tmp)), member))

#endif /* KERNEL_LIST_H */
