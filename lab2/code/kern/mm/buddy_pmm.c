#include <buddy_pmm.h>
#include <list.h>
#include <stdio.h>

// LAB2 function2-3: standard physical buddies, with exact-size public requests.
// property must represent a whole block as well as an exact allocation length.
#define BUDDY_MAX_ORDER (sizeof(unsigned int) * 8 - 1)
#define BUDDY_MANAGED ((uint64_t)1 << PG_buddy_managed)
#define BUDDY_FREE ((uint64_t)1 << PG_buddy_free)

static list_entry_t free_lists[BUDDY_MAX_ORDER + 1];
static size_t nr_free, nr_managed;
static bool initialized;

static bool
buddy_managed(struct Page *p) {
    return !PageReserved(p) && (p->flags & BUDDY_MANAGED) != 0;
}

static bool
buddy_free(struct Page *p) {
    return buddy_managed(p) && (p->flags & BUDDY_FREE) != 0;
}

// Validate numeric addresses before subtracting Page pointers or dereferencing.
static size_t
buddy_range_index(struct Page *base, size_t n) {
    assert(initialized && pages != NULL && npage > nbase);
    size_t count = npage - nbase;
    assert(count <= (size_t)~(unsigned int)0);
    assert(npage - 1 <= (~(uintptr_t)0 >> PGSHIFT));
    assert(count <= (~(uintptr_t)0 - (uintptr_t)pages) / sizeof(struct Page));
    assert(base != NULL && n > 0 && (uintptr_t)base >= (uintptr_t)pages);
    uintptr_t delta = (uintptr_t)base - (uintptr_t)pages;
    assert(delta % sizeof(struct Page) == 0);
    size_t index = delta / sizeof(struct Page);
    assert(index < count && n <= count - index);
    return index;
}

static size_t
buddy_block_size(size_t order) {
    assert(order <= BUDDY_MAX_ORDER);
    return (size_t)1 << order;
}

static void
buddy_insert_block(struct Page *base, size_t order) {
    size_t size = buddy_block_size(order);
    assert(buddy_free(base) && !PageProperty(base));
    assert((page2ppn(base) & (size - 1)) == 0);
    base->property = size;
    SetPageProperty(base);
    list_add(&free_lists[order], &base->page_link);
}

static void
buddy_remove_block(struct Page *base) {
    assert(buddy_free(base) && PageProperty(base));
    list_del_init(&base->page_link);
    ClearPageProperty(base);
    base->property = 0;
}

// Do not call pa2page until both physical bounds have been established.
static struct Page *
buddy_partner(struct Page *base, size_t order) {
    size_t size = buddy_block_size(order);
    ppn_t ppn = page2ppn(base) ^ size;
    if (ppn < nbase || ppn >= npage || size > npage - ppn) {
        return NULL;
    }
    return pages + (ppn - nbase);
}

// All pages in the returned block are already marked free. No count changes.
static void
buddy_return_block(struct Page *base, size_t order) {
    while (order < BUDDY_MAX_ORDER) {
        struct Page *other = buddy_partner(base, order);
        if (other == NULL || !buddy_free(other) || !PageProperty(other) ||
            other->property != buddy_block_size(order)) {
            break;
        }
        buddy_remove_block(other);
        if (other < base) {
            base = other;
        }
        order++;
    }
    buddy_insert_block(base, order);
}

// Decompose any interval into physically aligned powers of two; keep all pages.
static void
buddy_return_range(struct Page *base, size_t n) {
    while (n != 0) {
        ppn_t ppn = page2ppn(base);
        size_t order = 0;
        while (order < BUDDY_MAX_ORDER) {
            size_t larger = buddy_block_size(order + 1);
            if (larger > n || (ppn & (larger - 1)) != 0) {
                break;
            }
            order++;
        }
        size_t size = buddy_block_size(order);
        buddy_return_block(base, order);
        base += size;
        n -= size;
    }
}

