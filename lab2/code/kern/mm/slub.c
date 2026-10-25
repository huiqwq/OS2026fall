#include <slub.h>
#include <pmm.h>
#include <list.h>
#include <stdio.h>
#include <string.h>

// LAB2 optional SLUB: single-core caches, one-page slabs, exact large pages.
#define SLUB_CACHE_COUNT 8
#define SLUB_ALIGNMENT 16
#define SLUB_MAX_SLOTS (PGSIZE / SLUB_ALIGNMENT)
#define SLUB_BITMAP_BYTES ((SLUB_MAX_SLOTS + 7) / 8)
#define SLUB_KIND_MASK 3
#define SLUB_SMALL 1
#define SLUB_LARGE 2

struct slub_cache {
    size_t size;
    list_entry_t partial, full;
};

struct slub_slab {
    struct slub_cache *cache;
    list_entry_t link;
    size_t object_offset, capacity, inuse;
    void *freelist;
    uint8_t allocated[SLUB_BITMAP_BYTES];
};

static const size_t class_sizes[SLUB_CACHE_COUNT] = {
    16, 32, 64, 128, 256, 512, 1024, 2048
};
static struct slub_cache caches[SLUB_CACHE_COUNT];
static bool slub_initialized;
static size_t slub_page_count, slab_pages, small_objects, large_pages, large_objects;
static uintptr_t first_va, end_va;

static size_t
slub_page_index(struct Page *page) {
    assert(slub_initialized && page != NULL);
    assert((uintptr_t)page >= (uintptr_t)pages);
    uintptr_t delta = (uintptr_t)page - (uintptr_t)pages;
    assert(delta % sizeof(struct Page) == 0);
    size_t index = delta / sizeof(struct Page);
    assert(index < slub_page_count);
    return index;
}

static uintptr_t
slub_owner_value(struct Page *head, unsigned int kind) {
    assert(kind == SLUB_SMALL || kind == SLUB_LARGE);
    return ((slub_page_index(head) + 1) << 2) | kind;
}

static struct Page *
slub_owner_head(uintptr_t owner) {
    unsigned int kind = owner & SLUB_KIND_MASK;
    assert(kind == SLUB_SMALL || kind == SLUB_LARGE);
    size_t ordinal = owner >> 2;
    assert(ordinal > 0 && ordinal <= slub_page_count);
    return pages + ordinal - 1;
}

static void *
slub_page_address(struct Page *page) {
    slub_page_index(page);
    return (void *)(page2pa(page) + va_pa_offset);
}

// Validate the whole mapped interval before consulting even Page ownership.
static struct Page *
slub_address_page(uintptr_t address) {
    assert(address >= first_va && address < end_va);
    uintptr_t pa = address - va_pa_offset;
    assert(PPN(pa) >= nbase && PPN(pa) < npage);
    return pa2page(pa);
}

static void
slub_check_backing(struct Page *head, size_t n) {
    size_t first = slub_page_index(head);
    assert(n > 0 && n <= slub_page_count - first);
    for (size_t i = 0; i < n; i++) {
        struct Page *p = head + i;
        assert(!PageReserved(p) && !PageProperty(p));
#if defined(PMM_BUDDY)
        // Read the documented lower-layer state; never alter its metadata/list.
        assert((p->flags & ((uint64_t)1 << PG_buddy_managed)) != 0);
        assert((p->flags & ((uint64_t)1 << PG_buddy_free)) == 0);
        assert(p->property == (i == 0 ? n : 0));
#endif
    }
}

static void
slub_record_pages(struct Page *head, size_t n, unsigned int kind) {
    slub_check_backing(head, n);
    uintptr_t owner = slub_owner_value(head, kind);
    for (size_t i = 0; i < n; i++) {
        assert(head[i].slub_owner == 0 && head[i].slub_npages == 0);
    }
    for (size_t i = 0; i < n; i++) {
        head[i].slub_owner = owner;
        head[i].slub_npages = i == 0 ? n : 0;
    }
}

