/* rio.c is a simple stream-oriented I/O abstraction that provides an interface
 * to write code that can consume/produce data using different concrete input
 * and output devices. For instance the same rdb.c code using the rio
 * abstraction can be used to read and write the RDB format using in-memory
 * buffers or files.
 *
 * A rio object provides the following methods:
 *  read: read from stream.
 *  write: write to stream.
 *  tell: get the current offset.
 *
 * It is also possible to set a 'checksum' method that is used by rio.c in order
 * to compute a checksum of the data written or read, or to query the rio object
 * for the current checksum.
 *
 * ----------------------------------------------------------------------------
 *
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


#include "fmacros.h"
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include "rio.h"
#include "util.h"
#include "crc64.h"
#include "config.h"
#include "server.h"




/***
 * 
io是Redis对IO操作的一个抽象
可以面向不同的输入输出设备
例如:一个缓冲区IO、文件IO和socket IO。
一个rio对象提供一下四个方法：
read：读操作
write：写操作
tell：读写的偏移量
flush：冲洗缓冲区操作
 * 
 * 
 * ***/



//缓冲区实现
/* ------------------------- Buffer I/O implementation ----------------------- */

/* Returns 1 or 0 for success/failure. */
// 将len长的buf写到一个缓冲区对象r中
static size_t rioBufferWrite(rio *r, const void *buf, size_t len) {
    r->io.buffer.ptr = sdscatlen(r->io.buffer.ptr,(char*)buf,len);
    r->io.buffer.pos += len;//更新偏移量
    return 1;
}

/* Returns 1 or 0 for success/failure. */
// 讲缓冲区对象r读到buf中,读len长
static size_t rioBufferRead(rio *r, void *buf, size_t len) {
    if (sdslen(r->io.buffer.ptr)-r->io.buffer.pos < len)
        return 0; /* not enough buffer to return len bytes. */
    memcpy(buf,r->io.buffer.ptr+r->io.buffer.pos,len);
    r->io.buffer.pos += len;//更新偏移量
    return 1;
}

/* Returns read/write position in buffer. */
// 返回缓冲区对象r当前的偏移量
static off_t rioBufferTell(rio *r) {
    return r->io.buffer.pos;
}

/* Flushes any buffer to target device if applicable. Returns 1 on success
 * and 0 on failures. */
//flush 缓冲区
static int rioBufferFlush(rio *r) {
    UNUSED(r);//void r,强转成void类型对象,缓冲区就相当于释放
    return 1; /* Nothing to do, our write just appends to the buffer. */
}

// 定义一个缓冲区对象并初始化方法和成员
static const rio rioBufferIO = {
    rioBufferRead,
    rioBufferWrite,
    rioBufferTell,
    rioBufferFlush,
    NULL,           /* update_checksum */
    0,              /* current checksum */
    0,              /* bytes read or written */
    0,              /* read/write chunk size */
    { { NULL, 0 } } /* union for io-specific vars */
};

//初始化一个缓冲区对象r
void rioInitWithBuffer(rio *r, sds s) {
    *r = rioBufferIO;
    r->io.buffer.ptr = s;
    r->io.buffer.pos = 0;
}



//标准文件io的实现
/* --------------------- Stdio file pointer implementation ------------------- */

/* Returns 1 or 0 for success/failure. */
//文件io的写入 写入len长度的buf
static size_t rioFileWrite(rio *r, const void *buf, size_t len) {
    size_t retval;

    //调用底层库函数写入
    retval = fwrite(buf,len,1,r->io.file.fp);
    // 更新写入的长度
    r->io.file.buffered += len;

    //达到了自动同步autosync 设置的值,进行flush操作
    if (r->io.file.autosync &&
        r->io.file.buffered >= r->io.file.autosync)
    {
        //flush到文件中
        fflush(r->io.file.fp);
        //同步操作
        aof_fsync(fileno(r->io.file.fp));
        //置0
        r->io.file.buffered = 0;
    }
    return retval;
}

/* Returns 1 or 0 for success/failure. */
// 从文件流对象r中读出len长度的字节到buf中
static size_t rioFileRead(rio *r, void *buf, size_t len) {
    return fread(buf,len,1,r->io.file.fp);
}

/* Returns read/write position in file. */
static off_t rioFileTell(rio *r) {
    return ftello(r->io.file.fp);
}

/* Flushes any buffer to target device if applicable. Returns 1 on success
 * and 0 on failures. */
static int rioFileFlush(rio *r) {
    return (fflush(r->io.file.fp) == 0) ? 1 : 0;
}