static void
buddy_init(void) {
    static_assert(BUDDY_MAX_ORDER < sizeof(size_t) * 8);
    static_assert(((size_t)1 << BUDDY_MAX_ORDER) >=
                  (PHYSICAL_MEMORY_END - DRAM_BASE) / PGSIZE);
    for (size_t order = 0; order <= BUDDY_MAX_ORDER; order++) {
        list_init(&free_lists[order]);
    }
    nr_free = nr_managed = 0;
    initialized = 1;
    // pages/npage are deliberately not inspected before page_init().
}

static void
buddy_init_memmap(struct Page *base, size_t n) {
    buddy_range_index(base, n);
    assert(nr_managed <= npage - nbase && n <= npage - nbase - nr_managed);
    assert(nr_free <= nr_managed);
    // Validate the complete registration before changing even its first page.
    for (size_t i = 0; i < n; i++) {
        assert(PageReserved(base + i));
    }
    for (size_t i = 0; i < n; i++) {
        struct Page *p = base + i;
        p->flags = BUDDY_MANAGED | BUDDY_FREE;
        p->property = 0;
        set_page_ref(p, 0);
        list_init(&p->page_link);
    }
    buddy_return_range(base, n);
    nr_managed += n;
    nr_free += n;
}

static struct Page *
buddy_alloc_pages(size_t n) {
    assert(initialized);
    if (n == 0 || n > nr_free) {
        return NULL;
    }
    size_t order = 0;
    while (buddy_block_size(order) < n) {
        if (order == BUDDY_MAX_ORDER) {
            return NULL;
        }
        order++;
    }
    size_t found = order;
    while (found <= BUDDY_MAX_ORDER && list_empty(&free_lists[found])) {
        found++;
    }
    if (found > BUDDY_MAX_ORDER) {
        return NULL;
    }
    // No state has changed on any failure path above.
    struct Page *base = le2page(list_next(&free_lists[found]), page_link);
    buddy_remove_block(base);
    while (found > order) {
        found--;
        buddy_insert_block(base + buddy_block_size(found), found);
    }
    for (size_t i = 0; i < n; i++) {
        struct Page *p = base + i;
        p->flags &= ~BUDDY_FREE;
        p->property = 0;
        set_page_ref(p, 0);
    }
    // PageProperty stays clear. Only the first allocated page records n.
    base->property = n;
    size_t tail = buddy_block_size(order) - n;
    if (tail != 0) {
        buddy_return_range(base + n, tail);
    }
    nr_free -= n;
    return base;
}

static void
buddy_free_pages(struct Page *base, size_t n) {
    buddy_range_index(base, n);
    assert(nr_free <= nr_managed && n <= nr_managed - nr_free);
    // The exact allocation record rejects partial, joined and repeated frees.
    assert(base->property == n);
    for (size_t i = 0; i < n; i++) {
        struct Page *p = base + i;
        assert(buddy_managed(p) && !buddy_free(p) && !PageProperty(p));
        assert(p->property == (i == 0 ? n : 0));
    }
    // All errors above are detected before touching any page or list.
    for (size_t i = 0; i < n; i++) {
        struct Page *p = base + i;
        p->flags |= BUDDY_FREE;
        p->property = 0;
        set_page_ref(p, 0);
        list_init(&p->page_link);
    }
    buddy_return_range(base, n);
    nr_free += n;
}

static size_t
buddy_nr_free_pages(void) {
    assert(initialized);
    return nr_free;
}

static bool
buddy_valid_link(list_entry_t *le, list_entry_t *head) {
    if (le == head) {
        return 1;
    }
    uintptr_t first = (uintptr_t)pages + offsetof(struct Page, page_link);
    uintptr_t address = (uintptr_t)le;
    return address >= first && (address - first) % sizeof(struct Page) == 0 &&
           (address - first) / sizeof(struct Page) < npage - nbase;
}

