#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

// Global process table, defined in proc.c.
extern struct proc proc[];

uint64 sys_exit(void) {
  int n;
  if (argint(0, &n) < 0) return -1;
  exit(n);
  return 0;  // not reached
}

uint64 sys_getpid(void) { return myproc()->pid; }

uint64 sys_fork(void) { return fork(); }

uint64 sys_wait(void) {
  uint64 p;
  int flags;
  if (argaddr(0, &p) < 0) return -1;
  if (argint(1, &flags) < 0) return -1;
  return wait(p, flags);
}

uint64 sys_sbrk(void) {
  int addr;
  int n;

  if (argint(0, &n) < 0) return -1;
  addr = myproc()->sz;
  if (growproc(n) < 0) return -1;
  return addr;
}

uint64 sys_sleep(void) {
  int n;
  uint ticks0;

  if (argint(0, &n) < 0) return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while (ticks - ticks0 < n) {
    if (myproc()->killed) {
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64 sys_kill(void) {
  int pid;

  if (argint(0, &pid) < 0) return -1;
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64 sys_uptime(void) {
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

uint64 sys_rename(void) {
  char name[16];
  int len = argstr(0, name, MAXPATH);
  if (len < 0) {
    return -1;
  }
  struct proc *p = myproc();
  memmove(p->name, name, len);
  p->name[len] = '\0';
  return 0;
}

// Yield the CPU: print scheduling related information, then give up the
// CPU so the scheduler can pick another runnable process.
uint64 sys_yield(void) {
  struct proc *p = myproc();

  // The kernel thread context (p->context) that will be saved on the
  // upcoming context switch.
  exit_info("Save the context of the process to the memory region from address %p to %p\n",
            (uint64)&p->context, (uint64)&p->context + sizeof(struct context));

  // Current running process: pid and the user pc saved in its trapframe.
  exit_info("Current running process pid is %d and user pc is %p\n", p->pid,
            p->trapframe->epc);

  // Find the next RUNNABLE process by scanning the process table circularly,
  // starting right after the current process (mirrors scheduler()).
  int start = (int)(p - proc);
  for (int i = 1; i <= NPROC; i++) {
    struct proc *np = &proc[(start + i) % NPROC];
    acquire(&np->lock);
    if (np->state == RUNNABLE) {
      int pid = np->pid;
      uint64 epc = np->trapframe->epc;
      release(&np->lock);
      exit_info("Next runnable process pid is %d and user pc is %p\n", pid, epc);
      break;
    }
    release(&np->lock);
  }

  // Actually give up the CPU.
  yield();
  return 0;
}