// 初始化一个文件流对象
static const rio rioFileIO = {
    rioFileRead,
    rioFileWrite,
    rioFileTell,
    rioFileFlush,
    NULL,           /* update_checksum */
    0,              /* current checksum */
    0,              /* bytes read or written */
    0,              /* read/write chunk size */
    { { NULL, 0 } } /* union for io-specific vars */
};

//初始化
void rioInitWithFile(rio *r, FILE *fp) {
    *r = rioFileIO;
    r->io.file.fp = fp;
    r->io.file.buffered = 0;
    r->io.file.autosync = 0;
}



// 文件描述符合集合实现
/* ------------------- File descriptors set implementation ------------------- */

/* Returns 1 or 0 for success/failure.
 * The function returns success as long as we are able to correctly write
 * to at least one file descriptor.
 *
 * When buf is NULL and len is 0, the function performs a flush operation
 * if there is some pending buffer, so this function is also used in order
 * to implement rioFdsetFlush(). */

// 将buf写入文件描述符集合对象
static size_t rioFdsetWrite(rio *r, const void *buf, size_t len) {
    ssize_t retval;
    int j;
    unsigned char *p = (unsigned char*) buf;
    int doflush = (buf == NULL && len == 0);//如果buf为空且len为0,相当于flush操作

    /* To start we always append to our buffer. If it gets larger than
     * a given size, we actually write to the sockets. */
    // 将buf中的内容写到文件描述符集合对象的缓冲区中
    if (len) {
        r->io.fdset.buf = sdscatlen(r->io.fdset.buf,buf,len);
        //len 置0
        len = 0; /* Prevent entering the while below if we don't flush. */
        //如果缓冲区太大需要flush到socket中
        if (sdslen(r->io.fdset.buf) > PROTO_IOBUF_LEN) doflush = 1;
    }

    // flush文件描述符集合对象,设置集合缓冲区长度和集合缓冲区地址
    if (doflush) {
        p = (unsigned char*) r->io.fdset.buf;
        len = sdslen(r->io.fdset.buf);
    }

    //写小的块,并行
    /* Write in little chunchs so that when there are big writes we
     * parallelize while the kernel is sending data in background to
     * the TCP socket. */
    while(len) {//一次可能无法flush完,需要循环多次
        size_t count = len < 1024 ? len : 1024;//一次最多1024个字节
        int broken = 0;
        for (j = 0; j < r->io.fdset.numfds; j++) {
            //跳过已经错误状态的fd
            if (r->io.fdset.state[j] != 0) {
                /* Skip FDs alraedy in error. */
                broken++;
                continue;
            }

            /* Make sure to write 'count' bytes to the socket regardless
             * of short writes. */
            size_t nwritten = 0;
            // 往文件描述符fd,新写的数据一次或多次写够count个字节
            while(nwritten != count) {//写失败,判断是不是写阻塞,是则设置errno 为超时
                retval = write(r->io.fdset.fds[j],p+nwritten,count-nwritten);
                if (retval <= 0) {
                    /* With blocking sockets, which is the sole user of this
                     * rio target, EWOULDBLOCK is returned only because of
                     * the SO_SNDTIMEO socket option, so we translate the error
                     * into one more recognizable by the user. */
                    if (retval == -1 && errno == EWOULDBLOCK) errno = ETIMEDOUT;
                    break;
                }
                nwritten += retval;
            }

            // 如果刚才写失败的情况,则将当前的文件描述符状态设置为错误的标记码
            if (nwritten != count) {
                /* Mark this FD as broken. */
                r->io.fdset.state[j] = errno;
                if (r->io.fdset.state[j] == 0) r->io.fdset.state[j] = EIO;
            }
        }
        // 所有的文件描述符都出错返回0
        if (broken == r->io.fdset.numfds) return 0; /* All the FDs in error. */
        p += count;
        len -= count;
        r->io.fdset.pos += count;
    }

    if (doflush) sdsclear(r->io.fdset.buf);
    return 1;
}

/* Returns 1 or 0 for success/failure. */
// 文件描述符集合对象不支持读,直接返回0
static size_t rioFdsetRead(rio *r, void *buf, size_t len) {
    UNUSED(r);
    UNUSED(buf);
    UNUSED(len);
    return 0; /* Error, this target does not support reading. */
}

/* Returns read/write position in file. */
static off_t rioFdsetTell(rio *r) {
    return r->io.fdset.pos;
}

/* Flushes any buffer to target device if applicable. Returns 1 on success
 * and 0 on failures. */