static void
slub_validate_pages(struct Page *head, size_t n, unsigned int kind) {
    slub_check_backing(head, n);
    uintptr_t owner = slub_owner_value(head, kind);
    for (size_t i = 0; i < n; i++) {
        assert(head[i].slub_owner == owner);
        assert(head[i].slub_npages == (i == 0 ? n : 0));
    }
}

static void
slub_forget_pages(struct Page *head, size_t n) {
    for (size_t i = 0; i < n; i++) {
        head[i].slub_owner = 0;
        head[i].slub_npages = 0;
    }
}

static struct slub_cache *
slub_validate_cache(struct slub_cache *cache) {
    uintptr_t address = (uintptr_t)cache, first = (uintptr_t)caches;
    assert(address >= first && address - first < sizeof(caches));
    assert((address - first) % sizeof(struct slub_cache) == 0);
    size_t index = (address - first) / sizeof(struct slub_cache);
    assert(cache->size == class_sizes[index]);
    return cache;
}

static size_t
slub_object_offset(void) {
    return ROUNDUP(sizeof(struct slub_slab), SLUB_ALIGNMENT);
}

static struct slub_slab *
slub_validate_slab(struct Page *page, bool allow_empty) {
    slub_validate_pages(page, 1, SLUB_SMALL);
    // The descriptor has established provenance before reading this page.
    struct slub_slab *slab = slub_page_address(page);
    struct slub_cache *cache = slub_validate_cache(slab->cache);
    size_t offset = slub_object_offset();
    assert(slab->object_offset == offset);
    assert(slab->capacity == (PGSIZE - offset) / cache->size);
    assert(slab->capacity > 0 && slab->capacity <= SLUB_MAX_SLOTS);
    assert(slab->inuse <= slab->capacity && (allow_empty || slab->inuse > 0));
    return slab;
}

static size_t
slub_slot_index(struct slub_slab *slab, void *object) {
    uintptr_t start = (uintptr_t)slab + slab->object_offset;
    uintptr_t address = (uintptr_t)object;
    assert(address >= start);
    uintptr_t delta = address - start;
    assert(delta % slab->cache->size == 0);
    size_t index = delta / slab->cache->size;
    assert(index < slab->capacity);
    return index;
}

static bool
slub_bit(const uint8_t *bitmap, size_t index) {
    return (bitmap[index / 8] & (1U << (index % 8))) != 0;
}

static bool
slub_valid_link(list_entry_t *link, list_entry_t *head) {
    if (link == head) {
        return 1;
    }
    uintptr_t address = (uintptr_t)link;
    if (address < first_va || address >= end_va ||
        (address - first_va) % PGSIZE != offsetof(struct slub_slab, link)) {
        return 0;
    }
    struct Page *page = slub_address_page(address);
    return page->slub_owner == slub_owner_value(page, SLUB_SMALL) &&
           page->slub_npages == 1 && !PageReserved(page) && !PageProperty(page);
}

static void
slub_check_links(list_entry_t *link, list_entry_t *head) {
    assert(slub_valid_link(link, head));
    assert(slub_valid_link(list_prev(link), head));
    assert(slub_valid_link(list_next(link), head));
    assert(list_next(list_prev(link)) == link);
    assert(list_prev(list_next(link)) == link);
}

