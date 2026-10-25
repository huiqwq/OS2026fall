// Execute the real modules with this process's matching descriptors/backing RAM.
#include <stdarg.h>
#include <string.h>
#if defined(PMM_BUDDY)
#include "../../kern/mm/buddy_pmm.c"
#define LOWER_MANAGER buddy_pmm_manager
#else
#include "../../kern/mm/best_fit_pmm.c"
#define LOWER_MANAGER best_fit_pmm_manager
#endif
#include "../../kern/mm/slub.c"

extern int puts(const char *);
extern int printf(const char *, ...);
extern int vprintf(const char *, va_list);
extern void exit(int) __attribute__((noreturn));
#define HOST_PAGES 128
static struct Page descriptors[HOST_PAGES], saved_descriptors[HOST_PAGES];
static uint8_t ram[HOST_PAGES][PGSIZE] __attribute__((aligned(PGSIZE)));
static uint8_t saved_ram[sizeof(ram)];
static struct slub_cache saved_caches[SLUB_CACHE_COUNT];
static size_t lower_records[HOST_PAGES], saved_records[HOST_PAGES];
static size_t saved_counts[7];
static uintptr_t saved_first, saved_end;
static bool saved_initialized, expected_panic, strict_records = 1;
static bool fail_next_page_request;
static size_t page_requests, rejected_requests, last_page_request, last_page_free;
static const char *scenario;
#if defined(PMM_BUDDY)
static list_entry_t saved_lower_lists[BUDDY_MAX_ORDER + 1];
#else
static free_area_t saved_lower_area;
#endif
struct Page *pages;
size_t npage;
const size_t nbase = DRAM_BASE / PGSIZE;
uint64_t va_pa_offset;
const struct pmm_manager *pmm_manager = &LOWER_MANAGER;

struct Page *alloc_pages(size_t n) {
    page_requests++; last_page_request = n;
    // Fault injection at the page interface, after real SLUB control flow.
    if (fail_next_page_request) {
        fail_next_page_request = 0;
        rejected_requests++;
        return NULL;
    }
    struct Page *p = pmm_manager->alloc_pages(n);
    if (p && strict_records) {
        assert(p >= pages && p < pages + HOST_PAGES);
        size_t first = p - pages;
        assert(n <= HOST_PAGES - first && lower_records[first] == 0);
        lower_records[first] = n;
    }
    return p;
}
void free_pages(struct Page *p, size_t n) {
    if (strict_records) {
        assert(p >= pages && p < pages + HOST_PAGES);
        size_t first = p - pages;
        // Independent original-request ledger: rejects partial or expanded frees.
        assert(lower_records[first] == n && n != 0);
        lower_records[first] = 0;
    }
    last_page_free = n;
    pmm_manager->free_pages(p, n);
}
size_t nr_free_pages(void) { return pmm_manager->nr_free_pages(); }
int cprintf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int result = vprintf(fmt, ap);
    va_end(ap); return result;
}

