// File system implementation.  Five layers:
//   + Blocks: allocator for raw disk blocks.
//   + Log: crash recovery for multi-step updates.
//   + Files: inode allocator, reading, writing, metadata.
//   + Directories: inode with special contents (list of other inodes!)
//   + Names: paths like /usr/rtm/xv6/fs.c for convenient naming.
//
// This file contains the low-level file system manipulation
// routines.  The (higher-level) system call implementations
// are in sysfile.c.

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "file.h"

#define min(a, b) ((a) < (b) ? (a) : (b))
// there should be one superblock per disk device, but we run with
// only one device
struct superblock sb; 

// Read the super block.
static void
readsb(int dev, struct superblock *sb)
{
  struct buf *bp;

  bp = bread(dev, 1);
  memmove(sb, bp->data, sizeof(*sb));
  brelse(bp);
}

// Init fs
void
fsinit(int dev) {
  readsb(dev, &sb);
  if(sb.magic != FSMAGIC)
    panic("invalid file system");
  initlog(dev, &sb);
}

// ブロックを 0 クリアする
// Zero a block.
static void
bzero(int dev, int bno)
{
  struct buf *bp;

  bp = bread(dev, bno);
  memset(bp->data, 0, BSIZE);
  log_write(bp);
  brelse(bp);
}

// Blocks.

// データブロックをひとつ確保し、ブロック番号を返す
// Allocate a zeroed disk block.
// returns 0 if out of disk space.
static uint
balloc(uint dev)
{
  int b, bi, m;
  struct buf *bp;

  bp = 0;
  // BPB: Bitmap bits Per Block
  // ブロックひとつあたりのビット数(ブロックサイズ1024、1バイトは8ビットなので 8096 になる)
  // ビットマップブロック1つで 8096 個のブロックの使用状況を保持できるということ
  for(b = 0; b < sb.size; b += BPB){
    // ビットマップブロックごとに処理をしていく
    // b はブロック番号(8096 ずつ増えていく)
    // 候補のブロックの使用状況を持つビットマップブロックを取得
    bp = bread(dev, BBLOCK(b, sb));
    for(bi = 0; bi < BPB && b + bi < sb.size; bi++){
      // bi もブロック番号のカウンタ(0~8095)
      // なので今見ているブロック番号は (b + bi) になる
      // m は bi に対応するビット位置のマスク
      m = 1 << (bi % 8);
      if((bp->data[bi/8] & m) == 0){  // Is block free?
        // 0 なら使用可能なので、ビットセットして使用中にする
        bp->data[bi/8] |= m;  // Mark block in use.
        // 変更したビットマップブロックのキャッシュをピン止め(bp は bpin される)
        log_write(bp);
        // bpin 効果で、brelse しても bp の refcnt は 1 となりキャッシュは解放されない
        brelse(bp);
        // 新たに確保したブロックの中身を 0 でクリアする
        bzero(dev, b + bi);
        // 見つけたブロック番号を返す
        return b + bi;
      }
    }
    brelse(bp);
  }
  printf("balloc: out of blocks\n");
  return 0;
}

// Free a disk block.
static void
bfree(int dev, uint b)
{
  struct buf *bp;
  int bi, m;

  // 解放したいブロックの情報を保持するビットマップブロックをキャッシュに乗せる
  bp = bread(dev, BBLOCK(b, sb));
  // alloc のときと同じ方法でビット位置を決定
  bi = b % BPB;
  m = 1 << (bi % 8);
  if((bp->data[bi/8] & m) == 0)
    panic("freeing free block");
  // フラグクリア
  bp->data[bi/8] &= ~m;
  // ログ(トランザクション)に追加
  log_write(bp);
  brelse(bp);
}

