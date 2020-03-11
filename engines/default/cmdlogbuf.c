/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 * arcus-memcached - Arcus memory cache server
 * Copyright 2019 JaM2in Co., Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/time.h>

#include "default_engine.h"
#ifdef ENABLE_PERSISTENCE
#include "cmdlogbuf.h"

/* FIXME: config log buffer size */
#define CMDLOG_BUFFER_SIZE (100 * 1024 * 1024) /* 100 MB */
#define CMDLOG_FLUSH_AUTO_SIZE (32 * 1024) /* 32 KB : see the nflush data type of log_FREQ */
#define CMDLOG_RECORD_MIN_SIZE 16          /* 8 bytes header + 8 bytes body */
#define CMDLOG_MAX_FILEPATH_LENGTH 255

#define ENABLE_DEBUG 0

/* log file state */
typedef struct _log_fstate {
    int       fd;
    bool      fsync_ongoing;
    size_t    size;
} log_FSTATE;

/* log file structure */
typedef struct _log_file {
    char       path[CMDLOG_MAX_FILEPATH_LENGTH+1];
    log_FSTATE curr;
    log_FSTATE next;
} log_FILE;

/* flush request structure */
typedef struct _log_freq {
    uint16_t  nflush;     /* amount of log buffer to flush */
    uint8_t   dual_write; /* flag of dual write */
    uint8_t   unused;
} log_FREQ;

/* log buffer structure */
typedef struct _log_buffer {
    /* log buffer */
    char       *data;   /* log buffer pointer */
    uint32_t    size;   /* log buffer size */
    uint32_t    head;   /* the head position in log buffer */
    uint32_t    tail;   /* the tail position in log buffer */
    int32_t     last;   /* the last position in log buffer */

    /* flush request queue */
    log_FREQ   *fque;   /* flush request queue pointer */
    uint32_t    fqsz;   /* flush request queue size */
    uint32_t    fbgn;   /* the queue index to begin flush */
    uint32_t    fend;   /* the queue index to end flush */
    int32_t     dw_end; /* the queue index to end dual write */
} log_BUFFER;

/* log flusher structure */
typedef struct _log_flusher {
    pthread_mutex_t  lock;
    pthread_cond_t   cond;
    bool             sleep;
    volatile uint8_t running;
    volatile bool    reqstop;
} log_FLUSHER;

/* log global structure */
struct log_global {
    log_FILE        log_file;
    log_BUFFER      log_buffer;
    log_FLUSHER     log_flusher;
    LogSN           nxt_write_lsn;
    LogSN           nxt_flush_lsn;
    LogSN           nxt_fsync_lsn;
    pthread_mutex_t log_write_lock;
    pthread_mutex_t log_flush_lock;
    pthread_mutex_t flush_lsn_lock;
    pthread_mutex_t fsync_lsn_lock;
    volatile bool   initialized;
};

/* global data */
static EXTENSION_LOGGER_DESCRIPTOR* logger = NULL;
static struct log_global log_gl;

/*
 * Static Functions
 */
/* FIXME: disk_byte_write is temporary function move to disk.c later */
/***************/
static ssize_t disk_byte_write(int fd, void *buf, size_t count)
{
    char   *bfptr = (char*)buf;
    ssize_t nleft = count;
    ssize_t nwrite;

    while (nleft > 0) {
        nwrite = write(fd, bfptr, nleft);
        if (nwrite == 0) break;
        if (nwrite <  0) {
            if (errno == EINTR) continue;
            return nwrite;
        }
        nleft -= nwrite;
        bfptr += nwrite;
    }
    return (count - nleft);
}

static int disk_open(const char *fname, int flags, int mode)
{
    int fd;
    while (1) {
        if ((fd = open(fname, flags, mode)) == -1) {
            if (errno == EINTR) continue;
        }
        break;
    }
    return fd;
}

static int disk_fsync(int fd)
{
    if (fsync(fd) != 0) {
        return -1;
    }
    return 0;
}

