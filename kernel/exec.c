#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"

static int loadseg(pde_t *, uint64, struct inode *, uint, uint);

int flags2perm(int flags)
{
    int perm = 0;
    if(flags & 0x1)
      perm = PTE_X;
    if(flags & 0x2)
      perm |= PTE_W;
    return perm;
}

int
exec(char *path, char **argv)
{
  char *s, *last;
  int i, off;
  uint64 argc, sz = 0, sp, ustack[MAXARG], stackbase;
  struct elfhdr elf;
  struct inode *ip;
  struct proghdr ph;
  pagetable_t pagetable = 0, oldpagetable;
  struct proc *p = myproc();

  begin_op();

  // ファイルを開く
  if((ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);

  // Check ELF header
  if(readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;

  if(elf.magic != ELF_MAGIC)
    goto bad;

  // exec のタイミングで、このプロセス用にページテーブルを作る
  // (main の procinit 時にはページの確保はしていない)
  // まだアプリ用にメモリは確保しないが trampoline と trapframe はマップする
  if((pagetable = proc_pagetable(p)) == 0)
    goto bad;

  // プログラムヘッダの数だけループ(プログラムヘッダとセグメントが1対1対応する)
  // Load program into memory.
  for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
    // プログラムヘッダを読み込んだあと、おかしな内容になっていないか確認
    if(readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if(ph.type != ELF_PROG_LOAD)
      continue;
    if(ph.memsz < ph.filesz)
      goto bad;
    if(ph.vaddr + ph.memsz < ph.vaddr)
      goto bad;
    if(ph.vaddr % PGSIZE != 0)
      goto bad;

    uint64 sz1;
    // 元々サイズは 0 で、それを必要なサイズ(ph.vaddr + ph.memsz)まで広げる
    // vaddr はロード先の仮想アドレス、memsz はこのセグメントが使うサイズ
    // つまり、仮想アドレス 0 から、必要なすべての範囲に対してページを割り当てている
    // (xv6 ではアドレス 0 から text/data を配置するようになっているので適切)
    if((sz1 = uvmalloc(pagetable, sz, ph.vaddr + ph.memsz, flags2perm(ph.flags))) == 0)
      goto bad;
    sz = sz1;

    // ヘッダで指定されたところ(ph.vaddr)にセグメントをロードする
    if(loadseg(pagetable, ph.vaddr, ip, ph.off, ph.filesz) < 0)
      goto bad;
  }
  iunlockput(ip);
  end_op();
  ip = 0;
  // ↑ここまでで elf のロードは終わり

  p = myproc();
  uint64 oldsz = p->sz;

  // Allocate two pages at the next page boundary.
  // Make the first inaccessible as a stack guard.
  // Use the second as the user stack.

  // elf でロードしたサイズ(ページサイズ単位)を求めておき
  // この次のアドレスには stack guard を置く
  // その次にスタックを置く
  sz = PGROUNDUP(sz);
  uint64 sz1;
  // 2ページ分確保
  if((sz1 = uvmalloc(pagetable, sz, sz + 2*PGSIZE, PTE_W)) == 0)
    goto bad;
  // 追加した2ページ分を含めてサイズ sz を更新
  sz = sz1;
  // 直前の uvmalloc で確保した2ページのうち、下位の1ページをユーザアクセス不可にする
  // text/data が仮想アドレス 0 から始まっているから、
  // sz は仮想アドレスと同じ値になっていることに注意
  uvmclear(pagetable, sz-2*PGSIZE);
  // スタックポインタは確保したページの最上位アドレスに指定
  // (スタックは上位から下位に向かって、つまり stack guard の方向に伸びていく)
  sp = sz;
  // stack guard との境界を stackbase とする
  stackbase = sp - PGSIZE;

  // exec ではすべての引数は文字列で渡される
  // argc は渡されていない
  // 文字列の配列 argv は、末尾に null ポインタが渡されることになっている
  // Push argument strings, prepare rest of stack in ustack.
  for(argc = 0; argv[argc]; argc++) {
    if(argc >= MAXARG)
      goto bad;
    // 引数の文字数 + 1 だけスタックを消費
    sp -= strlen(argv[argc]) + 1;
    sp -= sp % 16; // riscv sp must be 16-byte aligned
    // もしスタックを使い切ったら異常終了
    if(sp < stackbase)
      goto bad;
    // カーネル空間からユーザ空間に引数をコピー
    // システムコールなので exec は supervisor モードで動いている
    if(copyout(pagetable, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    ustack[argc] = sp;
  }
  ustack[argc] = 0;

  // push the array of argv[] pointers.
  sp -= (argc+1) * sizeof(uint64);
  sp -= sp % 16;
  if(sp < stackbase)
    goto bad;
  if(copyout(pagetable, sp, (char *)ustack, (argc+1)*sizeof(uint64)) < 0)
    goto bad;

  // arguments to user main(argc, argv)
  // argc is returned via the system call return
  // value, which goes in a0.
  p->trapframe->a1 = sp;

  // Save program name for debugging.
  for(last=s=path; *s; s++)
    if(*s == '/')
      last = s+1;
  safestrcpy(p->name, last, sizeof(p->name));
    
  // Commit to the user image.
  oldpagetable = p->pagetable;
  p->pagetable = pagetable;
  p->sz = sz;
  p->trapframe->epc = elf.entry;  // initial program counter = main
  p->trapframe->sp = sp; // initial stack pointer
  proc_freepagetable(oldpagetable, oldsz);

  return argc; // this ends up in a0, the first argument to main(argc, argv)

 bad:
  if(pagetable)
    proc_freepagetable(pagetable, sz);
  if(ip){
    iunlockput(ip);
    end_op();
  }
  return -1;
}

// Load a program segment into pagetable at virtual address va.
// va must be page-aligned
// and the pages from va to va+sz must already be mapped.
// Returns 0 on success, -1 on failure.
static int
loadseg(pagetable_t pagetable, uint64 va, struct inode *ip, uint offset, uint sz)
{
  uint i, n;
  uint64 pa;

  for(i = 0; i < sz; i += PGSIZE){
    // ユーザプロセス用のページテーブルをたどり、
    // ロード先の仮想アドレスに対応するページの物理アドレスを探す
    pa = walkaddr(pagetable, va + i);
    if(pa == 0)
      panic("loadseg: address should exist");
    if(sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    // ページの大きさ分だけ elf からデータを読み、書き込む
    if(readi(ip, 0, (uint64)pa, offset+i, n) != n)
      return -1;
  }
  
  return 0;
}
