// Compile the actual implementation, including its static invariant checker.
// This process owns the global pages array and its corresponding backing RAM.
#include <stdarg.h>
#include <string.h>
#include "../../kern/mm/buddy_pmm.c"

extern int puts(const char *);
extern int printf(const char *, ...);
extern int vprintf(const char *, va_list);
extern void exit(int) __attribute__((noreturn));

#define MAX_PAGES BUDDY_TEST_MAX_PAGES
#define MAX_LIVE 128
static struct Page pool[MAX_PAGES], saved_pool[MAX_PAGES];
static uint8_t payload[MAX_PAGES][PGSIZE] __attribute__((aligned(PGSIZE)));
static uint8_t registered[MAX_PAGES], seen[MAX_PAGES];
static unsigned int owner[MAX_PAGES];
static struct buddy_test_record live[MAX_LIVE];
static list_entry_t saved_heads[BUDDY_MAX_ORDER + 1];
static size_t capacity, total, saved_free, saved_managed;
static bool saved_initialized, expect_panic;
static const char *scenario;
struct Page *pages;
size_t npage;
const size_t nbase = DRAM_BASE / PGSIZE;
uint64_t va_pa_offset;
const struct pmm_manager *pmm_manager = &buddy_pmm_manager;

struct Page *alloc_pages(size_t n) { return pmm_manager->alloc_pages(n); }
void free_pages(struct Page *p, size_t n) { pmm_manager->free_pages(p, n); }
size_t nr_free_pages(void) { return pmm_manager->nr_free_pages(); }
int cprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int result = vprintf(fmt, ap);
    va_end(ap);
    return result;
}

static void capture(void) {
    memcpy(saved_pool, pool, capacity * sizeof(struct Page));
    memcpy(saved_heads, free_lists, sizeof(free_lists));
    saved_free = nr_free;
    saved_managed = nr_managed;
    saved_initialized = initialized;
}
static bool unchanged(void) {
    return memcmp(saved_pool, pool, capacity * sizeof(struct Page)) == 0 &&
           memcmp(saved_heads, free_lists, sizeof(free_lists)) == 0 &&
           saved_free == nr_free && saved_managed == nr_managed &&
           saved_initialized == initialized;
}
// The reference pattern differs from the implementation's kernel test pattern.
static uint8_t pattern(unsigned int tag, size_t page, size_t byte) {
    return (uint8_t)(tag * 19 + page * 11 + byte * 7 + (byte >> 7));
}
static bool data_intact(void) {
    for (size_t slot = 0; slot < MAX_LIVE; slot++) {
        struct buddy_test_record *r = &live[slot];
        if (!r->base) continue;
        size_t start = r->base - pages;
        for (size_t i = 0; i < r->n; i++) {
            for (size_t j = 0; j < PGSIZE; j++) {
                if (payload[start + i][j] != pattern(r->tag, i, j)) return 0;
            }
        }
    }
    return 1;
}
void __panic(const char *file, int line, const char *fmt, ...) {
    const char *name = file;
    for (const char *p = file; *p; p++) if (*p == '/') name = p + 1;
    if (expect_panic && strcmp(name, "buddy_pmm.c") == 0 &&
        unchanged() && data_intact()) {
        printf("PASS %s: implementation panic before state/data change\n", scenario);
        exit(0);
    }
    printf("FAIL %s at %s:%d: ", scenario, file, line);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    puts("");
    exit(1);
}

static void inspect(void) {
    buddy_check_invariants();
    memset(seen, 0, capacity);
    size_t free_total = 0, held = 0;
    for (size_t order = 0; order <= BUDDY_MAX_ORDER; order++) {
        list_entry_t *head = &free_lists[order], *le = head;
        size_t visits = 0;
        assert(list_next(list_prev(head)) == head && list_prev(list_next(head)) == head);
        while ((le = list_next(le)) != head) {
            assert(++visits <= capacity);
            assert(list_next(list_prev(le)) == le && list_prev(list_next(le)) == le);
            struct Page *p = le2page(le, page_link);
            assert(p >= pages && p < pages + capacity);
            size_t first = p - pages, size = (size_t)1 << order;
            assert(size <= capacity - first && p->property == size);
            assert(((nbase + first) & (size - 1)) == 0);
            for (size_t i = first; i < first + size; i++) {
                assert(registered[i] && owner[i] == 0 && seen[i] == 0);
                seen[i] = 1;
                free_total++;
            }
        }
    }
    for (size_t i = 0; i < capacity; i++) {
        if (!registered[i]) {
            assert(!seen[i] && !owner[i] && PageReserved(pages + i));
        } else if (owner[i]) {
            assert(!seen[i] && owner[i] <= MAX_LIVE);
            struct buddy_test_record *r = &live[owner[i] - 1];
            assert(r->base && pages + i >= r->base && pages + i < r->base + r->n);
            assert(!buddy_free(pages + i));
            held++;
        } else assert(seen[i] == 1);
    }
    size_t record_pages = 0;
    for (size_t slot = 0; slot < MAX_LIVE; slot++) {
        struct buddy_test_record *r = &live[slot];
        if (!r->base) continue;
        size_t first = r->base - pages;
        for (size_t i = 0; i < r->n; i++) {
            assert(owner[first + i] == slot + 1);
            assert(page2pa(r->base + i) == page2pa(r->base) + i * PGSIZE);
        }
        record_pages += r->n;
    }
    assert(record_pages == held && free_total == nr_free_pages());
    assert(free_total + held == total && total == nr_managed);
    assert(data_intact());
}