static void
buddy_check_invariants(void) {
    // Read-only checks of the live allocator; no test pool or saved-list swap.
    buddy_range_index(pages, 1);
    size_t total = 0, blocks = 0;
    for (size_t order = 0; order <= BUDDY_MAX_ORDER; order++) {
        list_entry_t *head = &free_lists[order], *le = head;
        size_t size = buddy_block_size(order);
        do {
            assert(buddy_valid_link(list_prev(le), head));
            assert(buddy_valid_link(list_next(le), head));
            assert(list_next(list_prev(le)) == le);
            assert(list_prev(list_next(le)) == le);
            le = list_next(le);
            if (le == head) {
                break;
            }
            assert(++blocks <= nr_managed);
            struct Page *base = le2page(le, page_link);
            buddy_range_index(base, size);
            assert(PageProperty(base) && base->property == size);
            assert((page2ppn(base) & (size - 1)) == 0);
            for (size_t i = 0; i < size; i++) {
                struct Page *p = base + i;
                assert(buddy_free(p));
                if (i != 0) {
                    assert(!PageProperty(p) && p->property == 0);
                }
            }
            // Aligned power-of-two intervals can overlap only by nesting.
            // The no-interior-head check above therefore proves disjointness.
            if (order < BUDDY_MAX_ORDER) {
                struct Page *other = buddy_partner(base, order);
                assert(other == NULL || !buddy_free(other) ||
                       !PageProperty(other) || other->property != size);
            }
            assert(total <= nr_managed && size <= nr_managed - total);
            total += size;
        } while (1);
    }
    size_t managed = 0, free_pages = 0, heads = 0;
    for (size_t i = 0; i < npage - nbase;) {
        struct Page *p = pages + i;
        if (PageReserved(p)) {
            i++;
            continue;
        }
        assert(buddy_managed(p));
        if (buddy_free(p)) {
            managed++;
            free_pages++;
            if (PageProperty(p)) {
                assert(p->property != 0);
                heads++;
            } else {
                assert(p->property == 0);
            }
            i++;
        } else {
            // Validate exact allocation records, including their interiors.
            size_t n = p->property;
            buddy_range_index(p, n);
            for (size_t j = 0; j < n; j++) {
                struct Page *q = p + j;
                assert(buddy_managed(q) && !buddy_free(q) && !PageProperty(q));
                assert(q->property == (j == 0 ? n : 0));
            }
            managed += n;
            i += n;
        }
    }
    assert(managed == nr_managed && heads == blocks);
    assert(total == nr_free && free_pages == nr_free);
}

// Independent fixed reference data: never allocate test records with this PMM.
#define BUDDY_TEST_MAX_PAGES ((PHYSICAL_MEMORY_END - DRAM_BASE) / PGSIZE)
#define BUDDY_TEST_SLOTS 9
struct buddy_test_record {
    struct Page *base;
    size_t n;
    unsigned int tag;
};
static struct buddy_test_record test_records[BUDDY_TEST_SLOTS];
static uint8_t test_original_heads[BUDDY_TEST_MAX_PAGES];

static uint8_t
buddy_test_pattern(unsigned int tag, size_t page, size_t byte) {
    return (uint8_t)(tag * 37 + page * 23 + byte * 13 + (byte >> 8));
}

// Called only for pages obtained through alloc_pages; no KADDR is assumed.
static uint8_t *
buddy_test_data(struct Page *page) {
    return (uint8_t *)(page2pa(page) + va_pa_offset);
}

