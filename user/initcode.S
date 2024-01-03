# Initial process that execs /init.
# This code runs in user space.

#include "syscall.h"

# exec(init, argv)
.globl start
start:
        # 引数を入れる
        # a0 と a1 に exec の引数を入れ、a7 にシステムコールの番号を入れる
        la a0, init
        la a1, argv
        li a7, SYS_exec
        # ecall はトラップを発生させるので uservec にジャンプする
        # uservec では引数を trapframe に退避し、ページテーブルを切り替えて
        # usertrap を実行する
        # usertrap では引数の確認などをしたあと syscall を実行する
        # syscall は、番号に応じて実際に実行する処理を変える
        ecall

# for(;;) exit();
exit:
        li a7, SYS_exit
        ecall
        jal exit

# char init[] = "/init\0";
init:
  .string "/init\0"

# char *argv[] = { init, 0 };
.p2align 2
argv:
  .long init
  .long 0