static void setup(size_t count) {
    assert(count > 0 && count <= MAX_PAGES);
    pages = NULL;
    npage = 0;
    buddy_pmm_manager.init();
    assert(nr_free_pages() == 0 && alloc_pages(1) == NULL);
    pages = pool;
    capacity = count;
    npage = nbase + count;
    // This process's actual descriptor and RAM globals agree with page2pa.
    va_pa_offset = (uintptr_t)payload - DRAM_BASE;
    memset(pool, 0xa5, count * sizeof(struct Page));
    memset(payload, 0xa5, count * PGSIZE);
    memset(registered, 0, count);
    memset(owner, 0, count * sizeof(owner[0]));
    memset(live, 0, sizeof(live));
    for (size_t i = 0; i < count; i++) SetPageReserved(pool + i);
    total = 0;
}
static void register_range(size_t start, size_t n) {
    assert(start < capacity && n > 0 && n <= capacity - start);
    capture();
    for (size_t i = start; i < start + n; i++) assert(!registered[i]);
    buddy_pmm_manager.init_memmap(pages + start, n);
    for (size_t i = 0; i < capacity; i++) {
        if (!registered[i] && (i < start || i >= start + n)) {
            assert(memcmp(pool + i, saved_pool + i, sizeof(struct Page)) == 0);
        }
    }
    for (size_t i = start; i < start + n; i++) registered[i] = 1;
    total += n;
    inspect();
}
// Predict standard-block availability solely from the independent page ledger.
static bool reference_can_allocate(size_t n) {
    if (n == 0 || n > capacity) return 0;
    size_t rounded = 1;
    while (rounded < n) rounded <<= 1;
    if (rounded > capacity) return 0;
    for (size_t first = 0; first <= capacity - rounded; first++) {
        if (((nbase + first) & (rounded - 1)) != 0) continue;
        size_t i = 0;
        while (i < rounded && registered[first + i] && !owner[first + i]) i++;
        if (i == rounded) return 1;
    }
    return 0;
}
static struct Page *take(size_t slot, size_t n, unsigned int tag, bool required) {
    assert(slot < MAX_LIVE && !live[slot].base);
    capture();
    size_t before = nr_free_pages();
    struct Page *p = alloc_pages(n);
    if (!p) {
        assert(!required && !reference_can_allocate(n) && unchanged());
        inspect();
        return NULL;
    }
    assert(reference_can_allocate(n));
    assert(p >= pages && p < pages + capacity);
    size_t first = p - pages;
    assert(n > 0 && n <= capacity - first && p->property == n);
    assert(before >= n && nr_free_pages() == before - n);
    for (size_t i = first; i < first + n; i++) assert(registered[i] && !owner[i]);
    live[slot].base = p;
    live[slot].n = n;
    live[slot].tag = tag;
    for (size_t i = 0; i < n; i++) {
        owner[first + i] = slot + 1;
        for (size_t j = 0; j < PGSIZE; j++) payload[first + i][j] = pattern(tag, i, j);
    }
    inspect();
    return p;
}
static void give(size_t slot) {
    inspect();
    struct buddy_test_record *r = &live[slot];
    assert(r->base);
    size_t first = r->base - pages, n = r->n, before = nr_free_pages();
    free_pages(r->base, n);
    assert(nr_free_pages() == before + n);
    for (size_t i = first; i < first + n; i++) {
        assert(owner[i] == slot + 1);
        owner[i] = 0;
    }
    r->base = NULL;
    inspect();
}
static void fail_request(size_t n) {
    assert(!reference_can_allocate(n));
    capture();
    assert(alloc_pages(n) == NULL);
    assert(unchanged());
    inspect();
}
static void cleanup(void) {
    for (size_t i = 0; i < MAX_LIVE; i++) if (live[i].base) give(i);
    assert(nr_free_pages() == total);
    inspect();
}

