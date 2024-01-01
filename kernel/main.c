#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

volatile static int started = 0;

// start.c/start から mret で main に "戻ってくる" ので、
// OS のメイン初期化処理はスーパーバイザモードで実行されることになる
// start() jumps here in supervisor mode on all CPUs.
void
main()
{
  // main にはすべての cpu はジャンプしてくるが、初期化処理を行うのは
  // cpuid が 0 のものひとつだけ
  if(cpuid() == 0){
    // コンソール(uart)を初期化
    consoleinit();
    // printf の初期化(ロック変数を初期化しているだけ)
    printfinit();
    printf("\n");
    printf("xv6 kernel is booting\n");
    printf("\n");

    // 物理メモリを freelist にすべてつなげる
    kinit();         // physical page allocator
    // デバイスやカーネルの動作に必要なページを登録する
    // ここまではページングが無効なので直接物理アドレスにアクセスできている
    kvminit();       // create kernel page table
    // この CPU のページングを有効にする
    kvminithart();   // turn on paging

    // プロセス構造体の初期化
    procinit();      // process table
    // トラップ用のロックの初期化だけ
    trapinit();      // trap vectors
    // トラップベクタ(stvec)を設定する
    trapinithart();  // install kernel trap vector

    // plic のレジスタを操作し割込みの優先度を設定
    plicinit();      // set up interrupt controller
    // 割込み許可レジスタを設定(RISC-V では CPU コアごとに設定される)
    plicinithart();  // ask PLIC for device interrupts

    binit();         // buffer cache
    iinit();         // inode table
    fileinit();      // file table
    virtio_disk_init(); // emulated hard disk
    userinit();      // first user process
    __sync_synchronize();
    started = 1;
  } else {
    while(started == 0)
      ;
    __sync_synchronize();
    printf("hart %d starting\n", cpuid());
    kvminithart();    // turn on paging
    trapinithart();   // install kernel trap vector
    plicinithart();   // ask PLIC for device interrupts
  }

  scheduler();        
}