static void
buddy_test_verify(size_t initial_free) {
    size_t held = 0;
    for (size_t slot = 0; slot < BUDDY_TEST_SLOTS; slot++) {
        struct buddy_test_record *r = &test_records[slot];
        if (r->base == NULL) {
            continue;
        }
        buddy_range_index(r->base, r->n);
        assert(r->base->property == r->n);
        uintptr_t pa = page2pa(r->base);
        assert(pa % PGSIZE == 0);
        for (size_t i = 0; i < r->n; i++) {
            assert(page2pa(r->base + i) == pa + i * PGSIZE);
            assert(buddy_managed(r->base + i) && !buddy_free(r->base + i));
            assert(!PageProperty(r->base + i));
            uint8_t *data = buddy_test_data(r->base + i);
            for (size_t j = 0; j < PGSIZE; j++) {
                assert(data[j] == buddy_test_pattern(r->tag, i, j));
            }
        }
        for (size_t other = slot + 1; other < BUDDY_TEST_SLOTS; other++) {
            struct buddy_test_record *q = &test_records[other];
            if (q->base != NULL) {
                assert(r->base + r->n <= q->base || q->base + q->n <= r->base);
            }
        }
        held += r->n;
    }
    assert(nr_free_pages() + held == initial_free);
    buddy_check_invariants();
}

static bool
buddy_test_take(size_t slot, size_t n, unsigned int tag, size_t initial_free) {
    struct buddy_test_record *r = &test_records[slot];
    assert(r->base == NULL);
    size_t before = nr_free_pages();
    struct Page *base = alloc_pages(n);
    if (base == NULL) {
        assert(nr_free_pages() == before);
        buddy_test_verify(initial_free);
        return 0;
    }
    buddy_range_index(base, n);
    assert(base->property == n && before >= n && nr_free_pages() == before - n);
    // Check ownership and bounds before writing any returned physical memory.
    for (size_t i = 0; i < BUDDY_TEST_SLOTS; i++) {
        struct buddy_test_record *q = &test_records[i];
        if (q->base != NULL) {
            assert(base + n <= q->base || q->base + q->n <= base);
        }
    }
    for (size_t i = 0; i < n; i++) {
        assert(buddy_managed(base + i) && !buddy_free(base + i));
        assert(!PageProperty(base + i));
    }
    r->base = base;
    r->n = n;
    r->tag = tag;
    for (size_t i = 0; i < n; i++) {
        uint8_t *data = buddy_test_data(base + i);
        for (size_t j = 0; j < PGSIZE; j++) {
            data[j] = buddy_test_pattern(tag, i, j);
        }
    }
    buddy_test_verify(initial_free);
    return 1;
}

static void
buddy_test_give(size_t slot, size_t initial_free) {
    struct buddy_test_record *r = &test_records[slot];
    assert(r->base != NULL);
    buddy_test_verify(initial_free);
    size_t before = nr_free_pages(), n = r->n;
    free_pages(r->base, n);
    r->base = NULL;
    r->n = 0;
    r->tag = 0;
    assert(nr_free_pages() == before + n);
    buddy_test_verify(initial_free);
}

// Save/compare exact block membership and order, independent of list ordering.
// No list heads or live nodes are copied, detached or reused by the tests.
static size_t
buddy_test_block_map(bool save) {
    size_t count = 0;
    if (save) {
        for (size_t i = 0; i < npage - nbase; i++) {
            test_original_heads[i] = 0;
        }
    }
    for (size_t order = 0; order <= BUDDY_MAX_ORDER; order++) {
        list_entry_t *head = &free_lists[order], *le = head;
        while ((le = list_next(le)) != head) {
            struct Page *base = le2page(le, page_link);
            size_t index = base - pages;
            if (save) {
                assert(test_original_heads[index] == 0);
                test_original_heads[index] = order + 1;
            } else {
                assert(test_original_heads[index] == order + 1);
            }
            count++;
        }
    }
    return count;
}