static void normal_case(const char *name) {
    if (strcmp(name, "basic") == 0) {
        setup(128); register_range(0, 128);
        const size_t sizes[] = {1, 2, 4, 8, 3, 5, 6, 7, 9};
        const size_t order[] = {8, 0, 6, 2, 7, 4, 1, 5, 3};
        for (size_t i = 0; i < 9; i++) take(i, sizes[i], i + 1, 1);
        for (size_t i = 0; i < 9; i++) give(order[i]);
        assert(pool[0].property == 128);
    } else if (strcmp(name, "tail") == 0) {
        setup(8); register_range(0, 8);
        assert(take(0, 5, 1, 1) == pool && nr_free_pages() == 3);
        assert(PageProperty(pool + 5) && pool[5].property == 1);
        assert(PageProperty(pool + 6) && pool[6].property == 2);
        assert(take(1, 2, 2, 1) == pool + 6);
        assert(take(2, 1, 3, 1) == pool + 5);
        assert(nr_free_pages() == 0);
        fail_request(1); fail_request(2); fail_request(5);
        cleanup();
    } else if (strcmp(name, "merge") == 0) {
        setup(8); register_range(0, 8);
        take(0, 5, 1, 1); give(0);
        assert(take(0, 8, 2, 1) == pool); give(0);
        const size_t orders[6][3] = {{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}};
        for (size_t order = 0; order < 6; order++) {
            take(0, 5, 1, 1); take(1, 2, 2, 1); take(2, 1, 3, 1);
            for (size_t i = 0; i < 3; i++) {
                give(orders[order][i]);
                if (i < 2) fail_request(8);
            }
            assert(take(0, 8, 4, 1) == pool); give(0);
        }
        setup(128); register_range(0, 128);
        for (size_t i = 0; i < 128; i++) take(i, 1, i + 1, 1);
        for (size_t i = 0; i < 128; i += 2) give(i);
        for (size_t i = 1; i < 128; i += 2) give(i);
        assert(pool[0].property == 128);
    } else if (strcmp(name, "non-buddies") == 0) {
        setup(8); register_range(0, 8);
        for (size_t i = 0; i < 4; i++) assert(take(i, 2, i + 1, 1) == pool + 2 * i);
        give(1); give(2);
        assert(nr_free_pages() == 4 && pool[2].property == 2 && pool[4].property == 2);
        fail_request(4);
        cleanup();
        assert(pool[0].property == 8);
    } else if (strcmp(name, "list-ends") == 0) {
        setup(8);
        for (size_t i = 0; i < 8; i += 2) register_range(i, 1);
        assert(!list_empty(&free_lists[0]));
        for (size_t i = 0; i < 4; i++) take(i, 1, i + 1, 1);
        fail_request(1);
        cleanup();
    } else if (strcmp(name, "init-ranges") == 0) {
        setup(128);
        register_range(3, 13); register_range(35, 21);
        register_range(64, 1); register_range(0, 3);
        assert(pool[0].property == 16);
        size_t n = total;
        for (size_t i = 0; i < n; i++) take(i, 1, i + 1, 1);
        fail_request(1);
        for (size_t i = 0; i < capacity; i++) assert((owner[i] != 0) == (registered[i] != 0));
        cleanup();
    } else if (strcmp(name, "single") == 0) {
        setup(1); register_range(0, 1); take(0, 1, 1, 1); fail_request(1); give(0);
    } else if (strcmp(name, "maximum") == 0) {
        setup(MAX_PAGES); register_range(0, MAX_PAGES);
        assert(PageProperty(pool) && pool[0].property == MAX_PAGES);
        size_t order = 0;
        while (((size_t)1 << order) < MAX_PAGES) order++;
        assert(((size_t)1 << order) == MAX_PAGES && !list_empty(&free_lists[order]));
        assert(take(0, MAX_PAGES, 1, 1) == pool); fail_request(1); give(0);
        assert(pool[0].property == MAX_PAGES);
        printf("maximum available order=%lu, pages=%lu\n", order, (size_t)MAX_PAGES);
    } else if (strcmp(name, "failures") == 0) {
        setup(8); inspect(); fail_request(1); fail_request(0); fail_request(~(size_t)0);
        register_range(0, 8);
        const size_t requests[] = {0, 9, ((size_t)1 << BUDDY_MAX_ORDER) - 1,
            (size_t)1 << BUDDY_MAX_ORDER, ((size_t)1 << BUDDY_MAX_ORDER) + 1,
            (size_t)1 << (sizeof(size_t) * 8 - 1), ~(size_t)0};
        for (size_t i = 0; i < sizeof(requests) / sizeof(requests[0]); i++) fail_request(requests[i]);
        setup(8); register_range(0, 5);
        assert(nr_free_pages() == 5);
        fail_request(5); // Allowed: continuous 5 pages, but no standard 8-page block.
        take(0, 4, 1, 1); fail_request(2); give(0);
    } else if (strcmp(name, "random") == 0) {
        setup(128); register_range(0, 128);
        unsigned int seed = 20261024;
        for (size_t step = 0; step < 2000; step++) {
            seed = seed * 1664525U + 1013904223U;
            size_t slot = (seed >> 16) % 32;
            if (live[slot].base) give(slot);
            else take(slot, 1 + (seed >> 8) % 17, step + 1, 0);
        }
        cleanup();
        assert(pool[0].property == 128);
    } else if (strcmp(name, "kernel-check") == 0 || strcmp(name, "kernel-check-small") == 0) {
        size_t count = strcmp(name, "kernel-check-small") == 0 ? 64 : 256;
        setup(count); register_range(0, count);
        if (count == 256) take(0, 5, 73, 1); // Protect an existing external caller.
        buddy_pmm_manager.check();
        inspect();
        cleanup();
    } else { printf("Unknown normal case %s\n", name); exit(2); }
    cleanup();
    printf("PASS %s\n", name);
}

