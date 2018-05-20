/*
 * Copyright (c) 2009-2012, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */


#ifndef __REDIS_RIO_H
#define __REDIS_RIO_H

#include <stdio.h>
#include <stdint.h>
#include "sds.h"

struct _rio {
    /* Backend functions.
     * Since this functions do not tolerate short writes or reads the return
     * value is simplified to: zero on error, non zero on complete success. */
    
    // 读,写,读写偏移量、flush操作的函数指针,非0表示成功
    size_t (*read)(struct _rio *, void *buf, size_t len);
    size_t (*write)(struct _rio *, const void *buf, size_t len);
    off_t (*tell)(struct _rio *);
    int (*flush)(struct _rio *);
    /* The update_cksum method if not NULL is used to compute the checksum of
     * all the data that was read or written so far. The method should be
     * designed so that can be called with the current checksum, and the buf
     * and len fields pointing to the new block of data to add to the checksum
     * computation. */
    // 计算和校验函数
    void (*update_cksum)(struct _rio *, const void *buf, size_t len);

    /* The current checksum */
    // 当前校验和
    uint64_t cksum;

    /* number of bytes read or written */
    // 读或写的字节数
    size_t processed_bytes;

    /* maximum single read or write chunk size */
    // 每次读或写的最大字节数
    size_t max_processing_chunk;

    /* Backend-specific vars. */
    // 读写的三种对象：
    // 缓冲区 IO
    // 标准输入输出
    // 文件描述符集合
    union {
        /* In-memory buffer target. */
        /*内存缓冲区 In-memory buffer target. */
        struct {
            sds ptr;//缓冲区的指针,本质是char *
            off_t pos;//缓冲区的偏移量
        } buffer;
        /* Stdio file pointer target. */
        /*标准文件IO*/
        struct {
            FILE *fp;// 文件指针,指向被打开的文件
             /* 最近一次同步之后所写的字节数 */
            off_t buffered; /* Bytes written since last fsync. */
            off_t autosync; /* fsync after 'autosync' bytes written. */
        } file;
        /* Multiple FDs target (used to write to N sockets). */
         /*文件描述符集合 */
        struct {
            /*文件描述符数组*/
            int *fds;       /* File descriptors. */
            /*每个文件的errorNo,0是ok*/
            int *state;     /* Error state of each fd. 0 (if ok) or errno. */
            /*文件描述符个数,数组长度*/
            int numfds;
            /*偏移量*/
            off_t pos;
            /*缓冲区*/
            sds buf;
        } fdset;
    } io;
};//rio 结构体

typedef struct _rio rio;

/* The following functions are our interface with the stream. They'll call the
 * actual implementation of read / write / tell, and will update the checksum
 * if needed. */

static inline size_t rioWrite(rio *r, const void *buf, size_t len) {
    while (len) {
        // 读的字节长度,不能超过每次读或写的最大字节数max_processing_chunk
        size_t bytes_to_write = (r->max_processing_chunk && r->max_processing_chunk < len) ? r->max_processing_chunk : len;
        // 更新和校验
        if (r->update_cksum) r->update_cksum(r,buf,bytes_to_write);
        // 更新偏移量,指向下一个读的位置
        if (r->write(r,buf,bytes_to_write) == 0)
            return 0;
        buf = (char*)buf + bytes_to_write;
        // 计算剩余要读的长度
        len -= bytes_to_write;
        // 更新读或写的字节数
        r->processed_bytes += bytes_to_write;
    }
    return 1;
}

static inline size_t rioRead(rio *r, void *buf, size_t len) {
    while (len) {
        size_t bytes_to_read = (r->max_processing_chunk && r->max_processing_chunk < len) ? r->max_processing_chunk : len;
        // 调用自身的write方法写入
        if (r->read(r,buf,bytes_to_read) == 0)
            return 0;
        // 更新和校验
        if (r->update_cksum) r->update_cksum(r,buf,bytes_to_read);
        buf = (char*)buf + bytes_to_read;
        len -= bytes_to_read;
        r->processed_bytes += bytes_to_read;
    }
    return 1;
}

//当前io的偏移量
static inline off_t rioTell(rio *r) {
    return r->tell(r);
}

//io的flush函数
static inline int rioFlush(rio *r) {
    return r->flush(r);
}

//初始化一个文件io
void rioInitWithFile(rio *r, FILE *fp);
//初始化缓冲区io
void rioInitWithBuffer(rio *r, sds s);
//初始化文件描述符集合io
void rioInitWithFdset(rio *r, int *fds, int numfds);

void rioFreeFdset(rio *r);

size_t rioWriteBulkCount(rio *r, char prefix, int count);
size_t rioWriteBulkString(rio *r, const char *buf, size_t len);
size_t rioWriteBulkLongLong(rio *r, long long l);
size_t rioWriteBulkDouble(rio *r, double d);

void rioGenericUpdateChecksum(rio *r, const void *buf, size_t len);
void rioSetAutoSync(rio *r, off_t bytes);

#endif