// Inodes.
//
// An inode describes a single unnamed file.
// The inode disk structure holds metadata: the file's type,
// its size, the number of links referring to it, and the
// list of blocks holding the file's content.
//
// The inodes are laid out sequentially on disk at block
// sb.inodestart. Each inode has a number, indicating its
// position on the disk.
//
// The kernel keeps a table of in-use inodes in memory
// to provide a place for synchronizing access
// to inodes used by multiple processes. The in-memory
// inodes include book-keeping information that is
// not stored on disk: ip->ref and ip->valid.
//
// An inode and its in-memory representation go through a
// sequence of states before they can be used by the
// rest of the file system code.
//
// * Allocation: an inode is allocated if its type (on disk)
//   is non-zero. ialloc() allocates, and iput() frees if
//   the reference and link counts have fallen to zero.
//
// * Referencing in table: an entry in the inode table
//   is free if ip->ref is zero. Otherwise ip->ref tracks
//   the number of in-memory pointers to the entry (open
//   files and current directories). iget() finds or
//   creates a table entry and increments its ref; iput()
//   decrements ref.
//
// * Valid: the information (type, size, &c) in an inode
//   table entry is only correct when ip->valid is 1.
//   ilock() reads the inode from
//   the disk and sets ip->valid, while iput() clears
//   ip->valid if ip->ref has fallen to zero.
//
// * Locked: file system code may only examine and modify
//   the information in an inode and its content if it
//   has first locked the inode.
//
// Thus a typical sequence is:
//   ip = iget(dev, inum)
//   ilock(ip)
//   ... examine and modify ip->xxx ...
//   iunlock(ip)
//   iput(ip)
//
// ilock() is separate from iget() so that system calls can
// get a long-term reference to an inode (as for an open file)
// and only lock it for short periods (e.g., in read()).
// The separation also helps avoid deadlock and races during
// pathname lookup. iget() increments ip->ref so that the inode
// stays in the table and pointers to it remain valid.
//
// Many internal file system functions expect the caller to
// have locked the inodes involved; this lets callers create
// multi-step atomic operations.
//
// The itable.lock spin-lock protects the allocation of itable
// entries. Since ip->ref indicates whether an entry is free,
// and ip->dev and ip->inum indicate which i-node an entry
// holds, one must hold itable.lock while using any of those fields.
//
// An ip->lock sleep-lock protects all ip-> fields other than ref,
// dev, and inum.  One must hold ip->lock in order to
// read or write that inode's ip->valid, ip->size, ip->type, &c.

// inode のキャッシュ？
struct {
  struct spinlock lock;
  struct inode inode[NINODE];
} itable;

void
iinit()
{
  int i = 0;
  
  initlock(&itable.lock, "itable");
  for(i = 0; i < NINODE; i++) {
    initsleeplock(&itable.inode[i].lock, "inode");
  }
}

static struct inode* iget(uint dev, uint inum);

// Allocate an inode on device dev.
// Mark it as allocated by  giving it type type.
// Returns an unlocked but allocated and referenced inode,
// or NULL if there is no free inode.
struct inode*
ialloc(uint dev, short type)
{
  int inum;
  struct buf *bp;
  struct dinode *dip;

  for(inum = 1; inum < sb.ninodes; inum++){
    // inode ブロックを取得、1番から順番に見ていく
    // 今見ている inode 番号が含まれる inode ブロックを取得
    bp = bread(dev, IBLOCK(inum, sb));
    // IPB はブロック1つに含まれる inode の数
    // 今見ている inode 番号の場所を計算
    dip = (struct dinode*)bp->data + inum%IPB;
    if(dip->type == 0){  // a free inode
      // 全体を 0 クリアしたあと使用中に変更
      memset(dip, 0, sizeof(*dip));
      dip->type = type;
      // inode のエントリを変更したので、今編集した inode ブロックをトランザクションに入れる
      log_write(bp);   // mark it allocated on the disk
      brelse(bp);
      return iget(dev, inum);
    }
    brelse(bp);
  }
  printf("ialloc: no inodes\n");
  return 0;
}