static void invalid_case(const char *name) {
    setup(128); register_range(16, 64);
    struct Page *a = take(0, 5, 1, 1);
    take(1, 2, 2, 1);
    if (strcmp(name, "duplicate") == 0) give(0);
    if (strcmp(name, "joined") == 0) {
        setup(8); register_range(0, 8);
        a = take(0, 4, 1, 1); take(1, 4, 2, 1);
    }
    capture(); expect_panic = 1;
    if (strcmp(name, "null") == 0) free_pages(NULL, 1);
    else if (strcmp(name, "zero") == 0) free_pages(a, 0);
    else if (strcmp(name, "short") == 0) free_pages(a, 4);
    else if (strcmp(name, "long") == 0) free_pages(a, 6);
    else if (strcmp(name, "internal") == 0) free_pages(a + 1, 4);
    else if (strcmp(name, "duplicate") == 0) free_pages(a, 5);
    else if (strcmp(name, "joined") == 0) free_pages(a, 8);
    else if (strcmp(name, "reserved") == 0) free_pages(pool, 1);
    else if (strcmp(name, "unregistered") == 0) free_pages(pool + 100, 1);
    else if (strcmp(name, "before-array") == 0) free_pages((struct Page *)((uintptr_t)pool - sizeof(struct Page)), 1);
    else if (strcmp(name, "past-array") == 0) free_pages(pool + capacity, 1);
    else if (strcmp(name, "misaligned-pointer") == 0) free_pages((struct Page *)((uintptr_t)a + 1), 5);
    else if (strcmp(name, "huge-length") == 0) free_pages(a, ~(size_t)0);
    else if (strcmp(name, "overlap-registration") == 0) buddy_pmm_manager.init_memmap(pool + 8, 32);
    else if (strcmp(name, "zero-registration") == 0) buddy_pmm_manager.init_memmap(pool, 0);
    else if (strcmp(name, "range-registration") == 0) buddy_pmm_manager.init_memmap(pool + 120, 16);
    else { puts("Unknown invalid case"); exit(2); }
    puts("FAIL: expected implementation panic was not raised"); exit(1);
}

static void corrupt_case(const char *name) {
    setup(8); register_range(0, 8);
    if (strcmp(name, "null-link") == 0) free_lists[3].next = NULL;
    else if (strcmp(name, "prev-link") == 0) pool[0].page_link.prev = &pool[1].page_link;
    else if (strcmp(name, "count") == 0) nr_free++;
    else if (strcmp(name, "size") == 0) pool[0].property = 7;
    else if (strcmp(name, "alignment") == 0) {
        buddy_remove_block(pool);
        pool[1].property = 4; SetPageProperty(pool + 1);
        list_add(&free_lists[2], &pool[1].page_link);
    } else if (strcmp(name, "inuse-page") == 0) pool[3].flags &= ~BUDDY_FREE;
    else if (strcmp(name, "overlap") == 0) buddy_insert_block(pool + 2, 1);
    else if (strcmp(name, "unmerged") == 0) {
        buddy_remove_block(pool); buddy_insert_block(pool, 2); buddy_insert_block(pool + 4, 2);
    } else if (strcmp(name, "leak") == 0) list_del_init(&pool[0].page_link);
    else { puts("Unknown corruption case"); exit(2); }
    capture(); expect_panic = 1;
    buddy_check_invariants();
    puts("FAIL: broken invariant was not detected"); exit(1);
}

int main(int argc, char **argv) {
    assert(argc == 3);
    scenario = argv[2];
    if (strcmp(argv[1], "normal") == 0) normal_case(argv[2]);
    else if (strcmp(argv[1], "invalid") == 0) invalid_case(argv[2]);
    else if (strcmp(argv[1], "corrupt") == 0) corrupt_case(argv[2]);
    else return 2;
    return 0;
}