void
slub_init(void) {
    assert(!slub_initialized);
    assert(pages != NULL && npage > nbase);
    assert(npage <= (~(uintptr_t)0 >> PGSHIFT));
    size_t count = npage - nbase;
    assert(count < (~(uintptr_t)0 >> 2));
    assert(count <= (~(uintptr_t)0 - (uintptr_t)pages) / sizeof(struct Page));
    assert(count <= ~(size_t)0 / SLUB_MAX_SLOTS);
    assert((va_pa_offset % PGSIZE) == 0);
    uintptr_t pa_first = nbase << PGSHIFT, pa_end = npage << PGSHIFT;
    assert(pa_end <= ~(uintptr_t)0 - va_pa_offset);
    static_assert(SLUB_ALIGNMENT >= sizeof(void *));
    static_assert(sizeof(struct slub_slab) + SLUB_ALIGNMENT - 1 < PGSIZE);
    size_t before = nr_free_pages();
    for (size_t i = 0; i < SLUB_CACHE_COUNT; i++) {
        caches[i].size = class_sizes[i];
        list_init(&caches[i].partial);
        list_init(&caches[i].full);
        assert((PGSIZE - slub_object_offset()) / caches[i].size > 0);
    }
    // New descriptor fields may contain arbitrary RAM, including on reserved pages.
    for (size_t i = 0; i < count; i++) {
        pages[i].slub_owner = 0;
        pages[i].slub_npages = 0;
    }
    slub_page_count = count;
    first_va = pa_first + va_pa_offset;
    end_va = pa_end + va_pa_offset;
    slab_pages = small_objects = large_pages = large_objects = 0;
    slub_initialized = 1;
    assert(nr_free_pages() == before);
    cprintf("slub: initialized, %lu free pages unchanged\n", before);
}

static struct slub_slab *
slub_new_slab(struct slub_cache *cache) {
    struct Page *page = alloc_pages(1);
    if (page == NULL) {
        return NULL;
    }
    slub_record_pages(page, 1, SLUB_SMALL);
    struct slub_slab *slab = slub_page_address(page);
    memset(slab, 0, sizeof(*slab));
    slab->cache = cache;
    slab->object_offset = slub_object_offset();
    slab->capacity = (PGSIZE - slab->object_offset) / cache->size;
    list_init(&slab->link);
    for (size_t i = slab->capacity; i > 0; i--) {
        void *object = (uint8_t *)slab + slab->object_offset + (i - 1) * cache->size;
        *(void **)object = slab->freelist;
        slab->freelist = object;
    }
    slab_pages++;
    return slab;
}

void *
kmalloc(size_t size) {
    assert(slub_initialized);
    if (size == 0) {
        return NULL;
    }
    if (size > class_sizes[SLUB_CACHE_COUNT - 1]) {
        if (size > ~(size_t)0 - (PGSIZE - 1)) {
            return NULL;
        }
        size_t n = (size + PGSIZE - 1) / PGSIZE;
        if (n > slub_page_count || n > nr_free_pages()) {
            return NULL;
        }
        struct Page *page = alloc_pages(n);
        if (page == NULL) {
            return NULL;
        }
        slub_record_pages(page, n, SLUB_LARGE);
        large_pages += n;
        large_objects++;
        return slub_page_address(page);
    }

    size_t index = 0;
    while (class_sizes[index] < size) {
        index++;
    }
    struct slub_cache *cache = &caches[index];
    struct slub_slab *slab;
    bool fresh = list_empty(&cache->partial);
    if (fresh) {
        slab = slub_new_slab(cache);
        if (slab == NULL) {
            return NULL;
        }
    } else {
        list_entry_t *link = list_next(&cache->partial);
        slub_check_links(link, &cache->partial);
        struct Page *page = slub_address_page((uintptr_t)link);
        slab = slub_validate_slab(page, 0);
        assert(slab->cache == cache && slab->inuse < slab->capacity);
    }
    void *object = slab->freelist;
    assert(object != NULL);
    size_t slot = slub_slot_index(slab, object);
    assert(!slub_bit(slab->allocated, slot));
    void *next = *(void **)object;
    if (next != NULL) {
        size_t next_slot = slub_slot_index(slab, next);
        assert(next_slot != slot && !slub_bit(slab->allocated, next_slot));
    }
    assert((next == NULL) == (slab->capacity - slab->inuse == 1));
    slab->freelist = next;
    slab->allocated[slot / 8] |= 1U << (slot % 8);
    slab->inuse++;
    small_objects++;
    if (fresh) {
        list_add(slab->inuse == slab->capacity ? &cache->full : &cache->partial,
                 &slab->link);
    } else if (slab->inuse == slab->capacity) {
        list_del_init(&slab->link);
        list_add(&cache->full, &slab->link);
    }
    return object;
}

