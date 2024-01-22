#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"

#define PIPESIZE 512

struct pipe {
  struct spinlock lock;
  char data[PIPESIZE];
  uint nread;     // number of bytes read
  uint nwrite;    // number of bytes written
  int readopen;   // read fd is still open
  int writeopen;  // write fd is still open
};

// パイプを新しく作成し、引数としてもらった2つの引数の参照先に file 構造体のポインタを入れる
int
pipealloc(struct file **f0, struct file **f1)
{
  struct pipe *pi;

  pi = 0;
  *f0 = *f1 = 0;
  // ファイル構造体を2つ確保
  // 本物のファイルではないので inode の確保とかは不要
  if((*f0 = filealloc()) == 0 || (*f1 = filealloc()) == 0)
    goto bad;
  // メモリを1ページ分確保
  // ここでファイルをやりとりする
  if((pi = (struct pipe*)kalloc()) == 0)
    goto bad;
  // 最初はパイプは読み書きの療法ができる
  pi->readopen = 1;
  pi->writeopen = 1;
  pi->nwrite = 0;
  pi->nread = 0;
  initlock(&pi->lock, "pipe");
  // ひとつめの引数には読み取り用のファイル構造体を返す
  (*f0)->type = FD_PIPE;
  (*f0)->readable = 1;
  (*f0)->writable = 0;
  (*f0)->pipe = pi;
  // ふたつめの引数には書き込み用のファイル構造体を返す
  (*f1)->type = FD_PIPE;
  (*f1)->readable = 0;
  (*f1)->writable = 1;
  (*f1)->pipe = pi;
  return 0;

 bad:
  if(pi)
    kfree((char*)pi);
  if(*f0)
    fileclose(*f0);
  if(*f1)
    fileclose(*f1);
  return -1;
}

void
pipeclose(struct pipe *pi, int writable)
{
  acquire(&pi->lock);
  if(writable){
    pi->writeopen = 0;
    wakeup(&pi->nread);
  } else {
    pi->readopen = 0;
    wakeup(&pi->nwrite);
  }
  if(pi->readopen == 0 && pi->writeopen == 0){
    release(&pi->lock);
    kfree((char*)pi);
  } else
    release(&pi->lock);
}

int
pipewrite(struct pipe *pi, uint64 addr, int n)
{
  int i = 0;
  struct proc *pr = myproc();

  acquire(&pi->lock);
  while(i < n){
    if(pi->readopen == 0 || killed(pr)){
      release(&pi->lock);
      return -1;
    }
    if(pi->nwrite == pi->nread + PIPESIZE){ //DOC: pipewrite-full
      // バッファがいっぱいになってしまったら、読み取り待ちのプロセスを起こして sleep する
      wakeup(&pi->nread);
      sleep(&pi->nwrite, &pi->lock);
    } else {
      char ch;
      // 1文字ずつ読み取ってキューに入れていく
      if(copyin(pr->pagetable, &ch, addr + i, 1) == -1)
        break;
      pi->data[pi->nwrite++ % PIPESIZE] = ch;
      i++;
    }
  }
  // 書き終わったので、読み取り待ちのプロセスを起こす
  wakeup(&pi->nread);
  release(&pi->lock);

  return i;
}

int
piperead(struct pipe *pi, uint64 addr, int n)
{
  int i;
  struct proc *pr = myproc();
  char ch;

  // read/write で、同時にキューを操作できるのは1つだけ
  acquire(&pi->lock);
  // 書いたバイト数と読んだバイト数が同じならからっぽなので、sleep して待つ
  while(pi->nread == pi->nwrite && pi->writeopen){  //DOC: pipe-empty
    // いつのまにかプロセスが kill されてしまっていたら抜ける
    if(killed(pr)){
      release(&pi->lock);
      return -1;
    }
    // pi->lock を握ったまま sleep するとデッドロックする
    // sleep は、渡された pi->lock を開放してから休止状態にするので問題なし
    sleep(&pi->nread, &pi->lock); //DOC: piperead-sleep
  }
  // while を抜けてきたということはデータが入ってきたということ
  for(i = 0; i < n; i++){  //DOC: piperead-copy
    // データがなくなってしまったら抜ける
    if(pi->nread == pi->nwrite)
      break;
    ch = pi->data[pi->nread++ % PIPESIZE];
    // 1バイトずつコピー
    if(copyout(pr->pagetable, addr + i, &ch, 1) == -1)
      break;
  }
  // 読み終わったのでパイプがあいた状態
  // よって write 側でバッファがあくのを待っているプロセスを起こす
  wakeup(&pi->nwrite);  //DOC: piperead-wakeup
  release(&pi->lock);
  return i;
}
