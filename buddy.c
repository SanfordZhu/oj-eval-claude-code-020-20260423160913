#include "buddy.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define NULL ((void *)0)

#define MAX_RANK 16
#define PAGE_SIZE 4096
#define MAX_PAGES 32768

// Free lists for each rank (1-indexed)
static void *free_lists[MAX_RANK + 1];

// Base address and total pages
static void *base_addr = NULL;
static int total_pages = 0;
static int max_rank = 0;

// Allocation rank for each page (0 = unallocated, 1-16 = allocated at that rank)
static unsigned char alloc_rank[MAX_PAGES];

// Get the size of a block with given rank
static size_t get_size(int rank) {
    return (size_t)PAGE_SIZE << (rank - 1);
}

// Get the number of pages in a block of given rank
static int get_page_count(int rank) {
    return 1 << (rank - 1);
}

// Get the buddy of a block
static void *get_buddy(void *p, int rank) {
    uintptr_t addr = (uintptr_t)p;
    uintptr_t base = (uintptr_t)base_addr;
    uintptr_t offset = addr - base;
    uintptr_t size = get_size(rank);
    return (void *)(base + (offset ^ size));
}

// Get the page index of an address
static int get_page_index(void *p) {
    uintptr_t addr = (uintptr_t)p;
    uintptr_t base = (uintptr_t)base_addr;
    return (int)((addr - base) / PAGE_SIZE);
}

// Check if a page is allocated
static int is_page_allocated(int page_idx) {
    if (page_idx < 0 || page_idx >= total_pages) {
        return 0;
    }
    return alloc_rank[page_idx] != 0;
}

// Get the allocation rank of a page
static int get_page_rank(int page_idx) {
    if (page_idx < 0 || page_idx >= total_pages) {
        return 0;
    }
    return alloc_rank[page_idx];
}

// Set the allocation rank of a page
static void set_page_rank(int page_idx, int rank) {
    if (page_idx < 0 || page_idx >= total_pages) {
        return;
    }
    alloc_rank[page_idx] = rank;
}

// Mark all pages in a block as allocated with given rank
static void mark_pages_allocated(void *p, int rank) {
    int page_idx = get_page_index(p);
    int count = get_page_count(rank);
    for (int i = 0; i < count; i++) {
        set_page_rank(page_idx + i, rank);
    }
}

// Mark all pages in a block as unallocated
static void mark_pages_unallocated(void *p, int rank) {
    int page_idx = get_page_index(p);
    int count = get_page_count(rank);
    for (int i = 0; i < count; i++) {
        set_page_rank(page_idx + i, 0);
    }
}

// Check if all pages in a block are unallocated
static int are_all_pages_unallocated(void *p, int rank) {
    int page_idx = get_page_index(p);
    int count = get_page_count(rank);
    for (int i = 0; i < count; i++) {
        if (is_page_allocated(page_idx + i)) {
            return 0;
        }
    }
    return 1;
}

// Remove a block from the free list
static void remove_from_list(void *p, int rank) {
    void **prev = &free_lists[rank];
    while (*prev != NULL && *prev != p) {
        prev = (void **)*prev;
    }
    if (*prev == p) {
        *prev = *(void **)p;
    }
}

// Check if a block is in the free list
static int is_in_free_list(void *p, int rank) {
    void *curr = free_lists[rank];
    while (curr != NULL) {
        if (curr == p) {
            return 1;
        }
        curr = *(void **)curr;
    }
    return 0;
}

// Insert a block into the free list
static void insert_into_list(void *p, int rank) {
    *(void **)p = free_lists[rank];
    free_lists[rank] = p;
}

int init_page(void *p, int pgcount) {
    base_addr = p;
    total_pages = pgcount;

    // Calculate max rank needed
    int n = pgcount;
    max_rank = 1;
    while (n > 1) {
        n >>= 1;
        max_rank++;
    }

    // Initialize all free lists to NULL
    for (int i = 1; i <= MAX_RANK; i++) {
        free_lists[i] = NULL;
    }

    // Initialize allocation ranks (all unallocated)
    memset(alloc_rank, 0, sizeof(alloc_rank));

    // Insert the entire memory pool into the appropriate free list
    insert_into_list(p, max_rank);

    return OK;
}

void *alloc_pages(int rank) {
    if (rank < 1 || rank > MAX_RANK) {
        return ERR_PTR(-EINVAL);
    }

    // Find the smallest available block that can satisfy the request
    int curr_rank = rank;
    while (curr_rank <= max_rank && free_lists[curr_rank] == NULL) {
        curr_rank++;
    }

    if (curr_rank > max_rank) {
        return ERR_PTR(-ENOSPC);
    }

    // Remove the block from the free list
    void *block = free_lists[curr_rank];
    remove_from_list(block, curr_rank);

    // Split the block down to the requested rank
    while (curr_rank > rank) {
        curr_rank--;
        void *buddy = (void *)((char *)block + get_size(curr_rank));
        insert_into_list(buddy, curr_rank);
    }

    // Mark pages as allocated with the requested rank
    mark_pages_allocated(block, rank);

    return block;
}

void *return_pages(void *p) {
    if (p == NULL) {
        return ERR_PTR(-EINVAL);
    }

    uintptr_t addr = (uintptr_t)p;
    uintptr_t base = (uintptr_t)base_addr;

    if (addr < base || addr >= base + total_pages * PAGE_SIZE) {
        return ERR_PTR(-EINVAL);
    }

    int page_idx = get_page_index(p);

    // Check if page is allocated
    if (!is_page_allocated(page_idx)) {
        return ERR_PTR(-EINVAL);
    }

    // Get the rank at which this page was allocated
    int rank = get_page_rank(page_idx);

    // Mark pages as unallocated
    mark_pages_unallocated(p, rank);

    // Merge with buddy if possible
    void *block = p;
    while (rank < max_rank) {
        void *buddy = get_buddy(block, rank);

        // Check if buddy is in free list (unallocated)
        if (!is_in_free_list(buddy, rank)) {
            break;
        }

        // Remove buddy from free list
        remove_from_list(buddy, rank);

        // Merge - keep the lower address
        if ((uintptr_t)buddy < (uintptr_t)block) {
            block = buddy;
        }
        rank++;
    }

    insert_into_list(block, rank);
    return ERR_PTR(OK);
}

int query_ranks(void *p) {
    if (p == NULL) {
        return -EINVAL;
    }

    uintptr_t addr = (uintptr_t)p;
    uintptr_t base = (uintptr_t)base_addr;

    if (addr < base || addr >= base + total_pages * PAGE_SIZE) {
        return -EINVAL;
    }

    int page_idx = get_page_index(p);

    // If page is allocated, return the rank it was allocated at
    if (is_page_allocated(page_idx)) {
        return get_page_rank(page_idx);
    }

    // If unallocated, find the maximum rank where this address could be a valid block
    // and all pages in that block are unallocated
    for (int r = max_rank; r >= 1; r--) {
        uintptr_t size = get_size(r);
        uintptr_t offset = addr - base;
        if (offset % size == 0 && offset + size <= total_pages * PAGE_SIZE) {
            if (are_all_pages_unallocated(p, r)) {
                return r;
            }
        }
    }

    return -EINVAL;
}

int query_page_counts(int rank) {
    if (rank < 1 || rank > MAX_RANK) {
        return -EINVAL;
    }

    int count = 0;
    void *p = free_lists[rank];
    while (p != NULL) {
        count++;
        p = *(void **)p;
    }

    return count;
}