static int disk_close(int fd)
{
    while (1) {
        if (close(fd) != 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        break;
    }
    return 0;
}
/***************/

static void do_log_flusher_wakeup(log_FLUSHER *flusher)
{
    pthread_mutex_lock(&flusher->lock);
    if (flusher->sleep) {
        pthread_cond_signal(&flusher->cond);
    }
    pthread_mutex_unlock(&flusher->lock);
}

static void do_log_file_sync(int fd, bool close)
{
    int ret = disk_fsync(fd);
    if (ret < 0) {
        logger->log(EXTENSION_LOG_WARNING, NULL,
                    "log file fsync error (%d:%s)\n",
                    errno, strerror(errno));
        /* [FATAL] untreatable error => abnormal shutdown by assertion */
    }
    assert(ret == 0);

    if (close) {
        ret = disk_close(fd);
        if (ret < 0) {
            logger->log(EXTENSION_LOG_WARNING, NULL,
                        "log file close error (%d:%s)\n",
                        errno, strerror(errno));
            /* [FATAL] untreatable error => abnormal shutdown by assertion */
        }
        assert(ret == 0);
    }
}

static void do_log_file_write(char *log_ptr, uint32_t log_size, bool dual_write)
{
    log_FILE *logfile = &log_gl.log_file;
    assert(logfile->curr.fd != -1);

    /* The log data is appended */
    ssize_t nwrite = disk_byte_write(logfile->curr.fd, log_ptr, log_size);
    if (nwrite != log_size) {
        logger->log(EXTENSION_LOG_WARNING, NULL,
                    "log file(%d) write - write(%ld!=%ld) error=(%d:%s)\n",
                    logfile->curr.fd, nwrite, (ssize_t)log_size,
                    errno, strerror(errno));
    }
    /* FIXME::need error handling */
    assert(nwrite == log_size);
    logfile->curr.size += log_size;

    if (dual_write && logfile->next.fd != -1) {
        /* next_fd is guaranteed concurrency by log_flush_lock */

        /* The log data is appended */
        nwrite = disk_byte_write(logfile->next.fd, log_ptr, log_size);
        if (nwrite != log_size) {
            logger->log(EXTENSION_LOG_WARNING, NULL,
                        "log file(%d) write - write(%ld!=%ld) error=(%d:%s)\n",
                        logfile->next.fd, nwrite, (ssize_t)log_size,
                        errno, strerror(errno));
        }
        /* FIXME::need error handling */
        assert(nwrite == log_size);
        logfile->next.size += log_size;
    }
}

static uint32_t do_log_buff_flush(bool flush_all)
{
    log_BUFFER *logbuff = &log_gl.log_buffer;
    uint32_t    nflush = 0;
    bool        dual_write_flag = false;
    bool        next_fhlsn_flag = false;
    bool        cleanup_process = false;

    /* computate flush size */
    pthread_mutex_lock(&log_gl.log_write_lock);
    if (logbuff->dw_end != -1) {
        cleanup_process = true;
    }
    if (logbuff->fbgn == logbuff->dw_end) {
        logbuff->dw_end = -1;
        next_fhlsn_flag = true;
    }
    if (logbuff->fbgn != logbuff->fend) {
        nflush = logbuff->fque[logbuff->fbgn].nflush;
        dual_write_flag = logbuff->fque[logbuff->fbgn].dual_write;
        assert(nflush > 0);
    } else {
        if (flush_all && logbuff->fque[logbuff->fend].nflush > 0) {
            nflush = logbuff->fque[logbuff->fend].nflush;
            dual_write_flag = logbuff->fque[logbuff->fend].dual_write;
            if ((++logbuff->fend) == logbuff->fqsz) logbuff->fend = 0;
        }
    }
    if (nflush > 0) {
        if (logbuff->head == logbuff->last) {
            logbuff->last = -1;
            logbuff->head = 0;
        }
    }
    pthread_mutex_unlock(&log_gl.log_write_lock);

    if (next_fhlsn_flag) {
        pthread_mutex_lock(&log_gl.flush_lsn_lock);
        log_gl.nxt_flush_lsn.filenum += 1;
        log_gl.nxt_flush_lsn.roffset = 0;
        pthread_mutex_unlock(&log_gl.flush_lsn_lock);
    }

    if (nflush > 0) {
        if (cleanup_process) {
            /* Cleanup process. (fd was set to next_fd in the previous step)
             * Skip if requested by old cmdlog file only.
             */
            if (dual_write_flag) {
                do_log_file_write(&logbuff->data[logbuff->head], nflush, false);
            }
        } else {
            do_log_file_write(&logbuff->data[logbuff->head], nflush, dual_write_flag);
        }

        /* update nxt_flush_lsn */
        pthread_mutex_lock(&log_gl.flush_lsn_lock);
        log_gl.nxt_flush_lsn.roffset += nflush;
        pthread_mutex_unlock(&log_gl.flush_lsn_lock);

        /* update next flush position */
        pthread_mutex_lock(&log_gl.log_write_lock);
        logbuff->head += nflush;
        if (logbuff->head == logbuff->last) {
            logbuff->last = -1;
            logbuff->head = 0;
        }
        /* clear the flush request itself */
        logbuff->fque[logbuff->fbgn].nflush = 0;
        logbuff->fque[logbuff->fbgn].dual_write = false;
        if ((++logbuff->fbgn) == logbuff->fqsz) logbuff->fbgn = 0;
        pthread_mutex_unlock(&log_gl.log_write_lock);
    }
    return nflush;
}

static void do_log_buff_write(LogRec *logrec, log_waiter_t *waiter, bool dual_write)
{
    log_BUFFER *logbuff = &log_gl.log_buffer;
    uint32_t total_length = sizeof(LogHdr) + logrec->header.body_length;
    uint32_t spare_length;
    assert(total_length < logbuff->size);

    pthread_mutex_lock(&log_gl.log_write_lock);

    if (waiter != NULL) {
        waiter->lsn = log_gl.nxt_write_lsn;
    }

    /* find the positon to write in log buffer */
    while (1) {
        if (logbuff->head <= logbuff->tail) {
            assert(logbuff->last == -1);
            /* logbuff->head == logbuff->tail: empty state (NO full state) */
            if (total_length < (logbuff->size - logbuff->tail)) {
                break; /* enough buffer space */
            }
            if (logbuff->head > 0) {
                logbuff->last = logbuff->tail;
                logbuff->tail = 0;
                /* increase log flush end pointer
                 * to make to-be-flushed log data contiguous in memory.
                 */
                if (logbuff->fque[logbuff->fend].nflush > 0) {
                    if ((++logbuff->fend) == logbuff->fqsz) logbuff->fend = 0;
                }
                if (total_length < logbuff->head) {
                    break; /* enough buffer space */
                }
            }
        } else { /* logbuff->head > logbuff->tail */
            assert(logbuff->last != -1);
            if (total_length < (logbuff->head - logbuff->tail)) {
                break; /* enough buffer space */
            }
        }
        /* Lack of log buffer space: force flushing data on log buffer */
        pthread_mutex_unlock(&log_gl.log_write_lock);
        pthread_mutex_lock(&log_gl.log_flush_lock);
        (void)do_log_buff_flush(false);
        pthread_mutex_unlock(&log_gl.log_flush_lock);
        pthread_mutex_lock(&log_gl.log_write_lock);
    }

    /* write log record at the found location of log buffer */
    lrec_write_to_buffer(logrec, &logbuff->data[logbuff->tail]);
    logbuff->tail += total_length;

    /* update nxt_write_lsn */
    log_gl.nxt_write_lsn.roffset += total_length;

    /* update log flush reqeust */
    if (logbuff->fque[logbuff->fend].nflush > 0 &&
        logbuff->fque[logbuff->fend].dual_write != dual_write) {
        if ((++logbuff->fend) == logbuff->fqsz) logbuff->fend = 0;
    }
    while (total_length > 0) {
        /* check remain length */
        spare_length = CMDLOG_FLUSH_AUTO_SIZE - logbuff->fque[logbuff->fend].nflush;
        if (spare_length >= total_length) spare_length = total_length;

        logbuff->fque[logbuff->fend].nflush += spare_length;
        logbuff->fque[logbuff->fend].dual_write = dual_write;
        if (logbuff->fque[logbuff->fend].nflush == CMDLOG_FLUSH_AUTO_SIZE) {
            if ((++logbuff->fend) == logbuff->fqsz) logbuff->fend = 0;
        }
        total_length -= spare_length;
    }

    pthread_mutex_unlock(&log_gl.log_write_lock);

    /* wake up log flush thread if flush requests exist */
    if (logbuff->fbgn != logbuff->fend) {
        if (log_gl.log_flusher.sleep == true) {
            do_log_flusher_wakeup(&log_gl.log_flusher);
        }
    }
}

/* Log Flush Thread */
static void *log_flush_thread_main(void *arg)
{
    log_FLUSHER *flusher = &log_gl.log_flusher;
    struct timeval  tv;
    struct timespec to;
    uint32_t nflush;

    flusher->running = RUNNING_STARTED;
    while (1)
    {
        if (flusher->reqstop) {
            logger->log(EXTENSION_LOG_INFO, NULL, "Command log flush thread recognized stop request.\n");
            break;
        }

        pthread_mutex_lock(&log_gl.log_flush_lock);
        nflush = do_log_buff_flush(false);
        pthread_mutex_unlock(&log_gl.log_flush_lock);

        if (nflush == 0) {
            /* nothing to flush: do 10 ms sleep */
            gettimeofday(&tv, NULL);
            if ((tv.tv_usec + 10000) < 1000000) {
                tv.tv_usec += 10000;
                to.tv_sec  = tv.tv_sec;
                to.tv_nsec = tv.tv_usec * 1000;
            } else {
                to.tv_sec  = tv.tv_sec + 1;
                to.tv_nsec = 0;
            }
            pthread_mutex_lock(&flusher->lock);
            flusher->sleep = true;
            pthread_cond_timedwait(&flusher->cond, &flusher->lock, &to);
            flusher->sleep = false;
            pthread_mutex_unlock(&flusher->lock);
        }
    }
    flusher->running = RUNNING_STOPPED;
    return NULL;
}

/*
 * External Functions
 */
void log_file_sync(void)
{
    LogSN now_flush_lsn;
    int  fd      = -1;
    int  next_fd = -1;

    pthread_mutex_lock(&log_gl.log_flush_lock);
    /* get current nxt_flush_lsn */
    log_get_flush_lsn(&now_flush_lsn);

    fd      = log_gl.log_file.curr.fd;
    next_fd = log_gl.log_file.next.fd;
    log_gl.log_file.curr.fsync_ongoing = true;
    if (next_fd != -1) {
        log_gl.log_file.next.fsync_ongoing = true;
    }
    pthread_mutex_unlock(&log_gl.log_flush_lock);

    assert(fd != -1);

    /* fsync the log files */
    do_log_file_sync(fd, false); /* do not close */
    if (next_fd != -1) {
        do_log_file_sync(next_fd, false); /* do not close */
    }

    /* update nxt_fsync_lsn */
    pthread_mutex_lock(&log_gl.fsync_lsn_lock);
    log_gl.nxt_fsync_lsn = now_flush_lsn;
    pthread_mutex_unlock(&log_gl.fsync_lsn_lock);

    pthread_mutex_lock(&log_gl.log_flush_lock);
    if (fd == log_gl.log_file.curr.fd) {
        log_gl.log_file.curr.fsync_ongoing = false;
    } else {
        /* fd is not used anymore */
        (void)disk_close(fd);
    }
    if (next_fd != -1) {
        if (next_fd == log_gl.log_file.curr.fd) {
            log_gl.log_file.curr.fsync_ongoing = false;
        } else if (next_fd == log_gl.log_file.next.fd) {
            log_gl.log_file.next.fsync_ongoing = false;
        } else {
            /* next_fd is not used anymore */
            (void)disk_close(next_fd);
        }
    }
    pthread_mutex_unlock(&log_gl.log_flush_lock);
}

void log_buffer_flush(LogSN *upto_lsn)
{
    assert(upto_lsn);
    uint32_t nflush;

    do {
        pthread_mutex_lock(&log_gl.log_flush_lock);
        if (LOGSN_IS_LE(&log_gl.nxt_flush_lsn, upto_lsn)) {
            nflush = do_log_buff_flush(true);
            assert(nflush > 0);
            if (LOGSN_IS_GT(&log_gl.nxt_flush_lsn, upto_lsn)) {
                nflush = 0;
            }
        } else {
            nflush = 0;
        }
        pthread_mutex_unlock(&log_gl.log_flush_lock);
    } while (nflush > 0);
}

void log_record_write(LogRec *logrec, log_waiter_t *waiter, bool dual_write)
{
    /* write the log record on the log buffer */
    do_log_buff_write(logrec, waiter, dual_write);
}

/* FIXME: remove later, if not used */
/*
void log_get_write_lsn(LogSN *lsn)
{
    pthread_mutex_lock(&log_gl.log_write_lock);
    *lsn = log_gl.nxt_write_lsn;
    pthread_mutex_unlock(&log_gl.log_write_lock);
}
*/

void log_get_flush_lsn(LogSN *lsn)
{
    pthread_mutex_lock(&log_gl.flush_lsn_lock);
    *lsn = log_gl.nxt_flush_lsn;
    pthread_mutex_unlock(&log_gl.flush_lsn_lock);
}

void log_get_fsync_lsn(LogSN *lsn)
{
    pthread_mutex_lock(&log_gl.fsync_lsn_lock);
    *lsn = log_gl.nxt_fsync_lsn;
    pthread_mutex_unlock(&log_gl.fsync_lsn_lock);
}

void cmdlog_complete_dual_write(bool success)
{
    log_BUFFER *logbuff = &log_gl.log_buffer;
    int  prev_fd               = -1;
    bool prev_fsync_ongoing = false;

    pthread_mutex_lock(&log_gl.log_flush_lock);
    do {
        if (log_gl.log_file.next.fd == -1) {
            /* next_fd == -1 means the first state without log file.
             * created first log file by checkpoint.
             * do not cleanup file fds.
             */
            break;
        }
        if (success) {
            pthread_mutex_lock(&log_gl.log_write_lock);
            if (logbuff->fque[logbuff->fend].nflush > 0) {
                if ((++logbuff->fend) == logbuff->fqsz) logbuff->fend = 0;
            }
            /* Set the position where a dual write end. */
            assert(logbuff->dw_end == -1);
            logbuff->dw_end = logbuff->fend;

            /* update nxt_write_lsn */
            log_gl.nxt_write_lsn.filenum += 1;
            log_gl.nxt_write_lsn.roffset = 0;
            pthread_mutex_unlock(&log_gl.log_write_lock);

            prev_fd                            = log_gl.log_file.curr.fd;
            prev_fsync_ongoing                 = log_gl.log_file.curr.fsync_ongoing;
            log_gl.log_file.curr.fd            = log_gl.log_file.next.fd;
            log_gl.log_file.curr.fsync_ongoing = log_gl.log_file.next.fsync_ongoing;
            log_gl.log_file.curr.size          = log_gl.log_file.next.size;
            log_gl.log_file.next.fd            = -1;
            log_gl.log_file.next.fsync_ongoing = false;
            log_gl.log_file.next.size          = 0;
        } else {
            pthread_mutex_lock(&log_gl.log_write_lock);
            /* reset dual_write flag in flush request queue */
            int index = logbuff->fbgn;
            while (logbuff->fque[index].nflush > 0) {
                if (logbuff->fque[index].dual_write) {
                    logbuff->fque[index].dual_write = false;
                }
                if ((++index) == logbuff->fqsz) index = 0;
            }
            pthread_mutex_unlock(&log_gl.log_write_lock);

            prev_fd                            = log_gl.log_file.next.fd;
            prev_fsync_ongoing                 = log_gl.log_file.next.fsync_ongoing;
            log_gl.log_file.next.fd            = -1;
            log_gl.log_file.next.fsync_ongoing = false;
            log_gl.log_file.next.size          = 0;
        }

        if (prev_fd != -1 && !prev_fsync_ongoing) {
            /* prev_fd is not used anymore */
            (void)disk_close(prev_fd);
        }
    } while(0);
    pthread_mutex_unlock(&log_gl.log_flush_lock);
}

int cmdlog_file_prepare(char *path)
{
    log_FILE *logfile = &log_gl.log_file;
    int ret = 0;

    pthread_mutex_lock(&log_gl.log_flush_lock);
    /* prepare cmdlog file */
    do {
        int fd = disk_open(path, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR | S_IRGRP);
        if (fd < 0) {
            logger->log(EXTENSION_LOG_WARNING, NULL,
                        "Failed to open the cmdlog file. path=%s err=%s\n",
                        logfile->path, strerror(errno));
            ret = -1;
            break;
        }
        snprintf(logfile->path, CMDLOG_MAX_FILEPATH_LENGTH, "%s", path);
        if (logfile->curr.fd == -1) {
            logfile->curr.fd            = fd;
            logfile->curr.fsync_ongoing = false;
            logfile->curr.size          = 0;
        } else {
            /* fd != -1 means that a new cmdlog file is created by checkpoint */
            logfile->next.fd            = fd;
            logfile->next.fsync_ongoing = false;
            logfile->next.size          = 0;
        }
    } while(0);
    pthread_mutex_unlock(&log_gl.log_flush_lock);

    return ret;
}

static void cmdlog_file_final(void)
{
    log_FILE *logfile = &log_gl.log_file;

    /* checkpoint is not in progress
     * so next_fd is not exists
     */
    assert(logfile->next.fd == -1);

    if (logfile->curr.fd != -1) {
        (void)disk_fsync(logfile->curr.fd);
        (void)disk_close(logfile->curr.fd);
        logfile->curr.fd = -1;
    }
}

size_t cmdlog_file_getsize(void)
{
    log_FILE *logfile = &log_gl.log_file;
    size_t file_size = 0;

    pthread_mutex_lock(&log_gl.log_flush_lock);
    pthread_mutex_lock(&log_gl.log_write_lock);
    if (log_gl.log_buffer.dw_end == -1) {
        file_size = logfile->curr.size;
    }
    pthread_mutex_unlock(&log_gl.log_write_lock);
    pthread_mutex_unlock(&log_gl.log_flush_lock);

    return file_size;
}

int cmdlog_file_apply(void)
{
    log_FILE *logfile = &log_gl.log_file;
    assert(logfile->curr.fd > 0);

    logger->log(EXTENSION_LOG_INFO, NULL,
                "[RECOVERY - CMDLOG] applying command log file. path=%s\n", logfile->path);

    struct stat file_stat;
    fstat(logfile->curr.fd, &file_stat);
    logfile->curr.size = file_stat.st_size;
    if (logfile->curr.size == 0) {
        logger->log(EXTENSION_LOG_INFO, NULL,
                    "[RECOVERY - CMDLOG] log file is empty.\n");
        return 0;
    }

    int ret = 0;
    int seek_offset = 0;
    char buf[MAX_LOG_RECORD_SIZE];
    while (log_gl.initialized && seek_offset < logfile->curr.size) {
        LogRec *logrec = (LogRec*)buf;
        LogHdr *loghdr = &logrec->header;

        if (logfile->curr.size - seek_offset < sizeof(LogHdr)) {
            logger->log(EXTENSION_LOG_INFO, NULL,
                        "[RECOVERY - CMDLOG] header of last command was not completely written. "
                        "header_length=%ld\n", sizeof(LogHdr));
            break;
        }

        ssize_t nread = read(logfile->curr.fd, loghdr, sizeof(LogHdr));
        if (nread != sizeof(LogHdr)) {
            logger->log(EXTENSION_LOG_WARNING, NULL,
                        "[RECOVERY - CMDLOG] failed : read header data "
                        "nread(%zd) != header_length(%lu).\n", nread, sizeof(LogHdr));
            ret = -1; break;
        }
        seek_offset += nread;

        if (logfile->curr.size - seek_offset < loghdr->body_length) {
            logger->log(EXTENSION_LOG_INFO, NULL,
                        "[RECOVERY - CMDLOG] body of last command was not completely written. "
                        "body_length=%d\n", loghdr->body_length);
            seek_offset = lseek(logfile->curr.fd, -nread, SEEK_CUR);
            if (seek_offset < 0) {
                logger->log(EXTENSION_LOG_WARNING, NULL,
                            "[RECOVERY - CMDLOG] failed : lseek(SEEK_CUR-%zd). path=%s, error=%s.\n",
                            nread, logfile->path, strerror(errno));
                ret = -1;
            }
            break;
        }

        if (loghdr->body_length > 0) {
            int max_body_length = MAX_LOG_RECORD_SIZE - nread;
            if (max_body_length < loghdr->body_length) {
                logger->log(EXTENSION_LOG_WARNING, NULL,
                            "[RECOVERY - CMDLOG] failed : body length is abnormally too big "
                            "max_body_length(%d) < body_length(%u).\n",
                            max_body_length, loghdr->body_length);
                ret = -1; break;
            }
            logrec->body = buf + nread;
            nread = read(logfile->curr.fd, logrec->body, loghdr->body_length);
            if (nread != loghdr->body_length) {
                logger->log(EXTENSION_LOG_WARNING, NULL,
                            "[RECOVERY - CMDLOG] failed : read body data "
                            "nread(%zd) != body_length(%u).\n", nread, loghdr->body_length);
                ret = -1; break;
            }
            seek_offset += nread;
            ENGINE_ERROR_CODE err = lrec_redo_from_record(logrec);
            if (err != ENGINE_SUCCESS) {
                logger->log(EXTENSION_LOG_WARNING, NULL,
                            "[RECOVERY - CMDLOG] warning : log record redo failed.\n");
                if (err == ENGINE_ENOMEM) {
                    logger->log(EXTENSION_LOG_WARNING, NULL,
                                "[RECOVERY - CMDLOG] failed : out of memory.\n");
                    ret = -1; break;
                }
            }
        }
    }
    if (ret < 0) {
        close(logfile->curr.fd);
    } else {
        logfile->curr.size = seek_offset;
        logger->log(EXTENSION_LOG_INFO, NULL, "[RECOVERY - CMDLOG] success.\n");
    }
    return ret;
}

ENGINE_ERROR_CODE cmdlog_buf_init(struct default_engine* engine)
{
    logger = engine->server.log->get_logger();

    memset(&log_gl, 0, sizeof(log_gl));

    /* log global init */
    log_gl.nxt_fsync_lsn.filenum = 1;
    log_gl.nxt_fsync_lsn.roffset = 0;
    log_gl.nxt_flush_lsn = log_gl.nxt_fsync_lsn;
    log_gl.nxt_write_lsn = log_gl.nxt_fsync_lsn;

    pthread_mutex_init(&log_gl.log_write_lock, NULL);
    pthread_mutex_init(&log_gl.log_flush_lock, NULL);
    pthread_mutex_init(&log_gl.flush_lsn_lock, NULL);
    pthread_mutex_init(&log_gl.fsync_lsn_lock, NULL);

    /* log file init */
    log_FILE *logfile = &log_gl.log_file;
    logfile->path[0]   = '\0';
    logfile->curr.fd   = -1;
    logfile->next.fd   = -1;
    logfile->curr.fsync_ongoing = false;
    logfile->next.fsync_ongoing = false;
    logfile->curr.size = 0;
    logfile->next.size = 0;

    /* log buffer init */
    log_BUFFER *logbuff = &log_gl.log_buffer;

    logbuff->size = CMDLOG_BUFFER_SIZE;
    logbuff->data = malloc(logbuff->size);
    if (logbuff->data == NULL) {
        return ENGINE_ENOMEM;
    }
    logbuff->head = 0;
    logbuff->tail = 0;
    logbuff->last = -1;

    /* log flush request queue init - ring shaped queue */
    logbuff->fqsz = (logbuff->size / CMDLOG_RECORD_MIN_SIZE);
    logbuff->fque = (log_FREQ*)malloc(logbuff->fqsz * sizeof(log_FREQ));
    if (logbuff->fque == NULL) {
        free(logbuff->data);
        return ENGINE_ENOMEM;
    }
    memset(logbuff->fque, 0, logbuff->fqsz * sizeof(log_FREQ));
    logbuff->fbgn = 0;
    logbuff->fend = 0;
    logbuff->dw_end = -1;

    /* log flush thread init */
    log_FLUSHER *flusher = &log_gl.log_flusher;
    pthread_mutex_init(&flusher->lock, NULL);
    pthread_cond_init(&flusher->cond, NULL);
    flusher->sleep = false;
    flusher->running = RUNNING_UNSTARTED;
    flusher->reqstop = false;

    log_gl.initialized = true;
    logger->log(EXTENSION_LOG_INFO, NULL, "CMDLOG BUFFER module initialized.\n");

    return ENGINE_SUCCESS;
}

void cmdlog_buf_final(void)
{
    if (log_gl.initialized == false) {
        return;
    }

    /* log buffer final */
    log_BUFFER *logbuff = &log_gl.log_buffer;

    if (logbuff->data != NULL) {
        free((void*)logbuff->data);
        logbuff->data = NULL;
    }

    if (logbuff->fque != NULL) {
        free((void*)logbuff->fque);
        logbuff->fque = NULL;
    }

    /* log file final */
    cmdlog_file_final();

    pthread_mutex_destroy(&log_gl.log_write_lock);
    pthread_mutex_destroy(&log_gl.log_flush_lock);
    pthread_mutex_destroy(&log_gl.flush_lsn_lock);
    log_gl.initialized = false;
    logger->log(EXTENSION_LOG_INFO, NULL, "CMDLOG BUFFER module destroyed.\n");
}

ENGINE_ERROR_CODE cmdlog_buf_flush_thread_start(void)
{
    assert(log_gl.initialized == true);

    pthread_t tid;
    log_gl.log_flusher.running = RUNNING_UNSTARTED;
    /* create log flush thread */
    if (pthread_create(&tid, NULL, log_flush_thread_main, NULL) != 0) {
        logger->log(EXTENSION_LOG_WARNING, NULL,
                    "Failed to create command log flush thread. error=%s\n", strerror(errno));
        return ENGINE_FAILED;
    }

    /* wait until log flush thread starts */
    while (log_gl.log_flusher.running == RUNNING_UNSTARTED) {
        usleep(5000); /* sleep 5ms */
    }
    logger->log(EXTENSION_LOG_INFO, NULL, "Command log flush thread started.\n");

    return ENGINE_SUCCESS;
}

void cmdlog_buf_flush_thread_stop(void)
{
    log_FLUSHER *flusher = &log_gl.log_flusher;
    if (flusher->running == RUNNING_UNSTARTED) {
        return;
    }

    while (flusher->running == RUNNING_STARTED) {
        flusher->reqstop = true;
        do_log_flusher_wakeup(flusher);
        usleep(5000); /* sleep 5ms */
    }
    logger->log(EXTENSION_LOG_INFO, NULL, "Command log flush thread stopped.\n");
}
#endif
