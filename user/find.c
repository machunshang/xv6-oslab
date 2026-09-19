#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fs.h"
#include "user/user.h"

// 返回 path 的最后一段（文件名部分），指向原字符串内部
char *basename(char *path) {
  char *p;

  for (p = path + strlen(path); p >= path && *p != '/'; p--)
    ;
  return p + 1;
}

// 在 path 这棵子树中查找名字为 target 的文件或目录，并打印其路径
void find(char *path, char *target) {
  char buf[512], *p;
  int fd;
  struct dirent de;
  struct stat st;

  if ((fd = open(path, 0)) < 0) {
    fprintf(2, "find: cannot open %s\n", path);
    return;
  }

  if (fstat(fd, &st) < 0) {
    fprintf(2, "find: cannot stat %s\n", path);
    close(fd);
    return;
  }

  // 名字匹配就打印。注意：文件和目录都要判断，
  // 否则 `find . <dir>` 这种"目标本身是目录"的情况会漏掉。
  if (strcmp(basename(path), target) == 0) printf("%s\n", path);

  // 目录还需要继续向下递归
  if (st.type == T_DIR) {
    // 先把 "path/" 准备好，后续把目录项名字拼接到 p 处
    if (strlen(path) + 1 + DIRSIZ + 1 > sizeof buf) {
      fprintf(2, "find: path too long\n");
      close(fd);
      return;
    }
    strcpy(buf, path);
    p = buf + strlen(buf);
    *p++ = '/';

    while (read(fd, &de, sizeof(de)) == sizeof(de)) {
      if (de.inum == 0) continue;  // 空目录项，跳过

      // de.name 是定长数组，不一定有 '\0'，这里手工补上
      memmove(p, de.name, DIRSIZ);
      p[DIRSIZ] = 0;

      // 递归的“防死循环”关键：跳过 . 和 ..
      if (strcmp(p, ".") == 0 || strcmp(p, "..") == 0) continue;

      find(buf, target);
    }
  }

  close(fd);
}

int main(int argc, char *argv[]) {
  if (argc != 3) {
    fprintf(2, "usage: find <path> <name>\n");
    exit(1);
  }

  find(argv[1], argv[2]);
  exit(0);
}
