#ifndef _UNISTD_H
#define _UNISTD_H
#include <stddef.h>
#include <sys/types.h>
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 1
extern char **environ;
ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
int close(int fd);
off_t lseek(int fd, off_t off, int whence);
pid_t getpid(void);
pid_t getppid(void);
int chdir(const char *path);
char *getcwd(char *buf, size_t size);
int dup(int fd);
int dup2(int fd, int newfd);
int pipe(int fd[2]);
int isatty(int fd);
unsigned sleep(unsigned seconds);
int usleep(unsigned long usec);
int unlink(const char *path);
int rmdir(const char *path);
int access(const char *path, int mode);
void sync(void);
void _exit(int status) __attribute__((noreturn));
#endif
