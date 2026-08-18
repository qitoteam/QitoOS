/*
 * Qira OS - system call interface
 *
 * Shared between the kernel and userspace: the numbers here are the ABI.
 */
#ifndef QIRA_SYSCALL_H
#define QIRA_SYSCALL_H

#include <kernel/types.h>

/* Process control */
#define SYS_EXIT         0
#define SYS_YIELD        1
#define SYS_GETPID       2
#define SYS_GETPPID      3
#define SYS_SLEEP_MS     4
#define SYS_SPAWN        5
#define SYS_WAITPID      6
#define SYS_KILL         7

/* File I/O */
#define SYS_OPEN         10
#define SYS_CLOSE        11
#define SYS_READ         12
#define SYS_WRITE        13
#define SYS_SEEK         14
#define SYS_STAT         15
#define SYS_READDIR      16
#define SYS_MKDIR        17
#define SYS_UNLINK       18
#define SYS_RENAME       19
#define SYS_TRUNCATE     20
#define SYS_CHDIR        21
#define SYS_GETCWD       22

/* Console and input */
#define SYS_PUTS         30
#define SYS_GETCHAR      31
#define SYS_POLL_EVENT   32

/* System information */
#define SYS_UPTIME_MS    40
#define SYS_TIME         41
#define SYS_SYSINFO      42
#define SYS_DMESG        43

/* Memory */
#define SYS_SBRK         50
#define SYS_MMAP         51

/* IPC */
#define SYS_MSG_SEND     60
#define SYS_MSG_RECV     61
#define SYS_PIPE         62

/* Graphics/desktop */
#define SYS_WIN_CREATE   70
#define SYS_WIN_DESTROY  71
#define SYS_WIN_PRESENT  72

#define SYS_MAX          80

/* Error numbers returned as negative values. */
#define QE_OK        0
#define QE_PERM      1
#define QE_NOENT     2
#define QE_IO        5
#define QE_BADF      9
#define QE_NOMEM     12
#define QE_ACCESS    13
#define QE_EXIST     17
#define QE_NOTDIR    20
#define QE_ISDIR     21
#define QE_INVAL     22
#define QE_NFILE     23
#define QE_MFILE     24
#define QE_FBIG      27
#define QE_NOSPC     28
#define QE_SPIPE     29
#define QE_ROFS      30
#define QE_NAMETOOLONG 36
#define QE_NOSYS     38
#define QE_NOTEMPTY  39
#define QE_AGAIN     11

const char *qira_strerror(int error);

#ifdef QIRA_KERNEL
struct interrupt_frame;
void syscall_init(void);
void syscall_dispatch(struct interrupt_frame *frame);
uint64_t syscall_count(uint32_t number);
#endif

#endif /* QIRA_SYSCALL_H */
