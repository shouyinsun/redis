/*
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

#include "server.h"


/*****
 * 事务提供了一种将多个命令请求打包,然后一次性、按顺序地执行多个命令的机制
 * 并且在事务的执行期间,服务器不会打断事务而改去执行其他客户端的命令请求
 * 它会将事务中的所有命令都执行完毕,然后才去执行其他客户端的命令请求。 
- Redis中的事务（transaction）是一组命令的集合。事务同命令一样都是Redis中的最小执行单位,一个事务中的命令要么都执行,要么都不执行。

Redis事务（transaction）提供了以下五个命令,用于用户操作事务功能,其分别是：
MULTI	标记一个事务块的开始
DISCARD	放弃执行事务
EXEC	执行事务中的所有命令
WATCH	监视一个或多个key,如果至少有一个key在EXEC之前被修改,则放弃执行事务
UNWATCH	取消WATCH命令对所有键的监视



执行事务的过程分为以下的几个阶段：
开始事务
命令入队
执行事务
 * 
 * 
 * ******/
/* ================================ MULTI/EXEC ============================== */

/* Client state initialization for MULTI/EXEC */
void initClientMultiState(client *c) {
    c->mstate.commands = NULL;
    c->mstate.count = 0;
}

/* Release all the resources associated with MULTI/EXEC state */
void freeClientMultiState(client *c) {
    int j;

    for (j = 0; j < c->mstate.count; j++) {
        int i;
        multiCmd *mc = c->mstate.commands+j;

        for (i = 0; i < mc->argc; i++)
            decrRefCount(mc->argv[i]);
        zfree(mc->argv);
    }
    zfree(c->mstate.commands);
}

/* Add a new command into the MULTI commands queue */
//事务命令入队列
void queueMultiCommand(client *c) {
    multiCmd *mc;
    int j;
    // 增加队列的空间
    c->mstate.commands = zrealloc(c->mstate.commands,
            sizeof(multiCmd)*(c->mstate.count+1));
    // 获取新命令的存放地址
    mc = c->mstate.commands+c->mstate.count;
    // 设置新命令的参数列表、参数数量和命令函数
    mc->cmd = c->cmd;
    mc->argc = c->argc;
    mc->argv = zmalloc(sizeof(robj*)*c->argc);
    memcpy(mc->argv,c->argv,sizeof(robj*)*c->argc);
    // 增加引用计数
    for (j = 0; j < c->argc; j++)
        incrRefCount(mc->argv[j]);
    // 事务命令个数加1
    c->mstate.count++;
}

void discardTransaction(client *c) {
    freeClientMultiState(c);
    initClientMultiState(c);
    c->flags &= ~(CLIENT_MULTI|CLIENT_DIRTY_CAS|CLIENT_DIRTY_EXEC);
    unwatchAllKeys(c);
}

/* Flag the transacation as DIRTY_EXEC so that EXEC will fail.
 * Should be called every time there is an error while queueing a command. */
void flagTransaction(client *c) {
    if (c->flags & CLIENT_MULTI)
        c->flags |= CLIENT_DIRTY_EXEC;
}

void multiCommand(client *c) {
    // 客户端已经处于事务状态,回复错误后返回
    if (c->flags & CLIENT_MULTI) {
        addReplyError(c,"MULTI calls can not be nested");
        return;
    }
    // 打开客户的的事务状态标识
    c->flags |= CLIENT_MULTI;
    // 回复OK
    addReply(c,shared.ok);
}

void discardCommand(client *c) {
    if (!(c->flags & CLIENT_MULTI)) {
        addReplyError(c,"DISCARD without MULTI");
        return;
    }
    discardTransaction(c);
    addReply(c,shared.ok);
}

/* Send a MULTI command to all the slaves and AOF file. Check the execCommand
 * implementation for more information. */
void execCommandPropagateMulti(client *c) {
    robj *multistring = createStringObject("MULTI",5);

    propagate(server.multiCommand,c->db->id,&multistring,1,
              PROPAGATE_AOF|PROPAGATE_REPL);
    decrRefCount(multistring);
}


