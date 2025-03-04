#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;
  
  // xv6 ではプロセス数は NPROC で固定されており、管理領域も最初に準備される
  // すべてのプロセス用管理領域に対してループで初期化していく
  for(p = proc; p < &proc[NPROC]; p++) {
    // まずページテーブルを用意
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    // 仮想アドレスの後ろのほうに各プロセスのページテーブル用ページをマップする
    // それぞれのページの間に使っていないページが入っている
    // (KSTACK で 2*PGSIZE となっているので1ページずつ分ずつあいている)
    uint64 va = KSTACK((int) (p - proc));

    // カーネルのページテーブルにはすべてのプロセスのページテーブルが含まれているので
    // プロセス切り替え時の操作などが行える
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table.
void
procinit(void)
{
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  // すべてのプロセスに対してループ
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      // プロセス構造体を初期化するだけ
      // 特にスタック用のページの確保などはしない
      p->state = UNUSED;
      p->kstack = KSTACK((int) (p - proc));
  }
}

// ここで得た cpuid は、タイマ割込みによるコンテキストスイッチがあって
// 復帰後に実行される CPU が変わると食い違ってしまう
// なので cpuid を読んで使い終わるまでは割込みを無効にしないといけない
// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// myproc は、今カーネル処理を実行中の CPU コアで動いているプロセス構造体を返す
// そのときに割込みを無効にした上で mycpu を呼んでいる
// プロセス構造体は実行中の CPU が変わっても変化しないので割込みを有効化しても問題ない
// Return the current struct proc *, or zero if none.
struct proc*
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

