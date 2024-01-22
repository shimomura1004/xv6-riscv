//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  argint(n, &fd);
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();

  // プロセスは NOFILE 個のファイルまで開ける
  // 開いたファイルの file 構造体は proc.ofile に記録されている
  for(fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd] == 0){
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
    return -1;
  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;
  
  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
    return -1;

  return filewrite(f, p, n);
}

uint64
sys_close(void)
{
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat

  argaddr(1, &st);
  if(argfd(0, 0, &f) < 0)
    return -1;
  return filestat(f, st);
}

// 存在するファイル(old)への新しいリンク(new)を作成する
// Create the path new as a link to the same inode as old.
uint64
sys_link(void)
{
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;

  // システムコールの文字列引数2つを old と new にコピー
  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;

  begin_op();
  if((ip = namei(old)) == 0){
    // old で指定されたファイルがない場合はエラー
    end_op();
    return -1;
  }

  ilock(ip);
  if(ip->type == T_DIR){
    // 元ファイルとして指定されたものがディレクトリだったらエラー
    iunlockput(ip);
    end_op();
    return -1;
  }

  // リンク数を増やす
  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  // 新しいリンクとして指定されたパスの親ディレクトリの ip を dp に入れ、
  // name には新しく作るファイル名を入れる
  if((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  // デバイスをまたいでハードリンクを作ることはできない
  // 同じデバイス内であればディレクトリに新しいエントリを追加
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  // 先頭の2エントリ分を飛ばし、ディレクトリ dp の inode のエントリ数だけループ
  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0)
      // inode 番号が入っているということは空ではない
      return 0;
  }
  return 1;
}

// 指定されたパスのリンクを削除し、リンク数が 0 になったらファイル自体も消す
uint64
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], path[MAXPATH];
  uint off;

  // 引数から削除するパスを path にコピー
  if(argstr(0, path, MAXPATH) < 0)
    return -1;

  begin_op();
  // まず削除したいパスの親ディレクトリと最後の要素の名前を入手
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  // 見つけた親ディレクトリの子要素に name という名前のものがなかったらエラー
  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    // ディレクトリを消す場合は、ディレクトリが空じゃないといけない
    iunlockput(ip);
    goto bad;
  }

  // de を 0 クリア
  memset(&de, 0, sizeof(de));
  // からっぽの dirent dp を、親ディレクトリの削除したいエントリのところ(off)に書き込み
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if(ip->type == T_DIR){
    // 消したものがディレクトリだったら、dp の nlink を減らす
    // todo: ディレクトリの nlink は子要素の数？
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  // 削除された inode (ip) のリンク数を減らす
  ip->nlink--;
  iupdate(ip);
  // iunlockput 内で iput するので、リンク数が 0 になったらここで truncate される
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

// 新しく inode に名前をつける(link は既に名前のついた inode に別名をつけるもの)
static struct inode*
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  // 新規にファイルを追加したい親ディレクトリ(dp)を探す
  if((dp = nameiparent(path, name)) == 0)
    return 0;

  ilock(dp);

  if((ip = dirlookup(dp, name, 0)) != 0){
    // 既に親ディレクトリ内に同じ名前のファイルがあった場合
    // create を呼び出したシステムコール(open, mkdir, mkdev)によって動作を変える
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      // open から呼ばれた場合、見つかった同名のエントリがファイルなら成功
      // よってそのまま見つかったエントリ(ip)を返す
      return ip;
    // そうでない場合(mkdir や mkdev のとき、open だけどファイルじゃなかったとき)はエラー
    iunlockput(ip);
    return 0;
  }

  // 同じ名前のファイルはなかったということなので、まず新しい未使用の inode を確保
  if((ip = ialloc(dp->dev, type)) == 0){
    iunlockput(dp);
    return 0;
  }

  ilock(ip);
  // todo: デバイスファイルじゃないときの major/minor とは？
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    // もし "." と ".." を参照数としてカウントしてしまうと 0 にならないので削除できなくなる
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      goto fail;
  }

  // 新しく用意したファイル(ip)を親ディレクトリに追加
  if(dirlink(dp, name, ip->inum) < 0)
    goto fail;

  if(type == T_DIR){
    // 新規に作ったのがディレクトリなら、親ディレクトリの参照数を増やす
    // now that success is guaranteed:
    dp->nlink++;  // for ".."
    iupdate(dp);
  }

  iunlockput(dp);

  return ip;

 fail:
  // something went wrong. de-allocate ip.
  ip->nlink = 0;
  iupdate(ip);
  iunlockput(ip);
  iunlockput(dp);
  return 0;
}