// 执行事务
void execCommand(client *c) {
    int j;
    robj **orig_argv;
    int orig_argc;
    struct redisCommand *orig_cmd;
    // 是否传播的标识
    int must_propagate = 0; /* Need to propagate MULTI/EXEC to AOF / slaves? */
    // 如果客户端当前不处于事务状态,回复错误后返回
    if (!(c->flags & CLIENT_MULTI)) {
        addReplyError(c,"EXEC without MULTI");
        return;
    }

    /* Check if we need to abort the EXEC because:
     * 1) Some WATCHed key was touched.
     * 2) There was a previous error while queueing commands.
     * A failed EXEC in the first case returns a multi bulk nil object
     * (technically it is not an error but a special behavior), while
     * in the second an EXECABORT error is returned. */
    // 检查是否需要中断EXEC的执行因为：
    /*
        1. 被监控的key被修改
        2. 入队命令时发生了错误
    */
    // 第一种情况返回空回复对象,第二种情况返回一个EXECABORT错误
    // 如果客户的处于 1.命令入队时错误或者2.被监控的key被修改
    if (c->flags & (CLIENT_DIRTY_CAS|CLIENT_DIRTY_EXEC)) {
        // 回复错误信息
        addReply(c, c->flags & CLIENT_DIRTY_EXEC ? shared.execaborterr :
                                                  shared.nullmultibulk);
        // 取消事务
        discardTransaction(c);
        // 跳转到处理监控器代码
        goto handle_monitor;
    }

    /* Exec all the queued commands */
    // 执行队列数组中的命令
    // 因为所有的命令都是安全的,因此取消对客户端的所有的键的监视
    unwatchAllKeys(c); /* Unwatch ASAP otherwise we'll waste CPU cycles */
    // 备份EXEC命令
    orig_argv = c->argv;
    orig_argc = c->argc;
    orig_cmd = c->cmd;
    // 回复一个事务命令的个数
    addReplyMultiBulkLen(c,c->mstate.count);
    // 遍历执行所有事务命令
    for (j = 0; j < c->mstate.count; j++) {
        // 设置一个当前事务命令给客户端
        c->argc = c->mstate.commands[j].argc;
        c->argv = c->mstate.commands[j].argv;
        c->cmd = c->mstate.commands[j].cmd;

        /* Propagate a MULTI request once we encounter the first write op.
         * This way we'll deliver the MULTI/..../EXEC block as a whole and
         * both the AOF and the replication link will have the same consistency
         * and atomicity guarantees. */
        // 当执行到第一个写命令时,传播事务状态
        if (!must_propagate && !(c->cmd->flags & CMD_READONLY)) {
            // 发送一个MULTI命令给所有的从节点和AOF文件
            execCommandPropagateMulti(c);
            // 设置已经传播过的标识
            must_propagate = 1;
        }
        // 执行该命令
        call(c,CMD_CALL_FULL);
        // 命令可能会被修改,重新存储在事务命令队列中
        /* Commands may alter argc/argv, restore mstate. */
        c->mstate.commands[j].argc = c->argc;
        c->mstate.commands[j].argv = c->argv;
        c->mstate.commands[j].cmd = c->cmd;
    }
    // 还原命令和参数
    c->argv = orig_argv;
    c->argc = orig_argc;
    c->cmd = orig_cmd;
    // 取消事务状态
    discardTransaction(c);
    /* Make sure the EXEC command will be propagated as well if MULTI
     * was already propagated. */
    // 如果传播了EXEC命令,表示执行了写命令,更新数据库脏键数
    if (must_propagate) server.dirty++;

handle_monitor:
    /* Send EXEC to clients waiting data from MONITOR. We do it here
     * since the natural order of commands execution is actually:
     * MUTLI, EXEC, ... commands inside transaction ...
     * Instead EXEC is flagged as CMD_SKIP_MONITOR in the command
     * table, and we do it here with correct ordering. */
    // 如果服务器设置了监控器,并且服务器不处于载入文件的状态
    if (listLength(server.monitors) && !server.loading)
        replicationFeedMonitors(c,server.monitors,c->db->id,c->argv,c->argc);
}

/* ===================== WATCH (CAS alike for MULTI/EXEC) ===================
 *
 * The implementation uses a per-DB hash table mapping keys to list of clients
 * WATCHing those keys, so that given a key that is going to be modified
 * we can mark all the associated clients as dirty.
 *
 * Also every client contains a list of WATCHed keys so that's possible to
 * un-watch such keys when the client is freed or when UNWATCH is called. */

/* In the client->watched_keys list we need to use watchedKey structures
 * as in order to identify a key in Redis we need both the key name and the
 * DB */
typedef struct watchedKey {
    // 被监视的key
    robj *key;
    // 被监视的key所在的数据库
    redisDb *db;
} watchedKey;

