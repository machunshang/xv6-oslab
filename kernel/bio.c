// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

// Number of hash buckets.  A prime reduces collisions when block numbers
// visited by the workload follow a regular stride.
#define NBUCKET 13
#define HASH(blockno) ((blockno) % NBUCKET)

// One hash bucket: its own lock plus the head of a doubly linked list of
// buffers currently cached in this bucket.  head.next is the most recently
// used buffer, head.prev the least recently used one.
struct bucket {
  struct spinlock lock;
  struct buf head;
};

// bcache.lock[]
//   each bucket lock protects the membership, refcnt and list links of the
//   buffers hashed into that bucket.
// bcache.evict_lock
//   serializes the "miss -> pick a victim -> move it here" phase, which is
//   what guarantees that a block never gets two buffers in the cache.
struct {
  struct spinlock evict_lock;
  struct bucket bucket[NBUCKET];
  struct buf buf[NBUF];
} bcache;

static char bucket_lock_name[NBUCKET][10];

// caller must hold the lock of the bucket that contains b
static void
binsert(struct bucket *buk, struct buf *b)
{
  b->next = buk->head.next;
  b->prev = &buk->head;
  buk->head.next->prev = b;
  buk->head.next = b;
}

// caller must hold the lock of the bucket that contains b
static void
bremove(struct buf *b)
{
  b->prev->next = b->next;
  b->next->prev = b->prev;
  b->next = 0;
  b->prev = 0;
}

void
binit(void)
{
  struct buf *b;
  int i;

  initlock(&bcache.evict_lock, "bcache_evict");

  for(i = 0; i < NBUCKET; i++){
    // lock names must start with "bcache" to show up in the lock statistics
    snprintf(bucket_lock_name[i], sizeof(bucket_lock_name[i]), "bcache%d", i);
    initlock(&bcache.bucket[i].lock, bucket_lock_name[i]);
    bcache.bucket[i].head.prev = &bcache.bucket[i].head;
    bcache.bucket[i].head.next = &bcache.bucket[i].head;
  }

  // Spread the buffers over the buckets.  dev is set to an invalid value so
  // that no lookup can match an as-yet unused buffer.
  for(i = 0; i < NBUF; i++){
    b = &bcache.buf[i];
    initsleeplock(&b->lock, "buffer");
    b->dev = (uint)-1;
    b->blockno = i;
    b->valid = 0;
    b->refcnt = 0;
    b->next = 0;
    b->prev = 0;
    binsert(&bcache.bucket[i % NBUCKET], b);
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  struct buf *victim;
  struct bucket *buk;
  int h = HASH(blockno);

  buk = &bcache.bucket[h];

  // fast path: only the lock of the bucket the block belongs to.
  acquire(&buk->lock);
  for(b = buk->head.next; b != &buk->head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      bremove(b);
      binsert(buk, b);          // move to the front (most recently used)
      release(&buk->lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&buk->lock);

  // slow path: a miss.  Serialize with other misses, so that two CPUs can
  // never allocate two buffers for the same (dev, blockno).
  acquire(&bcache.evict_lock);

  // the block may have been cached by someone else in the meantime
  acquire(&buk->lock);
  for(b = buk->head.next; b != &buk->head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      bremove(b);
      binsert(buk, b);
      release(&buk->lock);
      release(&bcache.evict_lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // recycle the least recently used unused buffer of this bucket
  victim = 0;
  for(b = buk->head.prev; b != &buk->head; b = b->prev){
    if(b->refcnt == 0){
      victim = b;
      break;
    }
  }
  if(victim)
    bremove(victim);
  release(&buk->lock);

  // nothing free here: look in the other buckets, one lock at a time
  for(int i = 0; victim == 0 && i < NBUCKET; i++){
    struct bucket *o = &bcache.bucket[i];
    if(i == h)
      continue;
    acquire(&o->lock);
    for(b = o->head.prev; b != &o->head; b = b->prev){
      if(b->refcnt == 0){
        victim = b;
        break;
      }
    }
    if(victim)
      bremove(victim);
    release(&o->lock);
  }

  if(victim == 0)
    panic("bget: no buffers");

  // install the victim in the requested bucket
  acquire(&buk->lock);
  victim->dev = dev;
  victim->blockno = blockno;
  victim->valid = 0;
  victim->refcnt = 1;
  binsert(buk, victim);
  release(&buk->lock);

  release(&bcache.evict_lock);

  acquiresleep(&victim->lock);
  return victim;
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list of its bucket.
void
brelse(struct buf *b)
{
  struct bucket *buk;

  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  // b is still referenced here, so its blockno cannot change: safe to
  // compute the bucket without any lock.
  buk = &bcache.bucket[HASH(b->blockno)];

  acquire(&buk->lock);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    bremove(b);
    binsert(buk, b);
  }
  release(&buk->lock);
}

void
bpin(struct buf *b) {
  struct bucket *buk = &bcache.bucket[HASH(b->blockno)];
  acquire(&buk->lock);
  b->refcnt++;
  release(&buk->lock);
}

void
bunpin(struct buf *b) {
  struct bucket *buk = &bcache.bucket[HASH(b->blockno)];
  acquire(&buk->lock);
  b->refcnt--;
  release(&buk->lock);
}


