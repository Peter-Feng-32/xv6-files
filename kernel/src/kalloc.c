// Physical memory allocator, intended to allocate
// memory for user processes, kernel stacks, page table pages,
// and pipe buffers. Allocates 4096-byte pages.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "spinlock.h"
#include "vm.h"

void freerange(void *vstart, void *vend);
extern char end[]; // first address after kernel loaded from ELF file
                   // defined by the kernel linker script in kernel.ld

// Number of extra references to a page (ie. ref count = 0: page has 1 reference or 0 references)
int ref_count[PHYSTOP / PGSIZE];

struct run
{
  struct run *next;
};

struct
{
  struct spinlock lock;
  int use_lock;
  struct run *freelist;
} kmem;

void inc_ref_count(int pa)
{
  if (kmem.use_lock)
    acquire(&kmem.lock);
  ref_count[pa / PGSIZE]++;
  // cprintf("Inc pa: %d to %d\n", pa, ref_count[pa / PGSIZE]);

  if (kmem.use_lock)
    release(&kmem.lock);
}
void dec_ref_count(int pa)
{
  if (kmem.use_lock)
    acquire(&kmem.lock);
  ref_count[pa / PGSIZE]--;
  if (kmem.use_lock)
    release(&kmem.lock);
}

int get_ref_count(int pa)
{
  // cprintf("Start of Get Ref Count\n");

  int count = 0;
  if (kmem.use_lock)
    acquire(&kmem.lock);
  // cprintf("Middle of Get Ref Count\n");
  // cprintf("%d", pa);
  count = ref_count[pa / PGSIZE];
  // cprintf("Second Middle of Get Ref Count\n");

  if (kmem.use_lock)
    release(&kmem.lock);
  // cprintf("End of Get Ref Count\n");
  return count;
}

// Initialization happens in two phases.
// 1. main() calls kinit1() while still using entrypgdir to place just
// the pages mapped by entrypgdir on free list.
// 2. main() calls kinit2() with the rest of the physical pages
// after installing a full page table that maps them on all cores.
void kinit1(void *vstart, void *vend)
{
  // Initialize reference counter to 0.
  memset(ref_count, 0, (PHYSTOP / PGSIZE) * sizeof(int));

  initlock(&kmem.lock, "kmem");
  kmem.use_lock = 0;
  freerange(vstart, vend);
}

void kinit2(void *vstart, void *vend)
{
  freerange(vstart, vend);
  kmem.use_lock = 1;
}

void freerange(void *vstart, void *vend)
{
  char *p;
  p = (char *)PGROUNDUP((uint)vstart);
  for (; p + PGSIZE <= (char *)vend; p += PGSIZE)
    kfree(p);
}
// PAGEBREAK: 21
//  Free the page of physical memory pointed at by v,
//  which normally should have been returned by a
//  call to kalloc().  (The exception is when
//  initializing the allocator; see kinit above.)
void kfree(char *v)
{
  struct run *r;

  if ((uint)v % PGSIZE || v < end || V2P(v) >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  uint pa = V2P(v);
  if (get_ref_count(pa) > 0)
  {
    dec_ref_count(pa);
    return;
  }
  memset(v, 1, PGSIZE);

  if (kmem.use_lock)
    acquire(&kmem.lock);
  r = (struct run *)v;
  r->next = kmem.freelist;
  kmem.freelist = r;
  if (kmem.use_lock)
    release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
char *
kalloc(void)
{
  struct run *r;

  if (kmem.use_lock)
    acquire(&kmem.lock);
  r = kmem.freelist;
  if (r)
    kmem.freelist = r->next;
  if (kmem.use_lock)
    release(&kmem.lock);
  return (char *)r;
}