// Copy a modified in-memory inode to disk.
// Must be called after every change to an ip->xxx field
// that lives on disk.
// Caller must hold ip->lock.
void
iupdate(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  // 指定された inode が含まれる inode ブロックを取得
  bp = bread(ip->dev, IBLOCK(ip->inum, sb));
  // 指定された inode のオフセットを加算し inode のエントリのポインタを取得
  dip = (struct dinode*)bp->data + ip->inum%IPB;
  // inode ブロックのブロックキャッシュを更新
  dip->type = ip->type;
  dip->major = ip->major;
  dip->minor = ip->minor;
  dip->nlink = ip->nlink;
  dip->size = ip->size;
  memmove(dip->addrs, ip->addrs, sizeof(ip->addrs));
  // ブロックの変更をトランザクションに追加
  log_write(bp);
  brelse(bp);
}

// Find the inode with number inum on device dev
// and return the in-memory copy. Does not lock
// the inode and does not read it from disk.
static struct inode*
iget(uint dev, uint inum)
{
  struct inode *ip, *empty;

  acquire(&itable.lock);

  // Is the inode already in the table?
  empty = 0;
  for(ip = &itable.inode[0]; ip < &itable.inode[NINODE]; ip++){
    // 指定した inode が登録されていないか線形探索する
    if(ip->ref > 0 && ip->dev == dev && ip->inum == inum){
      // 見つかったら参照数を増やしてそれを返す
      ip->ref++;
      release(&itable.lock);
      return ip;
    }
    // 使っていないエントリがあったら覚えておく
    if(empty == 0 && ip->ref == 0)    // Remember empty slot.
      empty = ip;
  }

  // inode に空きがなかったらどうしようもない
  // Recycle an inode entry.
  if(empty == 0)
    panic("iget: no inodes");

  // inode のエントリを準備して返す
  // この時点ではストレージへのアクセスはしていない
  ip = empty;
  ip->dev = dev;
  ip->inum = inum;
  ip->ref = 1;
  // まだディスクの中の inode を読んでいないので invalid にしておく
  ip->valid = 0;
  release(&itable.lock);

  return ip;
}

// Increment reference count for ip.
// Returns ip to enable ip = idup(ip1) idiom.
struct inode*
idup(struct inode *ip)
{
  acquire(&itable.lock);
  ip->ref++;
  release(&itable.lock);
  return ip;
}

// Lock the given inode.
// Reads the inode from disk if necessary.
void
ilock(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  if(ip == 0 || ip->ref < 1)
    panic("ilock");

  acquiresleep(&ip->lock);

  if(ip->valid == 0){
    // まだストレージから inode を読んでいなければ読み出し
    // 対応する inode ブロックを探して読み込み
    bp = bread(ip->dev, IBLOCK(ip->inum, sb));
    dip = (struct dinode*)bp->data + ip->inum%IPB;
    // 中身をメモリ上のエントリにコピー
    ip->type = dip->type;
    ip->major = dip->major;
    ip->minor = dip->minor;
    ip->nlink = dip->nlink;
    ip->size = dip->size;
    memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
    // inode ブロックの中身は変更しないので log_write は不要でリリースだけする
    brelse(bp);
    // ストレージの inode を読んだので valid にする
    ip->valid = 1;
    if(ip->type == 0)
      panic("ilock: no type");
  }
}

// Unlock the given inode.
void
iunlock(struct inode *ip)
{
  if(ip == 0 || !holdingsleep(&ip->lock) || ip->ref < 1)
    panic("iunlock");

  releasesleep(&ip->lock);
}

// Drop a reference to an in-memory inode.
// If that was the last reference, the inode table entry can
// be recycled.
// If that was the last reference and the inode has no links
// to it, free the inode (and its content) on disk.
// All calls to iput() must be inside a transaction in
// case it has to free the inode.
void
iput(struct inode *ip)
{
  acquire(&itable.lock);

  if(ip->ref == 1 && ip->valid && ip->nlink == 0){
    // 指定された inode の参照数が1であり、valid なとき
    // つまり、この iput により誰も使わなくなり、かつディスクから読み込んでいるとき
    // これを解放する
    // inode has no links and no other references: truncate and free.

    // ip->ref == 1 means no other process can have ip locked,
    // so this acquiresleep() won't block (or deadlock).
    acquiresleep(&ip->lock);

    release(&itable.lock);

    // ファイルを削除する(inode のサイズを0にすることでデータブロックを開放する)
    itrunc(ip);
    // type を変更し未使用にする(有効な type は T_DIR, T_FILE, T_DEVICE のどれか)
    ip->type = 0;
    // ふたたび更新(変更はマージされる)
    iupdate(ip);
    // メモリ上の inode のキャッシュも未使用にする
    ip->valid = 0;

    releasesleep(&ip->lock);

    acquire(&itable.lock);
  }

  ip->ref--;
  release(&itable.lock);
}