void
kfree(void *ptr) {
    if (ptr == NULL) {
        return;
    }
    assert(slub_initialized);
    struct Page *page = slub_address_page((uintptr_t)ptr);
    uintptr_t owner = page->slub_owner;
    // Ordinary/reserved/previously reclaimed pages fail without reading payload.
    struct Page *head = slub_owner_head(owner);
    unsigned int kind = owner & SLUB_KIND_MASK;
    if (kind == SLUB_LARGE) {
        assert(page == head && ptr == slub_page_address(head));
        size_t n = head->slub_npages;
        slub_validate_pages(head, n, SLUB_LARGE);
        assert(large_objects > 0 && large_pages >= n);
        slub_forget_pages(head, n);
        large_objects--;
        large_pages -= n;
        free_pages(head, n);
        return;
    }
    assert(kind == SLUB_SMALL && page == head);
    struct slub_slab *slab = slub_validate_slab(page, 0);
    struct slub_cache *cache = slab->cache;
    size_t slot = slub_slot_index(slab, ptr);
    assert(slub_bit(slab->allocated, slot));
    assert(small_objects > 0 && slab_pages > 0);
    list_entry_t *list = slab->inuse == slab->capacity ? &cache->full : &cache->partial;
    slub_check_links(&slab->link, list);
    if (slab->inuse == 1) {
        list_del_init(&slab->link);
        slub_forget_pages(page, 1);
        small_objects--;
        slab_pages--;
        free_pages(page, 1);
        return; // Never read the slab after returning its page.
    }
    bool was_full = slab->inuse == slab->capacity;
    slab->allocated[slot / 8] &= ~(1U << (slot % 8));
    *(void **)ptr = slab->freelist;
    slab->freelist = ptr;
    slab->inuse--;
    small_objects--;
    if (was_full) {
        list_del_init(&slab->link);
        list_add(&cache->partial, &slab->link);
    }
}

static void
slub_check_objects(struct slub_slab *slab) {
    uint8_t seen[SLUB_BITMAP_BYTES] = {0};
    size_t used = 0, free_count = 0;
    for (size_t i = 0; i < SLUB_MAX_SLOTS; i++) {
        if (i < slab->capacity) {
            used += slub_bit(slab->allocated, i);
        } else {
            assert(!slub_bit(slab->allocated, i));
        }
    }
    assert(used == slab->inuse);
    void *object = slab->freelist;
    while (object != NULL) {
        assert(++free_count <= slab->capacity);
        size_t slot = slub_slot_index(slab, object);
        assert(!slub_bit(slab->allocated, slot) && !slub_bit(seen, slot));
        seen[slot / 8] |= 1U << (slot % 8);
        object = *(void **)object;
    }
    assert(free_count == slab->capacity - slab->inuse);
    for (size_t i = 0; i < slab->capacity; i++) {
        assert(slub_bit(seen, i) != slub_bit(slab->allocated, i));
        assert(((uintptr_t)slab + slab->object_offset + i * slab->cache->size) %
               SLUB_ALIGNMENT == 0);
    }
}

