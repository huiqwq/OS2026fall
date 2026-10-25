#include <default_pmm.h>
#include <best_fit_pmm.h>
#include <buddy_pmm.h>
#include <defs.h>
#include <error.h>
#include <memlayout.h>
#include <mmu.h>
#include <pmm.h>
#include <sbi.h>
#include <stdio.h>
#include <string.h>
#include <riscv.h>
#include <dtb.h>

// virtual address of physical page array
struct Page *pages;
// amount of physical memory (in pages)
size_t npage = 0;
// the kernel image is mapped at VA=KERNBASE and PA=info.base
uint64_t va_pa_offset;
// memory starts at 0x80000000 in RISC-V
// DRAM_BASE defined in riscv.h as 0x80000000
const size_t nbase = DRAM_BASE / PGSIZE;

// virtual address of boot-time page directory
uintptr_t *satp_virtual = NULL;
// physical address of boot-time page directory
uintptr_t satp_physical;

// physical memory management
const struct pmm_manager *pmm_manager;


static void check_alloc_page(void);

// init_pmm_manager - initialize a pmm_manager instance
static void init_pmm_manager(void) {
#if defined(PMM_BUDDY) && !defined(PMM_BEST_FIT)
    pmm_manager = &buddy_pmm_manager;
#elif defined(PMM_BEST_FIT) && !defined(PMM_BUDDY)
    pmm_manager = &best_fit_pmm_manager;
#else
#error "Select exactly one physical memory manager through Makefile PMM"
#endif
    cprintf("memory management: %s\n", pmm_manager->name);
    pmm_manager->init();
}

// init_memmap - call pmm->init_memmap to build Page struct for free memory
static void init_memmap(struct Page *base, size_t n) {
    pmm_manager->init_memmap(base, n);
}

// alloc_pages - call pmm->alloc_pages to allocate a continuous n*PAGESIZE
// memory
struct Page *alloc_pages(size_t n) {
    return pmm_manager->alloc_pages(n);
}

// free_pages - call pmm->free_pages to free a continuous n*PAGESIZE memory
void free_pages(struct Page *base, size_t n) {
    pmm_manager->free_pages(base, n);
}

// nr_free_pages - call pmm->nr_free_pages to get the size (nr*PAGESIZE)
// of current free memory
size_t nr_free_pages(void) {
    return pmm_manager->nr_free_pages();
}

//lab2 function1 2413287
static void page_init(void) {
    extern char end[];

    // 设置虚实地址偏移量：物理地址 = 虚拟地址 - va_pa_offset
    va_pa_offset = PHYSICAL_MEMORY_OFFSET;

    // 从设备树获取物理内存信息
    uint64_t mem_base = get_memory_base();
    uint64_t mem_size = get_memory_size();

    // 设备树信息无效时终止初始化
    if (mem_base == 0 || mem_size == 0) {
        panic("page_init: invalid memory info from device tree\n");
    }
    if (mem_size > ~(uint64_t)0 - mem_base) {
        panic("page_init: physical memory range overflows\n");
    }
    uint64_t mem_end = mem_base + mem_size;
    // nbase 和启动映射使用固定 DRAM_BASE，不能描述其他起点的 RAM。
    if (mem_base != DRAM_BASE) {
        panic("page_init: unsupported memory base 0x%016lx\n", mem_base);
    }

    // 输出探测到的物理内存范围
    cprintf("Physical memory map:\n");
    cprintf("  base: 0x%016lx, size: 0x%016lx, end: 0x%016lx\n",
            mem_base, mem_size, mem_end);

    // npage 是物理页号上界；描述符和空闲区必须使用同一管理末端。
    uintptr_t managed_end = ROUNDDOWN(
        mem_end < PHYSICAL_MEMORY_END ? mem_end : PHYSICAL_MEMORY_END,
        PGSIZE);
    npage = managed_end / PGSIZE;
    if (npage <= nbase) {
        panic("page_init: no complete managed physical pages\n");
    }
    size_t page_count = npage - nbase;
    if (page_count > ~(size_t)0 / sizeof(struct Page)) {
        panic("page_init: page descriptor size overflows\n");
    }
    size_t descriptor_bytes = page_count * sizeof(struct Page);

    // pages 描述符数组放在内核镜像 end 之后，并向上对齐到页边界
    uintptr_t kernel_end = (uintptr_t)end;
    if (kernel_end < KERNBASE || kernel_end > ~(uintptr_t)0 - (PGSIZE - 1)) {
        panic("page_init: invalid kernel end\n");
    }
    uintptr_t pages_va = ROUNDUP(kernel_end, PGSIZE);
    uintptr_t pages_pa = PADDR(pages_va);
    // 在写入任何描述符之前，验证内核、对齐占用及整个数组均位于 RAM 内。
    if (pages_pa < mem_base || pages_pa > managed_end ||
        descriptor_bytes > managed_end - pages_pa) {
        panic("page_init: kernel and page descriptors do not fit in managed RAM\n");
    }
    if (descriptor_bytes > ~(uintptr_t)0 - pages_va) {
        panic("page_init: page descriptor address overflows\n");
    }
    uintptr_t descriptor_end_pa = pages_pa + descriptor_bytes;
    if (descriptor_end_pa > ~(uintptr_t)0 - (PGSIZE - 1)) {
        panic("page_init: free memory alignment overflows\n");
    }
    uintptr_t freemem = ROUNDUP(descriptor_end_pa, PGSIZE);
    pages = (struct Page *)pages_va;

    // 先默认把所有物理页都标记为保留
    for (size_t i = 0; i < page_count; i++) {
        SetPageReserved(pages + i);
    }

    // 把 freemem 之后的空闲页交给分配器。
    // 注意：此时这些页仍是 reserved 状态——init_memmap 会先
    // assert(PageReserved) 再清空 flags，从而把它们变成空闲页；
    // 而内核镜像和描述符数组（< freemem 的页）保持 reserved，不进空闲区。
    if (freemem < managed_end) {
        init_memmap(pa2page(freemem), (managed_end - freemem) / PGSIZE);
    }
}

/* pmm_init - initialize the physical memory management */
void pmm_init(void) {
    // We need to alloc/free the physical memory (granularity is 4KB or other size).
    // So a framework of physical memory manager (struct pmm_manager)is defined in pmm.h
    // First we should init a physical memory manager(pmm) based on the framework.
    // Then pmm can alloc/free the physical memory.
    // Now the first_fit/best_fit/worst_fit/buddy_system pmm are available.
    init_pmm_manager();

    // detect physical memory space 探测物理内存空间（搞清楚总共有多少物理内存）, 
    //reserve already used memory  把已经用掉的内存（比如内核自身、OpenSBI 等占用的部分）
    //标记为"已保留"，不能参与分配,
    // then use pmm->init_memmap to create free page list 
    // 然后用 pmm->init_memmap 把剩下的空闲物理页组织成空闲页链表（free list）
    page_init();

    // use pmm->check to verify the correctness of the alloc/free function in a pmm
    //check_alloc_page()是对物理内存分配功能的一个测试
    check_alloc_page();

    extern char boot_page_table_sv39[];
    satp_virtual = (pte_t*)boot_page_table_sv39;
    satp_physical = PADDR(satp_virtual);
    cprintf("satp virtual address: 0x%016lx\nsatp physical address: 0x%016lx\n", satp_virtual, satp_physical);
}

static void check_alloc_page(void) {
    pmm_manager->check();
    cprintf("check_alloc_page() succeeded!\n");
}