static int rioFdsetFlush(rio *r) {
    /* Our flush is implemented by the write method, that recognizes a
     * buffer set to NULL with a count of zero as a flush request. */
    return rioFdsetWrite(r,NULL,0);
}

// 初始化一个文件描述符集合对象
static const rio rioFdsetIO = {
    rioFdsetRead,
    rioFdsetWrite,
    rioFdsetTell,
    rioFdsetFlush,
    NULL,           /* update_checksum */
    0,              /* current checksum */
    0,              /* bytes read or written */
    0,              /* read/write chunk size */
    { { NULL, 0 } } /* union for io-specific vars */
};

//init
void rioInitWithFdset(rio *r, int *fds, int numfds) {
    int j;

    *r = rioFdsetIO;
    r->io.fdset.fds = zmalloc(sizeof(int)*numfds);
    r->io.fdset.state = zmalloc(sizeof(int)*numfds);
    memcpy(r->io.fdset.fds,fds,sizeof(int)*numfds);
    for (j = 0; j < numfds; j++) r->io.fdset.state[j] = 0;
    r->io.fdset.numfds = numfds;
    r->io.fdset.pos = 0;
    r->io.fdset.buf = sdsempty();
}

/* release the rio stream. */
// 释放文件描述符集合流对象
void rioFreeFdset(rio *r) {
    zfree(r->io.fdset.fds);
    zfree(r->io.fdset.state);
    sdsfree(r->io.fdset.buf);
}

/* ---------------------------- Generic functions ---------------------------- */

/* This function can be installed both in memory and file streams when checksum
 * computation is needed. */
// 根据CRC64算法进行校验和
void rioGenericUpdateChecksum(rio *r, const void *buf, size_t len) {
    r->cksum = crc64(r->cksum,buf,len);
}

/* Set the file-based rio object to auto-fsync every 'bytes' file written.
 * By default this is set to zero that means no automatic file sync is
 * performed.
 *
 * This feature is useful in a few contexts since when we rely on OS write
 * buffers sometimes the OS buffers way too much, resulting in too many
 * disk I/O concentrated in very little time. When we fsync in an explicit
 * way instead the I/O pressure is more distributed across time. */

 // 设置自动同步的字节数限制,如果bytes为0,则意味着不执行
void rioSetAutoSync(rio *r, off_t bytes) {
    //限制为文件流对象
    serverAssert(r->read == rioFileIO.read);
    r->io.file.autosync = bytes;
}



//利用rio生成AOF协议  Append Only File
/* --------------------------- Higher level interface --------------------------
 *
 * The following higher level functions use lower level rio.c functions to help
 * generating the Redis protocol for the Append Only File. */

/* Write multi bulk count in the format: "*<count>\r\n". */

// 以"*<count>\r\n"格式为写如一个int整型的count
size_t rioWriteBulkCount(rio *r, char prefix, int count) {
    char cbuf[128];
    int clen;

    cbuf[0] = prefix;
    clen = 1+ll2string(cbuf+1,sizeof(cbuf)-1,count);
    cbuf[clen++] = '\r';
    cbuf[clen++] = '\n';
    if (rioWrite(r,cbuf,clen) == 0) return 0;
    return clen;
}

/* Write binary-safe string in the format: "$<count>\r\n<payload>\r\n". */
// 以"$<count>\r\n<payload>\r\n"为格式写入一个字符串
size_t rioWriteBulkString(rio *r, const char *buf, size_t len) {
    size_t nwritten;

    if ((nwritten = rioWriteBulkCount(r,'$',len)) == 0) return 0;
    if (len > 0 && rioWrite(r,buf,len) == 0) return 0;
    if (rioWrite(r,"\r\n",2) == 0) return 0;
    return nwritten+len+2;
}

/* Write a long long value in format: "$<count>\r\n<payload>\r\n". */
// 以"$<count>\r\n<payload>\r\n"为格式写入一个longlong 值
size_t rioWriteBulkLongLong(rio *r, long long l) {
    char lbuf[32];
    unsigned int llen;

    llen = ll2string(lbuf,sizeof(lbuf),l);
    return rioWriteBulkString(r,lbuf,llen);
}

/* Write a double value in the format: "$<count>\r\n<payload>\r\n" */
// 以"$<count>\r\n<payload>\r\n"为格式写入一个 double 值
size_t rioWriteBulkDouble(rio *r, double d) {
    char dbuf[128];
    unsigned int dlen;

    dlen = snprintf(dbuf,sizeof(dbuf),"%.17g",d);
    return rioWriteBulkString(r,dbuf,dlen);
}