static void
slub_check_invariants(void) {
    assert(slub_initialized && slub_page_count == npage - nbase);
    size_t before = nr_free_pages(), listed = 0, listed_objects = 0;
    for (size_t i = 0; i < SLUB_CACHE_COUNT; i++) {
        struct slub_cache *cache = slub_validate_cache(&caches[i]);
        for (size_t full = 0; full < 2; full++) {
            list_entry_t *head = full ? &cache->full : &cache->partial;
            list_entry_t *link = head;
            do {
                slub_check_links(link, head);
                link = list_next(link);
                if (link == head) {
                    break;
                }
                assert(++listed <= slub_page_count);
                struct Page *page = slub_address_page((uintptr_t)link);
                struct slub_slab *slab = slub_validate_slab(page, 0);
                assert(slab->cache == cache);
                assert(full ? slab->inuse == slab->capacity : slab->inuse < slab->capacity);
                slub_check_objects(slab);
                listed_objects += slab->inuse;
            } while (1);
        }
    }
    size_t owned_slabs = 0, owned_large = 0, owned_large_pages = 0;
    for (size_t i = 0; i < slub_page_count;) {
        struct Page *page = pages + i;
        uintptr_t owner = page->slub_owner;
        if (owner == 0) {
            assert(page->slub_npages == 0);
            i++;
            continue;
        }
        struct Page *head = slub_owner_head(owner);
        assert(head == page);
        if ((owner & SLUB_KIND_MASK) == SLUB_SMALL) {
            slub_validate_slab(page, 0);
            owned_slabs++;
            i++;
        } else {
            size_t n = page->slub_npages;
            slub_validate_pages(page, n, SLUB_LARGE);
            owned_large++;
            owned_large_pages += n;
            i += n;
        }
    }
    assert(listed == owned_slabs && listed == slab_pages);
    assert(listed_objects == small_objects);
    assert(owned_large == large_objects && owned_large_pages == large_pages);
    assert(slab_pages <= slub_page_count && large_pages <= slub_page_count - slab_pages);
    assert(nr_free_pages() == before);
}

#if defined(SLUB_TEST) && SLUB_TEST
// Test reference data is independent of kmalloc and compiled only for self-test.
#define SLUB_TEST_SLOTS (SLUB_MAX_SLOTS + 8)
struct slub_test_record {
    uint8_t *ptr;
    size_t size;
    struct Page *head;
    size_t npages;
    unsigned int tag;
};
static struct slub_test_record test_live[SLUB_TEST_SLOTS];
static const char *test_stage;
static size_t test_step, test_baseline;

#define SLUB_TEST_ASSERT(expr) do {                                      \
    if (!(expr)) {                                                       \
        panic("SLUB test [%s], step=%lu: %s", test_stage, test_step, #expr); \
    }                                                                   \
} while (0)

static size_t
slub_test_class(size_t size) {
    size_t index = 0;
    while (index < SLUB_CACHE_COUNT && ((size_t)16 << index) < size) index++;
    return index;
}

static size_t
slub_test_refs(struct Page *page) {
    size_t count = 0;
    for (size_t i = 0; i < SLUB_TEST_SLOTS; i++) {
        if (test_live[i].ptr && test_live[i].size <= 2048 && test_live[i].head == page) count++;
    }
    return count;
}

static uint8_t
slub_test_pattern(unsigned int tag, size_t offset) {
    return (uint8_t)(tag * 41 + offset * 13 + (offset >> 7));
}

