#include "buddy.h"
#define NULL ((void *)0)

#define MAX_RANK 16
#define PAGE_SIZE (4 * 1024)
#define MAX_PAGES (128 * 1024 / 4) // Maximum possible pages in 128MB test

// Free list node structure
typedef struct free_node {
    struct free_node *next;
    struct free_node *prev;
} free_node_t;

// Global variables
static void *base_addr = NULL;
static int total_pages = 0;
static free_node_t free_lists[MAX_RANK + 1]; // free_lists[1] to free_lists[16]
static int free_counts[MAX_RANK + 1]; // Count of free blocks for each rank
static unsigned char page_rank_map[MAX_PAGES]; // Store the rank of each page

// Convert physical address to page index
static inline int addr_to_page_idx(void *addr) {
    if (addr < base_addr) return -1;
    long long offset = (char *)addr - (char *)base_addr;
    if (offset < 0 || offset % PAGE_SIZE != 0) return -1;
    int idx = offset / PAGE_SIZE;
    if (idx >= total_pages) return -1;
    return idx;
}

// Convert page index to physical address
static inline void *page_idx_to_addr(int idx) {
    return (char *)base_addr + (long long)idx * PAGE_SIZE;
}

// Get buddy page index
static inline int get_buddy_idx(int idx, int rank) {
    int block_size = 1 << (rank - 1); // 2^(rank-1) pages
    return idx ^ block_size;
}

// Check if a page index is aligned for given rank
static inline int is_aligned(int idx, int rank) {
    int block_size = 1 << (rank - 1);
    return (idx % block_size) == 0;
}

// Initialize doubly linked list
static inline void init_list(free_node_t *head) {
    head->next = head;
    head->prev = head;
}

// Check if list is empty
static inline int is_list_empty(free_node_t *head) {
    return head->next == head;
}

// Add node to list
static inline void list_add(free_node_t *head, free_node_t *node, int rank) {
    node->next = head->next;
    node->prev = head;
    head->next->prev = node;
    head->next = node;
    free_counts[rank]++;
}

// Remove node from list
static inline void list_remove(free_node_t *node, int rank) {
    node->prev->next = node->next;
    node->next->prev = node->prev;
    free_counts[rank]--;
}

int init_page(void *p, int pgcount) {
    if (p == NULL || pgcount <= 0) return -EINVAL;

    base_addr = p;
    total_pages = pgcount;

    // Initialize all page ranks to 0 (unallocated)
    for (int i = 0; i < total_pages; i++) {
        page_rank_map[i] = 0;
    }

    // Initialize free lists and counters
    for (int i = 1; i <= MAX_RANK; i++) {
        init_list(&free_lists[i]);
        free_counts[i] = 0;
    }

    // Add free blocks to appropriate free lists
    // Start from the beginning and add largest possible blocks
    int current_page = 0;
    while (current_page < total_pages) {
        int remaining = total_pages - current_page;

        // Find the largest rank that fits
        int rank = MAX_RANK;
        while (rank >= 1) {
            int block_size = 1 << (rank - 1);
            if (block_size <= remaining && is_aligned(current_page, rank)) {
                // Add this block to free list
                void *block_addr = page_idx_to_addr(current_page);
                free_node_t *node = (free_node_t *)block_addr;
                list_add(&free_lists[rank], node, rank);

                // Mark first page with rank (free)
                page_rank_map[current_page] = rank;

                current_page += block_size;
                break;
            }
            rank--;
        }

        if (rank < 1) {
            // Can't fit any more blocks
            break;
        }
    }

    return OK;
}

void *alloc_pages(int rank) {
    if (rank < 1 || rank > MAX_RANK) {
        return ERR_PTR(-EINVAL);
    }

    // Find a free block of at least the requested rank
    int current_rank = rank;
    while (current_rank <= MAX_RANK && is_list_empty(&free_lists[current_rank])) {
        current_rank++;
    }

    if (current_rank > MAX_RANK) {
        // No free block available
        return ERR_PTR(-ENOSPC);
    }

    // Get a block from the free list
    free_node_t *node = free_lists[current_rank].next;
    list_remove(node, current_rank);
    void *block_addr = (void *)node;
    int page_idx = addr_to_page_idx(block_addr);

    // Split the block down to the requested rank
    while (current_rank > rank) {
        current_rank--;
        int block_size = 1 << (current_rank - 1);
        int buddy_idx = page_idx + block_size;
        void *buddy_addr = page_idx_to_addr(buddy_idx);

        // Add the buddy to the free list
        free_node_t *buddy_node = (free_node_t *)buddy_addr;
        list_add(&free_lists[current_rank], buddy_node, current_rank);

        // Mark buddy as free with rank
        page_rank_map[buddy_idx] = current_rank;
    }

    // Mark the allocated page (set high bit)
    page_rank_map[page_idx] = rank | 0x80;

    return block_addr;
}

int return_pages(void *p) {
    if (p == NULL) return -EINVAL;

    int page_idx = addr_to_page_idx(p);
    if (page_idx < 0) return -EINVAL;

    // Check if the page is allocated
    if ((page_rank_map[page_idx] & 0x80) == 0) {
        return -EINVAL; // Not allocated
    }

    int rank = page_rank_map[page_idx] & 0x7F;
    if (rank < 1 || rank > MAX_RANK) return -EINVAL;

    // Check if this is the start of an allocated block
    int block_size = 1 << (rank - 1);
    if (!is_aligned(page_idx, rank)) {
        return -EINVAL;
    }

    // Mark page as free
    page_rank_map[page_idx] = rank;

    // Try to merge with buddy
    while (rank < MAX_RANK) {
        int buddy_idx = get_buddy_idx(page_idx, rank);

        // Check if buddy exists and is free with the same rank
        if (buddy_idx < 0 || buddy_idx >= total_pages) break;
        if ((page_rank_map[buddy_idx] & 0x80) != 0) break; // Buddy is allocated
        if ((page_rank_map[buddy_idx] & 0x7F) != rank) break; // Buddy has different rank

        // Remove buddy from free list
        void *buddy_addr = page_idx_to_addr(buddy_idx);
        free_node_t *buddy_node = (free_node_t *)buddy_addr;
        list_remove(buddy_node, rank);

        // Merge: the lower address becomes the merged block
        if (buddy_idx < page_idx) {
            page_idx = buddy_idx;
        }

        rank++;
        // Update rank for merged block
        page_rank_map[page_idx] = rank;
    }

    // Add the block to the free list
    free_node_t *node = (free_node_t *)page_idx_to_addr(page_idx);
    list_add(&free_lists[rank], node, rank);

    return OK;
}

int query_ranks(void *p) {
    if (p == NULL) return -EINVAL;

    int page_idx = addr_to_page_idx(p);
    if (page_idx < 0) return -EINVAL;

    return page_rank_map[page_idx] & 0x7F;
}

int query_page_counts(int rank) {
    if (rank < 1 || rank > MAX_RANK) return -EINVAL;

    return free_counts[rank];
}