static void capture(void) {
    memcpy(saved_descriptors, descriptors, sizeof(descriptors));
    memcpy(saved_ram, ram, sizeof(ram));
    memcpy(saved_caches, caches, sizeof(caches));
    memcpy(saved_records, lower_records, sizeof(lower_records));
    saved_counts[0] = slub_page_count; saved_counts[1] = slab_pages;
    saved_counts[2] = small_objects; saved_counts[3] = large_pages;
    saved_counts[4] = large_objects; saved_counts[5] = nr_free_pages();
    saved_first = first_va; saved_end = end_va; saved_initialized = slub_initialized;
#if defined(PMM_BUDDY)
    memcpy(saved_lower_lists, free_lists, sizeof(free_lists));
    saved_counts[6] = nr_managed;
#else
    memcpy(&saved_lower_area, &free_area, sizeof(free_area));
#endif
}
static bool unchanged(void) {
    bool same = memcmp(saved_descriptors, descriptors, sizeof(descriptors)) == 0 &&
        memcmp(saved_ram, ram, sizeof(ram)) == 0 &&
        memcmp(saved_caches, caches, sizeof(caches)) == 0 &&
        memcmp(saved_records, lower_records, sizeof(lower_records)) == 0 &&
        saved_counts[0] == slub_page_count && saved_counts[1] == slab_pages &&
        saved_counts[2] == small_objects && saved_counts[3] == large_pages &&
        saved_counts[4] == large_objects && saved_counts[5] == nr_free_pages() &&
        saved_first == first_va && saved_end == end_va && saved_initialized == slub_initialized;
#if defined(PMM_BUDDY)
    return same && saved_counts[6] == nr_managed &&
        memcmp(saved_lower_lists, free_lists, sizeof(free_lists)) == 0;
#else
    return same && memcmp(&saved_lower_area, &free_area, sizeof(free_area)) == 0;
#endif
}
void __panic(const char *file, int line, const char *fmt, ...) {
    const char *name = file;
    for (const char *p = file; *p; p++) if (*p == '/') name = p + 1;
    if (expected_panic && strcmp(name, "slub.c") == 0 && unchanged()) {
        printf("PASS %s: SLUB assertion before any observable mutation\n", scenario);
        exit(0);
    }
    printf("FAIL %s at %s:%d: ", scenario, file, line);
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    puts(""); exit(1);
}

static void setup(bool initialize_slub) {
    pages = NULL; npage = 0; pmm_manager->init();
    pages = descriptors; npage = nbase + HOST_PAGES;
    va_pa_offset = (uintptr_t)ram - DRAM_BASE;
    memset(descriptors, 0xa5, sizeof(descriptors)); memset(ram, 0x5a, sizeof(ram));
    for (size_t i = 0; i < HOST_PAGES; i++) SetPageReserved(pages + i);
    pmm_manager->init_memmap(pages + 4, HOST_PAGES - 4);
    if (initialize_slub) {
        size_t before = nr_free_pages();
        memcpy(saved_descriptors, descriptors, sizeof(descriptors));
        slub_init();
        assert(nr_free_pages() == before);
        for (size_t i = 0; i < HOST_PAGES; i++) {
            assert(descriptors[i].slub_owner == 0 && descriptors[i].slub_npages == 0);
            assert(memcmp(descriptors + i, saved_descriptors + i,
                          offsetof(struct Page, slub_owner)) == 0);
        }
    }
}
static void read_only_check(void) {
    capture(); slub_check_invariants(); assert(unchanged());
}
static void fill(void *ptr, size_t size, unsigned int tag) {
    assert(ptr && (uintptr_t)ptr % 16 == 0);
    uint8_t *p = ptr;
    for (size_t i = 0; i < size; i++) p[i] = (uint8_t)(tag * 29 + i * 7 + (i >> 8));
}
static void verify(void *ptr, size_t size, unsigned int tag) {
    uint8_t *p = ptr;
    for (size_t i = 0; i < size; i++) assert(p[i] == (uint8_t)(tag * 29 + i * 7 + (i >> 8)));
}
static void final_empty(size_t baseline) {
    assert(nr_free_pages() == baseline && slab_pages == 0 && large_pages == 0);
    assert(small_objects == 0 && large_objects == 0);
    read_only_check();
}

static void normal(void) {
    setup(1);
    size_t initial = nr_free_pages();
    struct Page *ordinary = NULL;
    if (strcmp(scenario, "ordinary-coexist") == 0) {
        ordinary = alloc_pages(3); assert(ordinary != NULL);
        fill(slub_page_address(ordinary), 3 * PGSIZE, 99);
    }
    size_t baseline = nr_free_pages();
    slub_check();
    assert(test_step > 0); // A resource SKIP must never masquerade as this PASS.
    final_empty(baseline);
    // The original manager check is invoked through its real callback. Its
    // teacher-owned partial-free checks are outside SLUB's exact-free ledger.
    strict_records = 0; pmm_manager->check(); strict_records = 1;
    assert(nr_free_pages() == baseline);
    if (ordinary) {
        verify(slub_page_address(ordinary), 3 * PGSIZE, 99);
        assert(ordinary->slub_owner == 0 && ordinary->slub_npages == 0);
        free_pages(ordinary, 3);
    }
    final_empty(initial);
    for (size_t i = 0; i < HOST_PAGES; i++) assert(lower_records[i] == 0);
    printf("PASS %s: full normal suite, original page frees, final baseline restored\n", scenario);
}