static void
buddy_test_isolated_eight(size_t initial_free, size_t original_blocks) {
    // Obtain a real aligned arena, then legitimately hold the other free blocks.
    // Other callers stay allocated. No globals, list heads or nodes are swapped.
    struct Page *arena = alloc_pages(8);
    assert(arena != NULL && (page2ppn(arena) & 7) == 0);
    struct buddy_test_record held[64];
    size_t blocks = 0;
    for (size_t order = 0; order <= BUDDY_MAX_ORDER; order++) {
        list_entry_t *head = &free_lists[order], *le = head;
        while ((le = list_next(le)) != head) {
            blocks++;
        }
    }
    if (blocks > sizeof(held) / sizeof(held[0])) {
        free_pages(arena, 8);
        assert(nr_free_pages() == initial_free);
        assert(buddy_test_block_map(0) == original_blocks);
        cprintf("[buddy check] SKIP cases 2/3: more than 64 outside free blocks\n");
        return;
    }
    size_t count = 0;
    for (size_t order = 0; order <= BUDDY_MAX_ORDER; order++) {
        while (!list_empty(&free_lists[order])) {
            size_t n = buddy_block_size(order);
            held[count].base = alloc_pages(n);
            held[count].n = n;
            assert(held[count].base != NULL);
            count++;
        }
    }
    assert(count == blocks && nr_free_pages() == 0);
    free_pages(arena, 8);
    assert(nr_free_pages() == 8);
    buddy_check_invariants();

    cprintf("[buddy check] case 2: isolated aligned 8-page arena and tail data\n");
    assert(buddy_test_take(0, 5, 101, 8) && test_records[0].base == arena);
    assert(nr_free_pages() == 3);
    assert(PageProperty(arena + 5) && arena[5].property == 1);
    assert(PageProperty(arena + 6) && arena[6].property == 2);
    assert(buddy_test_take(1, 2, 102, 8) && test_records[1].base == arena + 6);
    assert(buddy_test_take(2, 1, 103, 8) && test_records[2].base == arena + 5);
    assert(nr_free_pages() == 0 && alloc_pages(1) == NULL);
    buddy_test_verify(8);
    buddy_test_give(1, 8); buddy_test_give(0, 8); buddy_test_give(2, 8);
    cprintf("[buddy check] case 2 PASS\n");

    cprintf("[buddy check] case 3: merge orders, busy tails and non-buddies\n");
    assert(buddy_test_take(0, 5, 110, 8));
    buddy_test_give(0, 8);
    assert(buddy_test_take(0, 8, 111, 8) && test_records[0].base == arena);
    buddy_test_give(0, 8);
    const size_t orders[6][3] = {
        {0, 1, 2}, {0, 2, 1}, {1, 0, 2},
        {1, 2, 0}, {2, 0, 1}, {2, 1, 0}
    };
    for (size_t order = 0; order < 6; order++) {
        assert(buddy_test_take(0, 5, 120, 8));
        assert(buddy_test_take(1, 2, 121, 8));
        assert(buddy_test_take(2, 1, 122, 8));
        for (size_t i = 0; i < 3; i++) {
            buddy_test_give(orders[order][i], 8);
            if (i != 2) {
                assert(alloc_pages(8) == NULL);
                buddy_test_verify(8);
            }
        }
        assert(buddy_test_take(0, 8, 123, 8) && test_records[0].base == arena);
        buddy_test_give(0, 8);
    }
    for (size_t i = 0; i < 8; i++) {
        assert(buddy_test_take(i, 1, i + 130, 8));
    }
    for (size_t i = 0; i < 8; i += 2) buddy_test_give(i, 8);
    for (size_t i = 1; i < 8; i += 2) buddy_test_give(i, 8);
    for (size_t i = 0; i < 4; i++) {
        assert(buddy_test_take(i, 2, i + 140, 8));
        assert(test_records[i].base == arena + i * 2);
    }
    buddy_test_give(1, 8); buddy_test_give(2, 8);
    assert(nr_free_pages() == 4 && alloc_pages(4) == NULL);
    buddy_test_verify(8);
    buddy_test_give(0, 8); buddy_test_give(3, 8);
    assert(buddy_test_take(0, 8, 150, 8) && test_records[0].base == arena);
    buddy_test_give(0, 8);

    for (size_t i = 0; i < count; i++) {
        free_pages(held[i].base, held[i].n);
    }
    assert(nr_free_pages() == initial_free);
    buddy_check_invariants();
    assert(buddy_test_block_map(0) == original_blocks);
    cprintf("[buddy check] case 3 PASS; outside holdings and arena restored\n");
}