// いわゆる open だが、単純に create を使うだけではない
uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  // open 時のモードを omode に取り出す
  argint(1, &omode);
  // 開くファイルのパスをコピー
  if((n = argstr(0, path, MAXPATH)) < 0)
    return -1;

  begin_op();

  if(omode & O_CREATE){
    // CREATE フラグつきで open しようとした場合
    // 必ずファイルとして新規の inode に名前をつける
    // todo: なぜ固定で T_FILE?
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0){
      end_op();
      return -1;
    }
  } else {
    // CREATE フラグがない open の場合、既存のファイルを開く
    if((ip = namei(path)) == 0){
      end_op();
      return -1;
    }
    ilock(ip);
    // 指定されたパスの inode がディレクトリだった場合は
    // 読み取り専用として開いていなかったらエラー
    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  // ここまでで、なにかしらのファイルを正しく開けている

  // 開いたものがデバイスファイルの場合、メジャー番号が正しいことを確認
  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)){
    iunlockput(ip);
    end_op();
    return -1;
  }

  // ファイルを確保
  // 成功したら、さらにプロセスが開いたファイルとしてそれを登録
  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  if(ip->type == T_DEVICE){
    // デバイスを開いた場合は、file 構造体 f にメジャー番号も記録
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    // デバイスでないなら普通のファイルかディレクトリ、つまり inode
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  // 書き込み専用モードでなかったら読み取れる
  f->readable = !(omode & O_WRONLY);
  // 書き込み専用モードか、読み書きモードだったら書き込める
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  // TRUNC フラグがついていたら itrunc を呼ぶ、つまりファイルを削除する
  // todo: どういう状況？
  if((omode & O_TRUNC) && ip->type == T_FILE){
    itrunc(ip);
  }

  iunlock(ip);
  end_op();

  return fd;
}

// いわゆる mkdir
uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  begin_op();
  // 引数のパスをコピーし、create でディレクトリを作る
  // ディレクトリの場合は major/minor はどちらも 0
  if(argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

// いわゆる mknod
uint64
sys_mknod(void)
{
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  begin_op();
  // 作成するデバイスの major/minor 番号を取得
  argint(1, &major);
  argint(2, &minor);
  // 作成するデバイスのパスを取得
  if((argstr(0, path, MAXPATH)) < 0 ||
     (ip = create(path, T_DEVICE, major, minor)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();
  
  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(p->cwd);
  end_op();
  p->cwd = ip;
  return 0;
}

uint64
sys_exec(void)
{
  // argv はカーネル内で確保しているので、仮想アドレスと物理アドレスが同じかも
  // 配列自体はスタックに確保、中身は kalloc で確保
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  argaddr(1, &uargv);
  if(argstr(0, path, MAXPATH) < 0) {
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv)){
      goto bad;
    }
    if(fetchaddr(uargv+sizeof(uint64)*i, (uint64*)&uarg) < 0){
      goto bad;
    }
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    // 引数1つで1ページ使っている(システムコールの処理完了後すみやかに開放している)
    argv[i] = kalloc();
    if(argv[i] == 0)
      goto bad;
    if(fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = exec(path, argv);

  // 引数用に確保したページをすべて開放
  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

 bad:
  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

uint64
sys_pipe(void)
{
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  // パイプのファイルディスクリプタを返すためのポインタを取得
  argaddr(0, &fdarray);
  // パイプを確保、rf が読み取り用ファイル、wf が書き込み用ファイル
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  // pipealloc で確保した2つのファイル構造体にファイルディスクリプタを割り当て
  // fdalloc 内で、プロセス構造体の ofile に登録される
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    // 失敗したら片付けしてから終了
    if(fd0 >= 0)
      p->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  // copyout でカーネル空間(fd0)からユーザ空間(fdarray)にデータをコピー
  // 2つのディスクリプタ fd0 と fd1 を返す
  if(copyout(p->pagetable, fdarray, (char*)&fd0, sizeof(fd0)) < 0 ||
     copyout(p->pagetable, fdarray+sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0){
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}