static void oom(void) {
    setup(1);
    size_t baseline = nr_free_pages();
    void *old = kmalloc(32), *large = kmalloc(4097);
    fill(old, 32, 10); fill(large, 4097, 11);
    fail_next_page_request = 1;
    size_t requests_before = page_requests;
    void *reuse = kmalloc(31);
    assert(reuse && page_requests == requests_before && fail_next_page_request);
    fill(reuse, 31, 12);
    bool small = strcmp(scenario, "oom-small") == 0;
    size_t size = small ? 256 : 16385, expected_pages = small ? 1 : 5;
    assert(nr_free_pages() >= expected_pages);
    capture();
    assert(kmalloc(size) == NULL);
    assert(rejected_requests == 1 && page_requests == requests_before + 1);
    assert(last_page_request == expected_pages && !fail_next_page_request && unchanged());
    verify(old, 32, 10); verify(large, 4097, 11); verify(reuse, 31, 12);
    read_only_check();
    size_t before = nr_free_pages();
    void *recovered = kmalloc(size);
    fill(recovered, size, 13);
    assert(nr_free_pages() == before - expected_pages);
    read_only_check();
    kfree(recovered);
    assert(last_page_free == expected_pages && nr_free_pages() == before);
    verify(old, 32, 10); verify(large, 4097, 11); verify(reuse, 31, 12);
    kfree(reuse); kfree(old); kfree(large);
    final_empty(baseline);
    printf("PASS %s: real SLUB page request n=%lu failed, recovery and exact free verified\n",
           scenario, expected_pages);
}

static void snapshot_failures(void) {
    setup(1);
    size_t baseline = nr_free_pages();
    void *a = kmalloc(15), *b = kmalloc(8193);
    fill(a, 15, 7); fill(b, 8193, 8);
    const size_t bad[] = {0, ~(size_t)0, ~(size_t)0 - 4094, ~(size_t)0 - 4095,
        (size_t)1 << (sizeof(size_t) * 8 - 1), (HOST_PAGES + 1) * PGSIZE};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        capture(); assert(kmalloc(bad[i]) == NULL && unchanged());
    }
    capture(); kfree(NULL); assert(unchanged());
    verify(a, 15, 7); verify(b, 8193, 8);
    kfree(a); kfree(b); final_empty(baseline);
    puts("PASS failure-snapshots: all bytes, lists and counters unchanged");
}