static void
buddy_check(void) {
    buddy_check_invariants();
    assert(pmm_manager == &buddy_pmm_manager);
    bool room = 0;
    for (size_t order = 6; order <= BUDDY_MAX_ORDER; order++) {
        if (!list_empty(&free_lists[order])) {
            room = 1;
        }
    }
    // Nine simultaneous basic requests require 59 rounded pages, fitting 64.
    if (!room || npage - nbase > BUDDY_TEST_MAX_PAGES) {
        cprintf("[buddy check] SKIP functional cases: need a 64-page block "
                "and supported reference-map capacity\n");
        return;
    }
    size_t initial_free = nr_free_pages();
    size_t original_blocks = buddy_test_block_map(1);
    for (size_t i = 0; i < BUDDY_TEST_SLOTS; i++) {
        assert(test_records[i].base == NULL);
    }
    cprintf("[buddy check] case 1: exact counts, continuity, ownership, data\n");
    const size_t sizes[BUDDY_TEST_SLOTS] = {1, 2, 4, 8, 3, 5, 6, 7, 9};
    const size_t release_order[BUDDY_TEST_SLOTS] = {8, 0, 6, 2, 7, 4, 1, 5, 3};
    for (size_t i = 0; i < BUDDY_TEST_SLOTS; i++) {
        assert(buddy_test_take(i, sizes[i], i + 1, initial_free));
    }
    for (size_t i = 0; i < BUDDY_TEST_SLOTS; i++) {
        buddy_test_give(release_order[i], initial_free);
    }
    assert(buddy_test_block_map(0) == original_blocks);
    cprintf("[buddy check] case 1 PASS\n");

    buddy_test_isolated_eight(initial_free, original_blocks);

    cprintf("[buddy check] case 5: zero and unrepresentable requests\n");
    assert(alloc_pages(0) == NULL);
    assert(alloc_pages(~(size_t)0) == NULL);
    buddy_test_verify(initial_free);
    assert(buddy_test_block_map(0) == original_blocks);
    cprintf("[buddy check] case 5 PASS (additional boundaries in host fixture)\n");

    cprintf("[buddy check] case 6: seed=20261024, 128 mixed steps\n");
    unsigned int seed = 20261024;
    for (size_t step = 0; step < 128; step++) {
        seed = seed * 1664525U + 1013904223U;
        size_t slot = (seed >> 16) % BUDDY_TEST_SLOTS;
        if (test_records[slot].base != NULL) {
            buddy_test_give(slot, initial_free);
        } else {
            buddy_test_take(slot, 1 + (seed >> 8) % 9, step + 16, initial_free);
        }
    }
    for (size_t slot = 0; slot < BUDDY_TEST_SLOTS; slot++) {
        if (test_records[slot].base != NULL) {
            buddy_test_give(slot, initial_free);
        }
    }
    assert(nr_free_pages() == initial_free);
    assert(buddy_test_block_map(0) == original_blocks);
    buddy_check_invariants();
    cprintf("[buddy check] case 6 PASS; free pages and all blocks restored\n");
}

const struct pmm_manager buddy_pmm_manager = {
    .name = "buddy_pmm_manager",
    .init = buddy_init,
    .init_memmap = buddy_init_memmap,
    .alloc_pages = buddy_alloc_pages,
    .free_pages = buddy_free_pages,
    .nr_free_pages = buddy_nr_free_pages,
    .check = buddy_check,
};
