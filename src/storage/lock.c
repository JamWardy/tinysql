// open, flock and getpid are POSIX, not C11, so glibc hides them under
// -std=c11 unless asked.
#define _DEFAULT_SOURCE

#include "lock.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

// path with ".lock" on the end, or NULL if memory ran out.
static char *make_lock_path(const char *path) {
    size_t length = strlen(path);
    char *lock_path = malloc(length + sizeof(".lock"));

    if (lock_path) {
        memcpy(lock_path, path, length);
        memcpy(lock_path + length, ".lock", sizeof(".lock"));
    }
    return lock_path;
}

// The holder's process ID, as it wrote it, for the message.
static void read_holder(int fd, char *pid, size_t size) {
    ssize_t length = pread(fd, pid, size - 1, 0);

    if (length <= 0) {
        snprintf(pid, size, "unknown");
        return;
    }
    pid[length] = '\0';
}

static void write_holder(int fd) {
    char pid[32];
    int length = snprintf(pid, sizeof(pid), "%ld\n", (long)getpid());

    // Only for the message another process shows, so failing doesn't matter.
    if (ftruncate(fd, 0) == 0) {
        (void)!pwrite(fd, pid, (size_t)length, 0);
    }
}

int database_lock(const char *path, DatabaseLock *lock, Error *error) {
    char *lock_path = make_lock_path(path);

    if (!lock_path) {
        return fail(error, ERROR_OUT_OF_MEMORY, "out of memory");
    }

    // The file is never deleted: a process that opened it just before the
    // delete could then lock it while another locks a new one.
    int fd = open(lock_path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        fail(error, ERROR_IO, "cannot open '%s': %s", lock_path,
             strerror(errno));
        free(lock_path);
        return 0;
    }
    free(lock_path);

    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        char holder[32];
        int busy = errno == EWOULDBLOCK;

        if (busy) {
            read_holder(fd, holder, sizeof(holder));
            holder[strcspn(holder, "\n")] = '\0';
            fail(error, ERROR_DATABASE_IN_USE,
                 "database '%s' is in use by process %s", path, holder);
        } else {
            fail(error, ERROR_IO, "cannot lock '%s': %s", path,
                 strerror(errno));
        }
        close(fd);
        return 0;
    }

    write_holder(fd);
    lock->fd = fd;
    return 1;
}

void database_unlock(DatabaseLock *lock) {
    close(lock->fd);  // Closing releases the flock.
    lock->fd = -1;
}