// Common idiom: unlock, then put.
void
iunlockput(struct inode *ip)
{
  iunlock(ip);
  iput(ip);
}

// Inode content
//
// The content (data) associated with each inode is stored
// in blocks on the disk. The first NDIRECT block numbers
// are listed in ip->addrs[].  The next NINDIRECT blocks are
// listed in block ip->addrs[NDIRECT].

// Return the disk block address of the nth block in inode ip.
// If there is no such block, bmap allocates one.
// returns 0 if out of disk space.
static uint
bmap(struct inode *ip, uint bn)
{
  uint addr, *a;
  struct buf *bp;

  if(bn < NDIRECT){
    // NDIRECT よりも小さいインデックスのブロックを要求された場合
    if((addr = ip->addrs[bn]) == 0){
      // 未確保なら新たにブロックを確保する
      addr = balloc(ip->dev);
      if(addr == 0)
        // 確保に失敗した場合
        return 0;
      // 確保できたらアドレスを入れる
      ip->addrs[bn] = addr;
    }
    return addr;
  }
  bn -= NDIRECT;

  if(bn < NINDIRECT){
    // NDIRECT に収まらない、後ろのほうのインデックスだった場合
    // まず INDIRECT なアドレス(実際にはブロック番号)を記憶するためのブロックを読み出す
    // Load indirect block, allocating if necessary.
    if((addr = ip->addrs[NDIRECT]) == 0){
      // INDIRECT 用のブロックがなかったらまずそれを確保
      // addr にはブロック番号が入る
      addr = balloc(ip->dev);
      if(addr == 0)
        return 0;
      ip->addrs[NDIRECT] = addr;
    }
    // INDIRECT 用のブロックを読み出し
    bp = bread(ip->dev, addr);
    a = (uint*)bp->data;
    // 指定されたインデックスに対応するブロックを読み出し
    if((addr = a[bn]) == 0){
      // まだ未確保なら確保
      addr = balloc(ip->dev);
      if(addr){
        a[bn] = addr;
        log_write(bp);
      }
    }
    brelse(bp);
    return addr;
  }

  // インデックスが大きすぎたら(NDIRECT + NINDIRECT を超えたら)エラー終了
  panic("bmap: out of range");
}

// Truncate inode (discard contents).
// Caller must hold ip->lock.
void
itrunc(struct inode *ip)
{
  int i, j;
  struct buf *bp;
  uint *a;

  for(i = 0; i < NDIRECT; i++){
    // この inode が保持している DIRECT なデータブロックを順番に解放していく
    // ip->addrs[i] にはブロック番号が入っている
    if(ip->addrs[i]){
      bfree(ip->dev, ip->addrs[i]);
      ip->addrs[i] = 0;
    }
  }

  // 最後のひとつ(addrs[NDIRECT])には INDIRECT なアドレスを覚えているブロックの番号が入っている
  if(ip->addrs[NDIRECT]){
    // もし ip->addrs[NDIRECT] が 0 じゃなかったら INDIRECT なデータブロックを持つということ
    // まずアドレスを覚えているブロックを読み出して
    bp = bread(ip->dev, ip->addrs[NDIRECT]);
    a = (uint*)bp->data;
    for(j = 0; j < NINDIRECT; j++){
      // INDIRECT なデータブロックを順番に解放して
      if(a[j])
        bfree(ip->dev, a[j]);
    }
    brelse(bp);
    // 最後にアドレスを覚えていたブロック自体も開放し、ブロック番号も 0 にクリアする
    bfree(ip->dev, ip->addrs[NDIRECT]);
    ip->addrs[NDIRECT] = 0;
  }

  ip->size = 0;
  iupdate(ip);
}