// pid を1ずつインクリメントして返すだけ
int
allocpid()
{
  int pid;
  
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  // xv6 では最大プロセス数は固定、空きがなければ失敗する
  return 0;

found:
  // 空いていたプロセス構造体に pid を入れ、ステータスを更新
  p->pid = allocpid();
  p->state = USED;

  // trapframe は、トラップが発生した場合にレジスタを退避する領域
  // この時点ではまだマップされていない、少し下の proc_pagettable でマップされる
  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // ユーザ用に空のページテーブルを作り、trampoline と trapframe をマップ
  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// assembled from ../user/initcode.S
// od -t xC ../user/initcode
uchar initcode[] = {
  0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
  0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
  0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
  0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
  0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
  0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  // allocate one user page and copy initcode's instructions
  // and data into it.
  uvmfirst(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
}

// sbrk はこの関数を使って実装されている
// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint64 sz;
  // growproc を呼んだプロセスの proc 構造体を取得
  // ページを取得・開放し、proc 構造体に含まれるページテーブルを更新する
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    // サイズを増やす
    if((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
  } else if(n < 0){
    // サイズを減らす
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // 自分が終了するとき、自分が生成した子プロセスを init に託す
  // 子プロセスが終了を通知する先がいなくなってしまう(zombie のままになってしまう)ことを防ぐため
  // Give any children to init.
  reparent(p);

  // 先に親を起こしているが、既に wait_lock を取っており、
  // 途中で wait が割り込んでくることはないので大丈夫
  // Parent might be sleeping in wait().
  wakeup(p->parent);
  
  // これからプロセス構造体の状態を変更していく(一時的に不整合になる)のでロックを取る
  acquire(&p->lock);

  p->xstate = status;
  // exit するときはプロセスの状態は zombie になる
  // その後、親プロセスの wait　が zombie になっていることを認識して unused に変更する
  p->state = ZOMBIE;

  release(&wait_lock);

  // 自分の終了処理が終わったら他にやることはないので早々に CPU を手放す
  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      // 自分の子プロセスを探す
      if(pp->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        // 子プロセスが先に終了していた場合は zombie 状態になっている
        if(pp->state == ZOMBIE){
          // Found one.
          pid = pp->pid;
          // 子プロセスの終了コードを、引数として受け取った(ユーザ空間の)アドレスにコピー
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                  sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          // trapframe の開放など、プロセス構造体の開放処理を行う
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          // 終了済みの子プロセスがいたら、いったん return してしまう
          // そうしないと子プロセスの終了コードなどを返せなくなってしまうため
          // wait を呼ぶアプリ側では何度も wait する必要がある
          return pid;
        }
        release(&pp->lock);
      }
    }

    // 子プロセスが見つからずに上記ループを抜けていたら終わり
    // No point waiting if we don't have any children.
    if(!havekids || killed(p)){
      release(&wait_lock);
      return -1;
    }
    
    // ここにきたということは、子プロセスは見つかったが終了していなかったということ
    // よって子プロセスが終わるのを待つ
    // 待つのに使うのは自分のプロセス構造体のアドレス
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  
  c->proc = 0;
  for(;;){
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();

    // 全プロセスのうち runnable なものを順番に実行していく
    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if(p->state == RUNNABLE) {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        p->state = RUNNING;
        c->proc = p;
        // swtch を呼んでユーザプロセスに切り替え(しばらく戻ってこない)
        swtch(&c->context, &p->context);

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }
      release(&p->lock);
    }
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

  // intena は、割込みが有効かどうかを表すフラグ
  intena = mycpu()->intena;
  // 控えていたレジスタを復元してプロセスを切り替え
  swtch(&p->context, &mycpu()->context);

  // ここにはしばらく戻ってこない

  mycpu()->intena = intena;
  // スタックポインタも復元されるので、return すると、
  // 切り替え先のプロセスが sched を呼んだところに戻る
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  // 実行するプロセスを切り替えるためにロックを取る
  // プロセス構造体のデータを切り替えていくので、途中で他の CPU が同じプロセス構造体を
  // 操作しないようにしないといけない
  acquire(&p->lock);
  // 今まで実行中だったプロセスステータスを実行可能にして、sched で切り替え
  p->state = RUNNABLE;
  sched();
  // この release は、別プロセスで acquire したロックを手放すもの
  // このプロセス自身が切り替え前に取ったロックを開放するわけではない
  // (プロセスの切り替えのタイミングを邪魔されないようにするのがロックの目的)
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    first = 0;
    fsinit(ROOTDEV);
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();

  // sleep は while 文の中で呼び出すべき
  // 複数のプロセスが同じリソースを待つ場合、先に起きた他のプロセスが
  // リソースをすべて使う可能性があり、その場合は再び待つ必要があるため
  
  // セマフォの実装などを念頭に sleep と wakeup の実装を想定する
  //   V 操作はカウントアップし、P 操作でウェイトしているプロセスを起こす
  //   P 操作はカウントダウンするが、カウンタが 0 だったらスリープする
  // V 操作で、カウンタが 0 なのを見て sleep を呼ぼうとした瞬間に別 CPU で P 操作が行われた場合
  // 先に wakeup が呼ばれ、そのあと sleep が呼ばれてデッドロックしてしまう
  // (lost wake-up problem)
  // これは「カウンタが 0 のときだけ sleep する」という不変量が破れたことが原因
  // (排他が足りずカウンタが1以上のときに sleep してしまう可能性がある)
  // これを避けるために sleep の引数にロック(lk)を追加している

  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    // すべてのプロセスを見て、wakeup を呼んだプロセス以外のものに対して処理する
    if(p != myproc()){
      acquire(&p->lock);
      // 指定されたチャネルの入力待ちで sleep しているプロセスを runnable にする
      // runnable にするだけで、切り替えはしない(sched は呼ばない)
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    // kill の対象となるプロセスを探す
    if(p->pid == pid){
      // 具体的な終了処理をするわけではなく、プロセス構造体にフラグを立てるだけ
      // このプロセスは他の CPU でなにか重要な処理をしている可能性もあるので
      // 適当に kill するわけにはいかない
      // ここでフラグを立てておいて、その後 usertrap が呼ばれるタイミングで
      // 自発的に終了(exit を呼ぶ)することになっている
      // (そのため、スケジューラから時間をもらうために sleep から起こす必要がある)
      // usertrap はシステムコールを呼んだりタイマ割込みが入ると呼ばれる
      p->killed = 1;
      // 対象プロセスが wait していたらまず起こす
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int
killed(struct proc *p)
{
  int k;
  
  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    // ユーザ空間へのコピーの場合は copyout を使う
    return copyout(p->pagetable, dst, src, len);
  } else {
    // カーネル空間内のコピーの場合は memmove するだけ
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [USED]      "used",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}
