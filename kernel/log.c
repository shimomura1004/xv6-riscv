#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"

// Simple logging that allows concurrent FS system calls.
//
// ひとつのログトランザクションには複数の FS 関係のシステムコールが含まれている
// アクティブなシステムコールがなくなったタイミングでコミットされる
//
// A log transaction contains the updates of multiple FS system
// calls. The logging system only commits when there are
// no FS system calls active. Thus there is never
// any reasoning required about whether a commit might
// write an uncommitted system call's updates to disk.
//
// A system call should call begin_op()/end_op() to mark
// its start and end. Usually begin_op() just increments
// the count of in-progress FS system calls and returns.
// But if it thinks the log is close to running out, it
// sleeps until the last outstanding end_op() commits.
//
// The log is a physical re-do log containing disk blocks.
// The on-disk log format:
//   header block, containing block #s for block A, B, C, ...
//   block A
//   block B
//   block C
//   ...
// Log appends are synchronous.

// Contents of the header block, used for both the on-disk header block
// and to keep track in memory of logged block# before commit.
struct logheader {
  int n;
  int block[LOGSIZE];
};

struct log {
  struct spinlock lock;
  int start;
  int size;
  int outstanding; // how many FS sys calls are executing.
  int committing;  // in commit(), please wait.
  int dev;
  struct logheader lh;
};
struct log log;

static void recover_from_log(void);
static void commit();

void
initlog(int dev, struct superblock *sb)
{
  // sb には、readsb でブロック番号1から読んだスーパーブロックの中身(メタデータ)が入っている
  if (sizeof(struct logheader) >= BSIZE)
    panic("initlog: too big logheader");

  initlock(&log.lock, "log");
  // スーパーブロックに書かれたメタデータで変数を初期化
  log.start = sb->logstart;
  log.size = sb->nlog;
  log.dev = dev;
  // 電源断などでコミット済みのトランザクションが残っていた場合に備え、最初に復帰処理を実施
  recover_from_log();
}

// いったんログブロックに書き込んだデータを使って実際のデータ領域を更新していく
// 途中で電源が切れた場合は最初からやり直すことになるが実害はない
// グローバル変数の log は電源が切れるともちろん失われるが
// initlog->recover_from_log->read_head で、先頭ログブロックに格納された
// ログヘッダから復元されるので問題ない
// 引数の recovering は、通常の commit のとき 0 で、リカバリ中だと 1
// Copy committed blocks from log to their home location
static void
install_trans(int recovering)
{
  int tail;

  for (tail = 0; tail < log.lh.n; tail++) {
    // 読み込み元はログブロックの1番目(0オリジン)から順番に選ぶ
    struct buf *lbuf = bread(log.dev, log.start+tail+1); // read log block
    // 書き込み先は、ログヘッダに書かれている本当のブロック番号のブロック
    struct buf *dbuf = bread(log.dev, log.lh.block[tail]); // read dst
    memmove(dbuf->data, lbuf->data, BSIZE);  // copy block to dst
    bwrite(dbuf);  // write dst to disk
    if(recovering == 0)
      // 普通のコミット中の場合は、ブロックキャッシュが固定(bpin)されているので解除
      // リカバリ中の場合は起動直後にここに到達しており、ブロックキャッシュは固定していないので不要
      bunpin(dbuf);
    brelse(lbuf);
    brelse(dbuf);
  }
}

// Read the log header from disk into the in-memory log header
static void
read_head(void)
{
  struct buf *buf = bread(log.dev, log.start);
  struct logheader *lh = (struct logheader *) (buf->data);
  int i;
  log.lh.n = lh->n;
  for (i = 0; i < log.lh.n; i++) {
    log.lh.block[i] = lh->block[i];
  }
  brelse(buf);
}

// write_head が書き込む先は、log.start で指されたブロックただひとつ
// log.start はスーパーブロック(1番目のブロック)に書かれた sb.logstart が代入されている
// mkfs.c を見ると 2 になっている
// 実際、0番ブロックがブートブロック、1番ブロックがスーパーブロック
// 2番以降がログブロックになっている
// Write in-memory log header to disk.
// This is the true point at which the
// current transaction commits.
static void
write_head(void)
{
  // 先頭のログブロックのブロックキャッシュを取得
  struct buf *buf = bread(log.dev, log.start);
  // このブロックにはログヘッダを入れる
  struct logheader *hb = (struct logheader *) (buf->data);
  int i;
  // 必要な情報をキャッシュに書き込んでから
  hb->n = log.lh.n;
  for (i = 0; i < log.lh.n; i++) {
    hb->block[i] = log.lh.block[i];
  }
  // ストレージに書き出してキャッシュをリリース
  bwrite(buf);
  brelse(buf);
}

// 起動時に initlog から呼ばれ、コミットされたトランザクションが残っていたら処理する
static void
recover_from_log(void)
{
  read_head();
  install_trans(1); // if committed, copy from log to disk
  // このあたりは commit のときと同じ動き
  log.lh.n = 0;
  write_head(); // clear the log
}