// Copy stat information from inode.
// Caller must hold ip->lock.
void
stati(struct inode *ip, struct stat *st)
{
  st->dev = ip->dev;
  st->ino = ip->inum;
  st->type = ip->type;
  st->nlink = ip->nlink;
  st->size = ip->size;
}

// Read data from inode.
// Caller must hold ip->lock.
// If user_dst==1, then dst is a user virtual address;
// otherwise, dst is a kernel address.
int
readi(struct inode *ip, int user_dst, uint64 dst, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  if(off > ip->size || off + n < off)
    // オフセットが大きすぎたり、読み込みサイズが大きすぎるときはエラー
    return 0;
  if(off + n > ip->size)
    // 読み込みサイズが終端を超えるときは縮める
    n = ip->size - off;

  // m は前回ループで読み込んだデータ数、読み込み位置をずらしながらループしている
  for(tot=0; tot<n; tot+=m, off+=m, dst+=m){
    // オフセットをブロックサイズで割り、何番目のブロックが必要かを計算
    // bmap でその位置のブロックのインデックスを取得
    uint addr = bmap(ip, off/BSIZE);
    if(addr == 0)
      break;
    // ブロックを読み出す
    bp = bread(ip->dev, addr);
    // 読み出し位置(offset)からブロックの終端までと
    // 残りの読み出しバイト数を比較し、小さい方を選ぶ
    // つまり、このブロックに対して読み取るバイト数 m を計算している
    m = min(n - tot, BSIZE - off%BSIZE);
    // データをデータブロックから読み出し
    if(either_copyout(user_dst, dst, bp->data + (off % BSIZE), m) == -1) {
      brelse(bp);
      tot = -1;
      break;
    }
    brelse(bp);
  }
  return tot;
}

// Write data to inode.
// Caller must hold ip->lock.
// If user_src==1, then src is a user virtual address;
// otherwise, src is a kernel address.
// Returns the number of bytes successfully written.
// If the return value is less than the requested n,
// there was an error of some kind.
int
writei(struct inode *ip, int user_src, uint64 src, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  if(off > ip->size || off + n < off)
    return -1;
  if(off + n > MAXFILE*BSIZE)
    // 書き込みサイズがファイルの最大サイズを超えるときはエラー
    return -1;

  for(tot=0; tot<n; tot+=m, off+=m, src+=m){
    // readi と同じでオフセット位置のデータブロックのインデックスを探す
    uint addr = bmap(ip, off/BSIZE);
    if(addr == 0)
      break;
    // データブロックを読み込む
    bp = bread(ip->dev, addr);
    // ブロック終端か、書き込むデータの末尾までの長さ m を計算
    m = min(n - tot, BSIZE - off%BSIZE);
    // コピー
    if(either_copyin(bp->data + (off % BSIZE), user_src, src, m) == -1) {
      brelse(bp);
      break;
    }
    log_write(bp);
    brelse(bp);
  }

  // ファイルサイズが伸びた場合
  if(off > ip->size)
    ip->size = off;

  // write the i-node back to disk even if the size didn't change
  // because the loop above might have called bmap() and added a new
  // block to ip->addrs[].
  iupdate(ip);

  return tot;
}

// Directories

int
namecmp(const char *s, const char *t)
{
  return strncmp(s, t, DIRSIZ);
}

// 指定されたディレクトリの中で指定された名前のファイルを探す
// poff には見つけたファイルのエントリのオフセットが入る
// Look for a directory entry in a directory.
// If found, set *poff to byte offset of entry.
struct inode*
dirlookup(struct inode *dp, char *name, uint *poff)
{
  uint off, inum;
  struct dirent de;

  if(dp->type != T_DIR)
    panic("dirlookup not DIR");

  // エントリを1つずつみていく
  for(off = 0; off < dp->size; off += sizeof(de)){
    // ディレクトリもデータブロックを使ってエントリを保持しているので、まずはそれを読む
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlookup read");
    if(de.inum == 0)
      // inode 番号が 0 のディレクトリは未使用
      continue;
    if(namecmp(name, de.name) == 0){
      // entry matches path element
      if(poff)
        // 探しているエントリを見つけたらそのオフセットを記録
        *poff = off;
      inum = de.inum;
      // iget は refcnt を増加させるので、もしディレクトリの中身を探している間に
      // 他のプロセスがエントリを削除したとしてもエントリはいきなり削除されず
      // 正しく動作できる
      return iget(dp->dev, inum);
    }
  }

  return 0;
}

