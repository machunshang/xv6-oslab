# Lab1 xv6 启动流程调试脚本
#
# 用法：
#   终端 1:  make qemu-gdb CPUS=1
#   终端 2:  make gdb
#   在 (gdb) 提示符下:  source commands.gdb
#
# 观察目标：第一个用户进程先运行 initcode，再通过 exec("/init") 变成 init。
# 说明：进程本身没有变，变的是这个进程里正在运行的程序。

# ---------- 1. start() -> main()：M 态切到 S 态 ----------
# kernel/start.c:58 就是 mret 指令
b start.c:58
c
p/x $mepc
p main

# ---------- 2. main() -> userinit()：创建第一个用户进程 ----------
b userinit
c
p cpus[$tp]->proc
finish

# ---------- 3. initcode 发起 exec("/init") 系统调用 ----------
b sys_exec
c
p cpus[$tp]->proc->name

# ---------- 4. exec 返回后：程序已经换成 init ----------
finish
p cpus[$tp]->proc->name