// FS システムコールを呼ぶ前に呼ぶ
// outstanding 数がインクリメントされ、複数のプロセスからのブロックアクセスを統合できる
// called at the start of each FS system call.
void
begin_op(void)
{
  acquire(&log.lock);
  while(1){
    if(log.committing){
      // ログをコミット中(書き込み中)だったら待つ
      sleep(&log, &log.lock);
    } else if(log.lh.n + (log.outstanding+1)*MAXOPBLOCKS > LOGSIZE){
      // 現在書き込まれているログ数に加え、処理中(outstanding)の全プロセスが
      // 最大のブロック数まで書き込んだ場合の合計が最大値を超える場合
      // ログが多くなりすぎるかもしれないのでここで止める
      // this op might exhaust log space; wait for commit.
      sleep(&log, &log.lock);
    } else {
      // 処理中の(FS システムコールを呼んでいる)プロセス数をひとつ増やし、ロックを開放してから抜ける
      // あとで outstanding なプロセスが 0 になったらまとめて commit することになる
      log.outstanding += 1;
      release(&log.lock);
      break;
    }
  }
}

// begin_op の逆で、outstanding 数を減らす
// outstanding なプロセス数が 0 になったらコミットする
// called at the end of each FS system call.
// commits if this was the last outstanding operation.
void
end_op(void)
{
  int do_commit = 0;

  acquire(&log.lock);
  log.outstanding -= 1;
  if(log.committing)
    // コミットは自分しかできないはず、他の誰かがコミットを呼んでいるのであれば異常
    panic("log.committing");
  if(log.outstanding == 0){
    // ブロックキャッシュにアクセスしているプロセスがいなくなったのでコミットする(フラグを立てる)
    do_commit = 1;
    log.committing = 1;
  } else {
    // 処理中のプロセス数が減ったので begin_op で待っているプロセスがいたら起こす
    // begin_op() may be waiting for log space,
    // and decrementing log.outstanding has decreased
    // the amount of reserved space.
    wakeup(&log);
  }
  release(&log.lock);

  if(do_commit){
    // call commit w/o holding locks, since not allowed
    // to sleep with locks.
    commit();
    acquire(&log.lock);
    log.committing = 0;
    // begin_op にコミットを待っているプロセスがいたら起こす
    wakeup(&log);
    release(&log.lock);
  }
}

// ブロックキャッシュからいきなり本物のブロックに書き込まず
// 一度ログブロックと呼ばれる部分に書き出すところが重要
// これでトランザクションをアトミックにできる
// write_log が書き込む先は、log.start + 1 から順番に使われる
// ログブロックの最初のブロックはログヘッダが使っているので、1つ後ろから使う
// Copy modified blocks from cache to log.
static void
write_log(void)
{
  int tail;

  for (tail = 0; tail < log.lh.n; tail++) {
    // ログブロックのブロックキャッシュを先頭から順番に取得
    struct buf *to = bread(log.dev, log.start+tail+1); // log block
    // プロセスが書き込んで dirty になったブロックキャッシュを取得
    struct buf *from = bread(log.dev, log.lh.block[tail]); // cache block
    // プロセスが書き込んだブロックキャッシュから、ログブロックのブロックキャッシュに
    // メモリの内容をコピーする(まだメモリ上の操作だけで、実際にはディスクには書かれていない)
    memmove(to->data, from->data, BSIZE);
    // virtio ドライバにアクセスし、本当にディスクに書き込む
    bwrite(to);  // write the log
    // 使ったキャッシュを開放する
    brelse(from);
    brelse(to);
  }
}

static void
commit()
{
  if (log.lh.n > 0) {
    // ログが1つでもあったら実行

    // ログブロックの1番目(0オリジン)以降に、ブロックキャッシュを書き込む
    write_log();     // Write modified blocks from cache to log
    // ログブロックの0番目に、ログヘッダを書き込む
    write_head();    // Write header to disk -- the real commit
    // この時点ではログヘッダなどはディスクに書き込まれているが、実際のファイルは未変更
    // いったんログブロックに書き込んだデータを実際のファイルがあるブロックに書き込んでいく
    install_trans(0); // Now install writes to home locations
    // ログキャッシュは全部書き込んだので、クリア
    log.lh.n = 0;
    // ログキャッシュが空の状態で write_head することで先頭ログブロックに書かれたログを消す
    // これをやらないと、次に電源オンされたときに同じ処理を繰り返してしまう
    write_head();    // Erase the transaction from the log
  }
}

// Caller has modified b->data and is done with the buffer.
// Record the block number and pin in the cache by increasing refcnt.
// commit()/write_log() will do the disk write.
//
// log_write() replaces bwrite(); a typical use is:
//   bp = bread(...)
//   modify bp->data[]
//   log_write(bp)
//   brelse(bp)
void
log_write(struct buf *b)
{
  int i;

  acquire(&log.lock);
  if (log.lh.n >= LOGSIZE || log.lh.n >= log.size - 1)
    panic("too big a transaction");
  if (log.outstanding < 1)
    // トランザクションを開始していないのに log に書き込もうとしたら異常
    panic("log_write outside of trans");

  for (i = 0; i < log.lh.n; i++) {
    if (log.lh.block[i] == b->blockno)   // log absorption
      break;
  }
  log.lh.block[i] = b->blockno;
  if (i == log.lh.n) {  // Add new block to log?
    // 新しいブロックの場合はピン止めする
    bpin(b);
    log.lh.n++;
  }
  release(&log.lock);
}