static void
slub_test_verify(void) {
    size_t expected_pages = 0, expected_slabs = 0, expected_small = 0;
    size_t expected_large = 0, expected_large_pages = 0;
    for (size_t i = 0; i < SLUB_TEST_SLOTS; i++) {
        struct slub_test_record *r = &test_live[i];
        if (!r->ptr) continue;
        uintptr_t address = (uintptr_t)r->ptr;
        SLUB_TEST_ASSERT(address % 16 == 0 && address >= first_va && address < end_va);
        SLUB_TEST_ASSERT(r->size <= end_va - address);
        slub_check_backing(r->head, r->npages);
        unsigned int kind = r->size <= 2048 ? SLUB_SMALL : SLUB_LARGE;
        uintptr_t owner = (((size_t)(r->head - pages) + 1) << 2) | kind;
        for (size_t p = 0; p < r->npages; p++) {
            SLUB_TEST_ASSERT(r->head[p].slub_owner == owner);
            SLUB_TEST_ASSERT(r->head[p].slub_npages == (p == 0 ? r->npages : 0));
            SLUB_TEST_ASSERT(page2pa(r->head + p) == page2pa(r->head) + p * PGSIZE);
        }
        if (kind == SLUB_SMALL) {
            size_t index = slub_test_class(r->size), stride = (size_t)16 << index;
            size_t offset = ROUNDUP(sizeof(struct slub_slab), 16);
            struct slub_slab *slab = slub_page_address(r->head);
            SLUB_TEST_ASSERT(index < SLUB_CACHE_COUNT && slab->cache == &caches[index]);
            SLUB_TEST_ASSERT(slab->cache->size == stride && slab->object_offset == offset);
            SLUB_TEST_ASSERT(slab->capacity == (PGSIZE - offset) / stride);
            SLUB_TEST_ASSERT(address >= (uintptr_t)slab + offset);
            size_t delta = address - (uintptr_t)slab - offset;
            SLUB_TEST_ASSERT(delta % stride == 0 && delta / stride < slab->capacity);
            SLUB_TEST_ASSERT(slub_bit(slab->allocated, delta / stride));
            SLUB_TEST_ASSERT(slab->inuse == slub_test_refs(r->head));
            bool first = 1;
            for (size_t j = 0; j < i; j++) {
                if (test_live[j].ptr && test_live[j].size <= 2048 && test_live[j].head == r->head) first = 0;
            }
            if (first) { expected_pages++; expected_slabs++; }
            expected_small++;
        } else {
            SLUB_TEST_ASSERT(r->npages == r->size / PGSIZE + (r->size % PGSIZE != 0));
            SLUB_TEST_ASSERT(r->ptr == slub_page_address(r->head));
            expected_pages += r->npages;
            expected_large_pages += r->npages;
            expected_large++;
        }
        for (size_t j = i + 1; j < SLUB_TEST_SLOTS; j++) {
            struct slub_test_record *other = &test_live[j];
            if (other->ptr) {
                SLUB_TEST_ASSERT(address + r->size <= (uintptr_t)other->ptr ||
                    (uintptr_t)other->ptr + other->size <= address);
            }
        }
        for (size_t j = 0; j < r->size; j++) {
            SLUB_TEST_ASSERT(r->ptr[j] == slub_test_pattern(r->tag, j));
        }
    }
    SLUB_TEST_ASSERT(nr_free_pages() + expected_pages == test_baseline);
    SLUB_TEST_ASSERT(slab_pages == expected_slabs && small_objects == expected_small);
    SLUB_TEST_ASSERT(large_pages == expected_large_pages && large_objects == expected_large);
    // Full global ownership/list checks at bounded checkpoints; data is checked every step.
    if ((test_step & 15) == 0) slub_check_invariants();
}

static void
slub_test_take(size_t slot, size_t size, unsigned int tag) {
    test_step++;
    SLUB_TEST_ASSERT(slot < SLUB_TEST_SLOTS && !test_live[slot].ptr && size > 0);
    size_t before = nr_free_pages();
    uint8_t *ptr = kmalloc(size);
    SLUB_TEST_ASSERT(ptr != NULL);
    uintptr_t address = (uintptr_t)ptr;
    SLUB_TEST_ASSERT(address >= first_va && address < end_va && address % 16 == 0);
    SLUB_TEST_ASSERT(size <= end_va - address);
    struct Page *head = slub_address_page(address);
    size_t n = size <= 2048 ? 1 : size / PGSIZE + (size % PGSIZE != 0);
    size_t cost = size <= 2048 && slub_test_refs(head) != 0 ? 0 : n;
    SLUB_TEST_ASSERT(before >= cost && nr_free_pages() == before - cost);
    slub_validate_pages(head, n, size <= 2048 ? SLUB_SMALL : SLUB_LARGE);
    if (size <= 2048) {
        struct slub_slab *slab = slub_validate_slab(head, 0);
        size_t index = slub_test_class(size), stride = (size_t)16 << index;
        size_t offset = ROUNDUP(sizeof(struct slub_slab), 16);
        SLUB_TEST_ASSERT(index < SLUB_CACHE_COUNT && slab->cache == &caches[index]);
        SLUB_TEST_ASSERT(address >= (uintptr_t)slab + offset);
        size_t delta = address - (uintptr_t)slab - offset;
        SLUB_TEST_ASSERT(delta % stride == 0 && delta / stride < (PGSIZE - offset) / stride);
    } else {
        SLUB_TEST_ASSERT(ptr == slub_page_address(head));
    }
    for (size_t i = 0; i < SLUB_TEST_SLOTS; i++) {
        if (test_live[i].ptr) {
            SLUB_TEST_ASSERT(address + size <= (uintptr_t)test_live[i].ptr ||
                (uintptr_t)test_live[i].ptr + test_live[i].size <= address);
        }
    }
    test_live[slot].ptr = ptr; test_live[slot].size = size;
    test_live[slot].head = head; test_live[slot].npages = n; test_live[slot].tag = tag;
    for (size_t i = 0; i < size; i++) ptr[i] = slub_test_pattern(tag, i);
    slub_test_verify();
}

