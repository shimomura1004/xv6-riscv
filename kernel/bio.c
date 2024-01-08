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

struct {
  struct spinlock lock;
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct buf head;
} bcache;

void
binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache");

  // NBUF 個のバッファをリンクリストにつなげておく
  // Create linked list of buffers
  bcache.head.prev = &bcache.head;
  bcache.head.next = &bcache.head;
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    initsleeplock(&b->lock, "buffer");
    bcache.head.next->prev = b;
    bcache.head.next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  // bget が返すバッファは、ブロックに対して常にひとつ
  // bcache.lock は、どのブロックがキャッシュされているか？の不変量を守るためのロック
  acquire(&bcache.lock);

  // まずキャッシュされていないか探す(先頭に戻ってきたら終了)
  // Is the block already cached?
  for(b = bcache.head.next; b != &bcache.head; b = b->next){
    // もしキャッシュに見つかった場合は
    if(b->dev == dev && b->blockno == blockno){
      // このブロックを使っているプロセス数を増やして終了
      b->refcnt++;
      release(&bcache.lock);
      // 各ブロックキャッシュが保持する lock は、バッファされている内容の
      // 読み書きが矛盾しないようにするためのもの
      acquiresleep(&b->lock);
      return b;
    }
  }

  // キャッシュされているものが見つからなかった場合は、最近使われていないものを再利用する
  // 後ろの方(head.prev)から探していく
  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  for(b = bcache.head.prev; b != &bcache.head; b = b->prev){
    // 参照カウントが 0 なら使われていないということなので、これを再利用する
    if(b->refcnt == 0) {
      b->dev = dev;
      b->blockno = blockno;
      // valid を 0 にすると、bread がディスクから読み直す
      b->valid = 0;
      b->refcnt = 1;
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // 新たにブロックキャッシュを確保できないということは
  // 同時にディスクを読み書きするプロセスが多すぎるということ
  // panic せず sleep してもいいが、デッドロックするかも
  panic("bget: no buffers");
}

// 指定されたブロックのコピーをメモリ上で読み書きできるバッファを提供する
// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  // 返ってきたバッファが valid でなかったらディスクから読み直す
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// bread で得たバッファに行った修正をブロックに反映する
// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// bread で得たバッファを開放
// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  // brelse しないとブロックキャッシュのロックを開放しない(他プロセスが使えない)ので注意
  releasesleep(&b->lock);

  // キャッシュの使用状況を更新するのでロックを取る
  acquire(&bcache.lock);
  b->refcnt--;
  if (b->refcnt == 0) {
    // 誰も使わなくなったらリストの先頭に戻す
    // no one is waiting for it.
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    bcache.head.next->prev = b;
    bcache.head.next = b;
  }
  
  release(&bcache.lock);
}

// 参照カウントを増やし開放されないようにする
void
bpin(struct buf *b) {
  acquire(&bcache.lock);
  b->refcnt++;
  release(&bcache.lock);
}

void
bunpin(struct buf *b) {
  acquire(&bcache.lock);
  b->refcnt--;
  release(&bcache.lock);
}