/* Watch for the specified key */
void watchForKey(client *c, robj *key) {
    list *clients = NULL;
    listIter li;
    listNode *ln;
    watchedKey *wk;

    /* Check if we are already watching for this key */
    listRewind(c->watched_keys,&li);
    // 遍历客户端监视的键的链表,检查是否已经监视了指定的键
    while((ln = listNext(&li))) {
        wk = listNodeValue(ln);
        // 如果键已经被监视,则直接返回
        if (wk->db == c->db && equalStringObjects(key,wk->key))
            return; /* Key already watched */
    }
    /* This key is not already watched in this DB. Let's add it */
    // 如果数据库中该键没有被client监视则添加它
    clients = dictFetchValue(c->db->watched_keys,key);
    // 没有被client监视
    if (!clients) {
        // 创建一个空链表
        clients = listCreate();
        // 值是被client监控的key,键是client,添加到数据库的watched_keys字典中
        dictAdd(c->db->watched_keys,key,clients);
        incrRefCount(key);
    }
    // 将当前client添加到监视该key的client链表的尾部
    listAddNodeTail(clients,c);
    /* Add the new key to the list of keys watched by this client */
    // 将新的被监视的key和与该key关联的数据库加入到客户端的watched_keys中
    wk = zmalloc(sizeof(*wk));
    wk->key = key;
    wk->db = c->db;
    incrRefCount(key);
    listAddNodeTail(c->watched_keys,wk);
}

/* Unwatch all the keys watched by this client. To clean the EXEC dirty
 * flag is up to the caller. */
void unwatchAllKeys(client *c) {
    listIter li;
    listNode *ln;

    if (listLength(c->watched_keys) == 0) return;
    listRewind(c->watched_keys,&li);
    while((ln = listNext(&li))) {
        list *clients;
        watchedKey *wk;

        /* Lookup the watched key -> clients list and remove the client
         * from the list */
        wk = listNodeValue(ln);
        clients = dictFetchValue(wk->db->watched_keys, wk->key);
        serverAssertWithInfo(c,NULL,clients != NULL);
        listDelNode(clients,listSearchKey(clients,c));
        /* Kill the entry at all if this was the only client */
        if (listLength(clients) == 0)
            dictDelete(wk->db->watched_keys, wk->key);
        /* Remove this watched key from the client->watched list */
        listDelNode(c->watched_keys,ln);
        decrRefCount(wk->key);
        zfree(wk);
    }
}

/* "Touch" a key, so that if this key is being WATCHed by some client the
 * next EXEC will fail. */
void touchWatchedKey(redisDb *db, robj *key) {
    list *clients;
    listIter li;
    listNode *ln;

    if (dictSize(db->watched_keys) == 0) return;
    // 找出监视该key的client链表
    clients = dictFetchValue(db->watched_keys, key);
    if (!clients) return;

    /* Mark all the clients watching this key as CLIENT_DIRTY_CAS */
    /* Check if we are already watching for this key */
    listRewind(clients,&li);
    // 遍历所有监视该key的client
    while((ln = listNext(&li))) {
        client *c = listNodeValue(ln);
        // 设置CLIENT_DIRTY_CAS标识
        c->flags |= CLIENT_DIRTY_CAS;
    }
}

/* On FLUSHDB or FLUSHALL all the watched keys that are present before the
 * flush but will be deleted as effect of the flushing operation should
 * be touched. "dbid" is the DB that's getting the flush. -1 if it is
 * a FLUSHALL operation (all the DBs flushed). */
void touchWatchedKeysOnFlush(int dbid) {
    listIter li1, li2;
    listNode *ln;

    /* For every client, check all the waited keys */
    listRewind(server.clients,&li1);
    while((ln = listNext(&li1))) {
        client *c = listNodeValue(ln);
        listRewind(c->watched_keys,&li2);
        while((ln = listNext(&li2))) {
            watchedKey *wk = listNodeValue(ln);

            /* For every watched key matching the specified DB, if the
             * key exists, mark the client as dirty, as the key will be
             * removed. */
            if (dbid == -1 || wk->db->id == dbid) {
                if (dictFind(wk->db->dict, wk->key->ptr) != NULL)
                    c->flags |= CLIENT_DIRTY_CAS;
            }
        }
    }
}

//WATCH命令
void watchCommand(client *c) {
    int j;
    //如果已经处于事务状态,则回复错误后返回,必须在执行MULTI命令执行前执行WATCH
    if (c->flags & CLIENT_MULTI) {
        addReplyError(c,"WATCH inside MULTI is not allowed");
        return;
    }
    // 遍历所有的参数
    for (j = 1; j < c->argc; j++)
        // 监控当前key
        watchForKey(c,c->argv[j]);
    addReply(c,shared.ok);
}

void unwatchCommand(client *c) {
    unwatchAllKeys(c);
    c->flags &= (~CLIENT_DIRTY_CAS);
    addReply(c,shared.ok);
}