static void
slub_test_give(size_t slot) {
    test_step++;
    SLUB_TEST_ASSERT(slot < SLUB_TEST_SLOTS && test_live[slot].ptr);
    slub_test_verify();
    struct slub_test_record r = test_live[slot];
    size_t gain = r.size <= 2048 && slub_test_refs(r.head) > 1 ? 0 : r.npages;
    size_t before = nr_free_pages();
    kfree(r.ptr);
    memset(&test_live[slot], 0, sizeof(test_live[slot]));
    SLUB_TEST_ASSERT(nr_free_pages() == before + gain);
    if (gain) {
        for (size_t i = 0; i < r.npages; i++) {
            // Read descriptors only: the slab payload has already been returned.
            SLUB_TEST_ASSERT(r.head[i].slub_owner == 0 && r.head[i].slub_npages == 0);
        }
    }
    slub_test_verify();
}

static void
slub_test_finish_case(void) {
    for (size_t i = 0; i < SLUB_TEST_SLOTS; i++) {
        if (test_live[i].ptr) slub_test_give(i);
    }
    SLUB_TEST_ASSERT(nr_free_pages() == test_baseline);
    SLUB_TEST_ASSERT(slab_pages == 0 && small_objects == 0 && large_pages == 0 && large_objects == 0);
    slub_check_invariants();
}

static void
slub_test_boundaries(void) {
    test_stage = "case 1: size boundaries";
    cprintf("[slub test] %s\n", test_stage);
    const size_t sizes[] = {1,15,16,17,31,32,33,63,64,65,127,128,129,
        255,256,257,511,512,513,1023,1024,1025,2047,2048,2049};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) slub_test_take(i, sizes[i], i + 1);
    slub_test_finish_case();
    cprintf("[slub test] case 1 PASS\n");
}

static void
slub_test_large(void) {
    test_stage = "case 2: exact large pages";
    cprintf("[slub test] %s\n", test_stage);
    const size_t sizes[] = {4095,4096,4097,8192,8193,16385};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) slub_test_take(i, sizes[i], i + 40);
    SLUB_TEST_ASSERT(test_live[5].npages == 5 && test_live[5].head->slub_npages == 5);
    const size_t order[] = {2,5,0,4,1,3};
    for (size_t i = 0; i < 6; i++) slub_test_give(order[i]);
    slub_test_finish_case();
    cprintf("[slub test] case 2 PASS (16385 bytes = 5 pages)\n");
}

static void
slub_test_multislab(void) {
    test_stage = "cases 3/4: multi-slab and immediate return";
    cprintf("[slub test] %s\n", test_stage);
    for (size_t index = 0; index < SLUB_CACHE_COUNT; index++) {
        size_t size = (size_t)16 << index;
        size_t capacity = (PGSIZE - ROUNDUP(sizeof(struct slub_slab), 16)) / size;
        SLUB_TEST_ASSERT(capacity > 0 && capacity + 1 <= SLUB_TEST_SLOTS);
        cprintf("[slub test] class=%lu capacity=%lu\n", size, capacity);
        for (size_t i = 0; i < capacity + 1; i++) slub_test_take(i, size, i + 70);
        SLUB_TEST_ASSERT(slab_pages == 2 && nr_free_pages() == test_baseline - 2);
        struct Page *first = test_live[0].head;
        struct slub_slab *slab = slub_validate_slab(first, 0);
        SLUB_TEST_ASSERT(slab->inuse == capacity);
        slub_check_invariants();
        slub_test_give(0);
        size_t before = nr_free_pages();
        if (capacity > 1) {
            slab = slub_validate_slab(first, 0);
            SLUB_TEST_ASSERT(slab->inuse == capacity - 1);
            slub_check_invariants();
        }
        slub_test_take(0, size, 600);
        // Any free slot is acceptable; a capacity-one slab was already reclaimed.
        SLUB_TEST_ASSERT(nr_free_pages() == before - (capacity == 1 ? 1 : 0));
        for (size_t i = 0; i < capacity + 1; i += 2) slub_test_give(i);
        for (size_t i = 1; i < capacity + 1; i += 2) slub_test_give(i);
        slub_test_finish_case();
    }
    cprintf("[slub test] cases 3/4 PASS\n");
}

