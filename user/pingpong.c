#include "kernel/types.h"
#include "user/user.h"

// pingpong: 父子进程之间通过两条管道做双向通信
//   p2c : 父 -> 子
//   c2p : 子 -> 父
int main(int argc, char *argv[]) {
  int p2c[2], c2p[2];
  int pid, parent, child;

  if (pipe(p2c) < 0 || pipe(c2p) < 0) {
    fprintf(2, "pingpong: pipe failed\n");
    exit(1);
  }

  pid = fork();
  if (pid < 0) {
    fprintf(2, "pingpong: fork failed\n");
    exit(1);
  }

  if (pid == 0) {
    // ---- 子进程 ----
    // 只保留 p2c 的读端和 c2p 的写端，其余一律关闭
    close(p2c[1]);
    close(c2p[0]);

    // 从父进程处读回父进程的 pid
    // （xv6 没有 getppid()，所以父进程主动把自己的 pid 传过来）
    if (read(p2c[0], &parent, sizeof(parent)) != sizeof(parent)) {
      fprintf(2, "pingpong: child read failed\n");
      exit(1);
    }
    close(p2c[0]);

    printf("%d: received ping from pid %d\n", getpid(), parent);

    // 把自己的 pid 回传给父进程
    child = getpid();
    if (write(c2p[1], &child, sizeof(child)) != sizeof(child)) {
      fprintf(2, "pingpong: child write failed\n");
      exit(1);
    }
    close(c2p[1]);

    exit(0);
  }

  // ---- 父进程 ----
  close(p2c[0]);
  close(c2p[1]);

  parent = getpid();
  if (write(p2c[1], &parent, sizeof(parent)) != sizeof(parent)) {
    fprintf(2, "pingpong: parent write failed\n");
    exit(1);
  }
  // 写完立刻关闭写端，否则子进程的 read 不会结束
  close(p2c[1]);

  if (read(c2p[0], &child, sizeof(child)) != sizeof(child)) {
    fprintf(2, "pingpong: parent read failed\n");
    exit(1);
  }
  close(c2p[0]);

  printf("%d: received pong from pid %d\n", getpid(), child);

  wait(0);
  exit(0);
}