static void invalid(void) {
    bool preinit = strcmp(scenario, "before-init-alloc") == 0 || strcmp(scenario, "before-init-free") == 0;
    setup(!preinit);
    capture(); kfree(NULL); assert(unchanged());
    void *a = NULL, *big = NULL, *ordinary = NULL, *free_slot = NULL;
    if (!preinit) {
        a = kmalloc(32); big = kmalloc(4097);
        fill(a, 32, 4); fill(big, 4097, 5);
        if (strcmp(scenario, "duplicate-live-slab") == 0) {
            void *other = kmalloc(32); fill(other, 32, 6); kfree(a);
        }
        if (strcmp(scenario, "duplicate-reclaimed") == 0) kfree(a);
        if (strcmp(scenario, "large-duplicate") == 0) kfree(big);
        if (strcmp(scenario, "ordinary") == 0) {
            struct Page *p = alloc_pages(1); assert(p); ordinary = slub_page_address(p);
        }
        if (strcmp(scenario, "unused-slot") == 0) {
            struct slub_slab *s = slub_validate_slab(slub_address_page((uintptr_t)a), 0);
            for (size_t i = 0; i < s->capacity; i++) if (!slub_bit(s->allocated, i)) {
                free_slot = (uint8_t *)s + s->object_offset + i * s->cache->size;
                break;
            }
            assert(free_slot != NULL);
        }
    }
    capture(); expected_panic = 1;
    if (strcmp(scenario, "before-init-alloc") == 0) kmalloc(1);
    else if (strcmp(scenario, "before-init-free") == 0) kfree(ram[4]);
    else if (strcmp(scenario, "repeat-init") == 0) slub_init();
    else if (strcmp(scenario, "small-interior") == 0) kfree((uint8_t *)a + 1);
    else if (strcmp(scenario, "unused-slot") == 0) kfree(free_slot);
    else if (strcmp(scenario, "duplicate-live-slab") == 0 || strcmp(scenario, "duplicate-reclaimed") == 0) kfree(a);
    else if (strcmp(scenario, "ordinary") == 0) kfree(ordinary);
    else if (strcmp(scenario, "reserved") == 0) kfree(ram[0]);
    else if (strcmp(scenario, "below-map") == 0) kfree((void *)(first_va - 16));
    else if (strcmp(scenario, "above-map") == 0) kfree((void *)end_va);
    else if (strcmp(scenario, "large-offset") == 0) kfree((uint8_t *)big + 16);
    else if (strcmp(scenario, "large-interior") == 0) kfree((uint8_t *)big + PGSIZE);
    else if (strcmp(scenario, "large-duplicate") == 0) kfree(big);
    else exit(2);
    puts("FAIL: invalid operation accepted"); exit(1);
}

static void corrupt(void) {
    setup(1);
    void *a = kmalloc(32), *big = kmalloc(4097);
    fill(a, 32, 4); fill(big, 4097, 5);
    struct Page *page = slub_address_page((uintptr_t)a);
    struct slub_slab *s = slub_validate_slab(page, 0);
    if (strcmp(scenario, "link") == 0) s->link.prev = NULL;
    else if (strcmp(scenario, "freelist-outside") == 0) s->freelist = (void *)1;
    else if (strcmp(scenario, "freelist-cycle") == 0) *(void **)s->freelist = s->freelist;
    else if (strcmp(scenario, "freelist-live") == 0) s->freelist = a;
    else if (strcmp(scenario, "bitmap") == 0) memset(s->allocated, 0, sizeof(s->allocated));
    else if (strcmp(scenario, "capacity") == 0) s->capacity++;
    else if (strcmp(scenario, "offset") == 0) s->object_offset -= 16;
    else if (strcmp(scenario, "inuse") == 0) s->inuse++;
    else if (strcmp(scenario, "wrong-list") == 0) {
        list_del_init(&s->link); list_add(&s->cache->full, &s->link);
    } else if (strcmp(scenario, "owner") == 0) slub_address_page((uintptr_t)big)[1].slub_owner = 0;
    else if (strcmp(scenario, "original-pages") == 0) slub_address_page((uintptr_t)big)->slub_npages = 3;
    else if (strcmp(scenario, "lower-free") == 0) SetPageProperty(page);
    else exit(2);
    capture(); expected_panic = 1; slub_check_invariants();
    puts("FAIL: corruption accepted"); exit(1);
}

int main(int argc, char **argv) {
    assert(argc == 3); scenario = argv[2];
    if (strcmp(argv[1], "normal") == 0) normal();
    else if (strcmp(argv[1], "oom") == 0) oom();
    else if (strcmp(argv[1], "failure") == 0) snapshot_failures();
    else if (strcmp(argv[1], "invalid") == 0) invalid();
    else if (strcmp(argv[1], "corrupt") == 0) corrupt();
    else return 2;
    return 0;
}