static void
slub_test_mixed(void) {
    test_stage = "case 5: mixed data and reuse";
    cprintf("[slub test] %s (seed=20261024, 384 steps)\n", test_stage);
    const size_t sizes[] = {1,17,64,127,257,511,1024,2048,2049,4097,8193};
    unsigned int seed = 20261024;
    for (size_t step = 0; step < 384; step++) {
        seed = seed * 1664525U + 1013904223U;
        size_t slot = (seed >> 16) % 16;
        if (test_live[slot].ptr) slub_test_give(slot);
        else slub_test_take(slot, sizes[(seed >> 8) % (sizeof(sizes) / sizeof(sizes[0]))], step + 700);
    }
    slub_test_finish_case();
    cprintf("[slub test] case 5 PASS\n");
}

static void
slub_test_null_overflow(void) {
    test_stage = "case 6: null and overflow";
    cprintf("[slub test] %s\n", test_stage);
    slub_test_take(0, 31, 1500);
    size_t before = nr_free_pages(), old_slabs = slab_pages, old_large = large_pages;
    kfree(NULL);
    SLUB_TEST_ASSERT(kmalloc(0) == NULL);
    SLUB_TEST_ASSERT(kmalloc(~(size_t)0) == NULL);
    SLUB_TEST_ASSERT(kmalloc(~(size_t)0 - (PGSIZE - 2)) == NULL);
    SLUB_TEST_ASSERT(kmalloc(~(size_t)0 - (PGSIZE - 1)) == NULL);
    SLUB_TEST_ASSERT(kmalloc((slub_page_count + 1) * PGSIZE) == NULL);
    SLUB_TEST_ASSERT(nr_free_pages() == before && slab_pages == old_slabs && large_pages == old_large);
    slub_test_verify();
    slub_test_finish_case();
    cprintf("[slub test] case 6 PASS (OOM injection is a separate host case)\n");
}
#endif

void
slub_check(void) {
    slub_check_invariants();
#if defined(SLUB_TEST) && SLUB_TEST
    if (small_objects || large_objects) {
        cprintf("[slub test] SKIP functional cases: require an empty controlled SLUB context\n");
        return;
    }
    size_t before = nr_free_pages();
    struct Page *probe = alloc_pages(64);
    if (probe == NULL) {
        cprintf("[slub test] SKIP functional cases: cannot obtain a 64-page backing block\n");
        return;
    }
    free_pages(probe, 64);
    test_stage = "resource probe"; test_step = 0; test_baseline = before;
    SLUB_TEST_ASSERT(nr_free_pages() == before);
    for (size_t i = 0; i < SLUB_TEST_SLOTS; i++) SLUB_TEST_ASSERT(!test_live[i].ptr);
    slub_test_boundaries();
    slub_test_large();
    slub_test_multislab();
    slub_test_mixed();
    slub_test_null_overflow();
    cprintf("[slub test] case 7 PASS (native page records checked throughout)\n");
    SLUB_TEST_ASSERT(nr_free_pages() == before);
    cprintf("[slub test] ALL NORMAL PASS; baseline=%lu restored\n", before);
#endif
    slub_check_invariants();
    cprintf("slub_check: %lu slabs, %lu small objects, %lu large objects (%lu pages)\n",
            slab_pages, small_objects, large_objects, large_pages);
}