// ディレクトリにエントリを追加する
// Write a new directory entry (name, inum) into the directory dp.
// Returns 0 on success, -1 on failure (e.g. out of disk blocks).
int
dirlink(struct inode *dp, char *name, uint inum)
{
  int off;
  struct dirent de;
  struct inode *ip;

  // Check that name is not present.
  if((ip = dirlookup(dp, name, 0)) != 0){
    iput(ip);
    return -1;
  }

  // Look for an empty dirent.
  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlink read");
    if(de.inum == 0)
      break;
  }

  // 変更したディレクトリエントリの部分を書き込む
  strncpy(de.name, name, DIRSIZ);
  de.inum = inum;
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    return -1;

  return 0;
}

// Paths

// Copy the next path element from path into name.
// Return a pointer to the element following the copied one.
// The returned path has no leading slashes,
// so the caller can check *path=='\0' to see if the name is the last one.
// If no name to remove, return 0.
//
// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
//
static char*
skipelem(char *path, char *name)
{
  char *s;
  int len;

  while(*path == '/')
    path++;
  if(*path == 0)
    return 0;
  s = path;
  while(*path != '/' && *path != 0)
    path++;
  len = path - s;
  if(len >= DIRSIZ)
    memmove(name, s, DIRSIZ);
  else {
    memmove(name, s, len);
    name[len] = 0;
  }
  while(*path == '/')
    path++;
  return path;
}

// パスを表す文字列を受け取って、指定されたファイルの inode を返す
// parent 引数が非 0 だったら、指定されたファイルの親ディレクトリの inode を返しつつ
// name にパスの最後の要素のファイル名をコピーする
// Look up and return the inode for a path name.
// If parent != 0, return the inode for the parent and copy the final
// path element into name, which must have room for DIRSIZ bytes.
// Must be called inside a transaction since it calls iput().
static struct inode*
namex(char *path, int nameiparent, char *name)
{
  struct inode *ip, *next;

  // 探索の起点を決める(ルートディレクトリかカレントディレクトリ)
  if(*path == '/')
    ip = iget(ROOTDEV, ROOTINO);
  else
    ip = idup(myproc()->cwd);

  // パス文字列の先頭からディレクトリ名をひとつずつ切り出していく
  while((path = skipelem(path, name)) != 0){
    ilock(ip);
    if(ip->type != T_DIR){
      // 今見ているエントリがディレクトリではなかったらエラー終了
      iunlockput(ip);
      return 0;
    }
    if(nameiparent && *path == '\0'){
      // 見ているのが末尾のエントリで、かつ親を探すモード(nameiparent)だったら終了
      // Stop one level early.
      iunlock(ip);
      return ip;
    }
    // skipelem によって切り出された先頭の名前を、ディレクトリのエントリから探す
    if((next = dirlookup(ip, name, 0)) == 0){
      // 見つからなかったらエラー終了
      iunlockput(ip);
      return 0;
    }
    iunlockput(ip);
    // 見つかったらディレクトリをひとつ潜ってループ
    ip = next;
  }
  // 最後まで探しきってループを抜けた場合
  if(nameiparent){
    // もし親を探していたら、ここにきてしまったらおかしい
    // 最初の skipelem がいきなり 0 を返したらここにくる
    // そのときは親がないのでエラー終了
    iput(ip);
    return 0;
  }
  return ip;
}

// 指定されたパスに対応するファイルの inode を返す
struct inode*
namei(char *path)
{
  char name[DIRSIZ];
  return namex(path, 0, name);
}

// 指定されたパスに対応するファイルの親ディレクトリの inode を返し
// パスの最後の要素のファイル名を name に入れる
struct inode*
nameiparent(char *path, char *name)
{
  return namex(path, 1, name);
}
