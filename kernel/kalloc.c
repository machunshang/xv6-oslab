// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

// One free list and one lock per CPU, so that kalloc()/kfree() running
// on different CPUs don't have to contend for a single global kmem.lock.
struct kmem {
  struct spinlock lock;
  struct run *freelist;
};

struct kmem kmems[NCPU];

// how many pages to grab when a CPU borrows from another CPU's list
#define STEAL_PAGES 64

static char kmem_lock_name[NCPU][8];

void
kinit()
{
  for(int c = 0; c < NCPU; c++){
    // lock names must start with "kmem" to show up in the lock statistics
    snprintf(kmem_lock_name[c], sizeof(kmem_lock_name[c]), "kmem%d", c);
    initlock(&kmems[c].lock, kmem_lock_name[c]);
    kmems[c].freelist = 0;
  }

  freerange(end, (void*)PHYSTOP);
}

// Called only from kinit() before other CPUs are running, so no locking
// is needed.  Pages are spread over the per-CPU lists round-robin so that
// no CPU starts with an empty list (which would cause useless stealing).
void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  struct run *r;
  struct kmem *k;
  static int nextcpu;

  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE){
    r = (struct run*)p;
    k = &kmems[nextcpu];
    r->next = k->freelist;
    k->freelist = r;
    nextcpu = (nextcpu + 1) % NCPU;
  }
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;
  struct kmem *k;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  // Free the page back to the list of the CPU running this code.
  // push_off() keeps the CPU from changing between cpuid() and the
  // insertion, so we always use the right list.
  push_off();
  k = &kmems[cpuid()];
  acquire(&k->lock);
  r->next = k->freelist;
  k->freelist = r;
  release(&k->lock);
  pop_off();
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;
  struct run *tail;
  struct run *rest;
  struct kmem *k;
  struct kmem *o;
  int id;
  int n;

  push_off();
  id = cpuid();
  k = &kmems[id];

  // first try the local list
  acquire(&k->lock);
  r = k->freelist;
  if(r)
    k->freelist = r->next;
  release(&k->lock);

  // local list empty: borrow a batch of pages from another CPU.
  // Only one lock is held at a time, so no lock-order deadlock.
  if(r == 0){
    for(int i = 1; i < NCPU && r == 0; i++){
      o = &kmems[(id + i) % NCPU];
      acquire(&o->lock);
      if(o->freelist){
        r = o->freelist;
        tail = r;
        n = 1;
        while(tail->next && n < STEAL_PAGES){
          tail = tail->next;
          n++;
        }
        rest = tail->next;
        tail->next = 0;
        o->freelist = rest;
      }
      release(&o->lock);

      if(r){
        // keep the rest of the batch on the local list, return one page
        if(r->next){
          acquire(&k->lock);
          tail->next = k->freelist;
          k->freelist = r->next;
          release(&k->lock);
          r->next = 0;
        }
      }
    }
  }
  pop_off();

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
