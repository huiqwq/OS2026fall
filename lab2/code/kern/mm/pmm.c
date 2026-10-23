#include <default_pmm.h>
#include <best_fit_pmm.h>
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
    pmm_manager = &best_fit_pmm_manager;
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

//lab2 function1 YOUR CODE
static void page_init(void) {
    extern char end[];

    // 设置虚实地址偏移量：物理地址 = 虚拟地址 - va_pa_offset
    va_pa_offset = PHYSICAL_MEMORY_OFFSET;

    // 从设备树获取物理内存信息
    uint64_t mem_base = get_memory_base();
    uint64_t mem_size = get_memory_size();
    uint64_t mem_end = mem_base + mem_size;

    // 设备树信息无效时终止初始化
    if (mem_base == 0 || mem_size == 0) {
        panic("page_init: invalid memory info from device tree\n");
    }

    // 输出探测到的物理内存范围
    cprintf("Physical memory map:\n");
    cprintf("  base: 0x%016lx, size: 0x%016lx, end: 0x%016lx\n",
            mem_base, mem_size, mem_end);

    // 可管理物理页总数（物理地址上界向下对齐到页边界）
    npage = mem_end / PGSIZE;

    // pages 描述符数组放在内核镜像 end 之后，并向上对齐到页边界
    pages = (struct Page *)ROUNDUP((uintptr_t)end, PGSIZE);

    // 先默认把所有物理页都标记为保留
    for (size_t i = 0; i < npage - nbase; i++) {
        SetPageReserved(pages + i);
    }

    // 描述符数组结束后的第一个空闲物理地址（向上对齐到页边界）
    uintptr_t freemem =
        PADDR((uintptr_t)pages + sizeof(struct Page) * (npage - nbase));
    freemem = ROUNDUP(freemem, PGSIZE);

    // 把 freemem 之后的空闲页交给分配器。
    // 注意：此时这些页仍是 reserved 状态——init_memmap 会先
    // assert(PageReserved) 再清空 flags，从而把它们变成空闲页；
    // 而内核镜像和描述符数组（< freemem 的页）保持 reserved，不进空闲区。
    if (freemem < mem_end) {
        init_memmap(pa2page(freemem), (mem_end - freemem) / PGSIZE);
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
