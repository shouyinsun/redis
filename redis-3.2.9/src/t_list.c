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

/****
 * 
 * 列表类型 底层实现是quicklist
 * 
 * 
 * 列表命令实现了阻塞命令,例如：BLPOP、BRPOP、BLPOPRPUSH
 * 
 * ***/



#include "server.h"

/*-----------------------------------------------------------------------------
 * List API
 *----------------------------------------------------------------------------*/

/* The function pushes an element to the specified list object 'subject',
 * at head or tail position as specified by 'where'.
 *
 * There is no need for the caller to increment the refcount of 'value' as
 * the function takes care of it if needed. */

//列表类型的头/尾 压入一个元素   PUSH命令的底层实现
void listTypePush(robj *subject, robj *value, int where) {
    if (subject->encoding == OBJ_ENCODING_QUICKLIST) {//encoding 为 QUICKLIST
        int pos = (where == LIST_HEAD) ? QUICKLIST_HEAD : QUICKLIST_TAIL;
        value = getDecodedObject(value);
        size_t len = sdslen(value->ptr);
        quicklistPush(subject->ptr, value->ptr, len, pos);
        decrRefCount(value);
    } else {
        serverPanic("Unknown list encoding");
    }
}

void *listPopSaver(unsigned char *data, unsigned int sz) {
    return createStringObject((char*)data,sz);
}


//列表类型的头/尾 弹出一个元素   POP命令的底层实现
robj *listTypePop(robj *subject, int where) {
    long long vlong;
    robj *value = NULL;

    int ql_where = where == LIST_HEAD ? QUICKLIST_HEAD : QUICKLIST_TAIL;
    if (subject->encoding == OBJ_ENCODING_QUICKLIST) {
        //从quicklist的头节点或尾节点pop弹出出一个entry,并赋值
        if (quicklistPopCustom(subject->ptr, ql_where, (unsigned char **)&value,
                               NULL, &vlong, listPopSaver)) {
            if (!value)
                value = createStringObjectFromLongLong(vlong);
        }
    } else {
        serverPanic("Unknown list encoding");
    }
    return value;
}

// 返回对象的长度,entry节点个数
unsigned long listTypeLength(robj *subject) {
    if (subject->encoding == OBJ_ENCODING_QUICKLIST) {
        return quicklistCount(subject->ptr);
    } else {
        serverPanic("Unknown list encoding");
    }
}

/* Initialize an iterator at the specified index. */
// 初始化一个指定的下标的列表类型的迭代器
listTypeIterator *listTypeInitIterator(robj *subject, long index,
                                       unsigned char direction) {
    listTypeIterator *li = zmalloc(sizeof(listTypeIterator));
    li->subject = subject;
    li->encoding = subject->encoding;
    li->direction = direction;
    li->iter = NULL;//quicklist迭代器为空
    /* LIST_HEAD means start at TAIL and move *towards* head.
     * LIST_TAIL means start at HEAD and move *towards tail. */
    int iter_direction =
        direction == LIST_HEAD ? AL_START_TAIL : AL_START_HEAD;
    if (li->encoding == OBJ_ENCODING_QUICKLIST) {
        li->iter = quicklistGetIteratorAtIdx(li->subject->ptr,
                                             iter_direction, index);
    } else {
        serverPanic("Unknown list encoding");
    }
    return li;
}

/* Clean up the iterator. */
// 释放迭代器空间
void listTypeReleaseIterator(listTypeIterator *li) {
    zfree(li->iter);
    zfree(li);
}

/* Stores pointer to current the entry in the provided entry structure
 * and advances the position of the iterator. Returns 1 when the current
 * entry is in fact an entry, 0 otherwise. */

// 将列表类型的迭代器指向的entry保存在提供的listTypeEntry结构中 
//1表示成功,0失败
int listTypeNext(listTypeIterator *li, listTypeEntry *entry) {
    /* Protect from converting when iterating */
    //确保对象编码类型和迭代器中encoding成员相等
    serverAssert(li->subject->encoding == li->encoding);

    entry->li = li;
    if (li->encoding == OBJ_ENCODING_QUICKLIST) {
        //保存在 &entry->entry 并有更新迭代器
        return quicklistNext(li->iter, &entry->entry);
    } else {
        serverPanic("Unknown list encoding");
    }
    return 0;
}

/* Return entry or NULL at the current position of the iterator. */
// 根据当前的迭代器 返回一个节点的value对象
robj *listTypeGet(listTypeEntry *entry) {
    robj *value = NULL;
    if (entry->li->encoding == OBJ_ENCODING_QUICKLIST) {
        if (entry->entry.value) {//string
            value = createStringObject((char *)entry->entry.value,
                                       entry->entry.sz);
        } else {//int
            value = createStringObjectFromLongLong(entry->entry.longval);
        }
    } else {
        serverPanic("Unknown list encoding");
    }
    return value;
}

//列表类型的插入
void listTypeInsert(listTypeEntry *entry, robj *value, int where) {
    if (entry->li->encoding == OBJ_ENCODING_QUICKLIST) {
        value = getDecodedObject(value);
        sds str = value->ptr;
        size_t len = sdslen(str);
        if (where == LIST_TAIL) {
            quicklistInsertAfter((quicklist *)entry->entry.quicklist,
                                 &entry->entry, str, len);
        } else if (where == LIST_HEAD) {
            quicklistInsertBefore((quicklist *)entry->entry.quicklist,
                                  &entry->entry, str, len);
        }
        decrRefCount(value);
    } else {
        serverPanic("Unknown list encoding");
    }
}

/* Compare the given object with the entry at the current position. */
// 比较列表类型的entry结构与对象的entry节点的值是否等
int listTypeEqual(listTypeEntry *entry, robj *o) {
    if (entry->li->encoding == OBJ_ENCODING_QUICKLIST) {
        serverAssertWithInfo(NULL,o,sdsEncodedObject(o));
        return quicklistCompare(entry->entry.zi,o->ptr,sdslen(o->ptr));
    } else {
        serverPanic("Unknown list encoding");
    }
}

/* Delete the element pointed to. */
// 删除迭代器指向的entry
void listTypeDelete(listTypeIterator *iter, listTypeEntry *entry) {
    if (entry->li->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklistDelEntry(iter->iter, &entry->entry);
    } else {
        serverPanic("Unknown list encoding");
    }
}

/* Create a quicklist from a single ziplist */
//ziplist 转换成 quicklist
void listTypeConvert(robj *subject, int enc) {
    serverAssertWithInfo(NULL,subject,subject->type==OBJ_LIST);
    serverAssertWithInfo(NULL,subject,subject->encoding==OBJ_ENCODING_ZIPLIST);

    if (enc == OBJ_ENCODING_QUICKLIST) {
        size_t zlen = server.list_max_ziplist_size;
        int depth = server.list_compress_depth;
        subject->ptr = quicklistCreateFromZiplist(zlen, depth, subject->ptr);
        subject->encoding = OBJ_ENCODING_QUICKLIST;
    } else {
        serverPanic("Unsupported list conversion");
    }
}

/*-----------------------------------------------------------------------------
 * List Commands
 *----------------------------------------------------------------------------*/
//PUSH命令
void pushGenericCommand(client *c, int where) {
    int j, waiting = 0, pushed = 0;
    //以写操作读取key对象的value
    robj *lobj = lookupKeyWrite(c->db,c->argv[1]);

    if (lobj && lobj->type != OBJ_LIST) {
        addReply(c,shared.wrongtypeerr);
        return;
    }

    for (j = 2; j < c->argc; j++) {//push值 遍历
        c->argv[j] = tryObjectEncoding(c->argv[j]);//编码value值
        if (!lobj) {//如果key存在
            lobj = createQuicklistObject();
            //创建一个quicklist,指定配置文件设置的list_max_ziplist_size 跟压缩深度 list_compress_depth
            quicklistSetOptions(lobj->ptr, server.list_max_ziplist_size,
                                server.list_compress_depth);
            //添加一个key/value 对
            dbAdd(c->db,c->argv[1],lobj);
        }
        listTypePush(lobj,c->argv[j],where);
        pushed++;
    }
    //发送当前列表中元素的个数
    addReplyLongLong(c, waiting + (lobj ? listTypeLength(lobj) : 0));
    if (pushed) {//有push
        char *event = (where == LIST_HEAD) ? "lpush" : "rpush";

        signalModifiedKey(c->db,c->argv[1]);
        //发送"lpush"或"rpush"事件通知
        notifyKeyspaceEvent(NOTIFY_LIST,event,c->argv[1],c->db->id);
    }
    server.dirty += pushed;
}

void lpushCommand(client *c) {
    pushGenericCommand(c,LIST_HEAD);
}

void rpushCommand(client *c) {
    pushGenericCommand(c,LIST_TAIL);
}


//pushx命令 exist 在refval前/后 push
void pushxGenericCommand(client *c, robj *refval, robj *val, int where) {
    robj *subject;
    listTypeIterator *iter;
    listTypeEntry entry;
    int inserted = 0;

    if ((subject = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,subject,OBJ_LIST)) return;

    if (refval != NULL) {
        /* Seek refval from head to tail */
        iter = listTypeInitIterator(subject,0,LIST_TAIL);
        while (listTypeNext(iter,&entry)) {
            if (listTypeEqual(&entry,refval)) {
                listTypeInsert(&entry,val,where);
                inserted = 1;
                break;
            }
        }
        listTypeReleaseIterator(iter);

        if (inserted) {
            signalModifiedKey(c->db,c->argv[1]);
            notifyKeyspaceEvent(NOTIFY_LIST,"linsert",
                                c->argv[1],c->db->id);
            server.dirty++;
        } else {
            /* Notify client of a failed insert */
            addReply(c,shared.cnegone);
            return;
        }
    } else {//refval 为空,则push
        char *event = (where == LIST_HEAD) ? "lpush" : "rpush";

        listTypePush(subject,val,where);
        signalModifiedKey(c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_LIST,event,c->argv[1],c->db->id);
        server.dirty++;
    }

    addReplyLongLong(c,listTypeLength(subject));
}

void lpushxCommand(client *c) {
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    pushxGenericCommand(c,NULL,c->argv[2],LIST_HEAD);
}

void rpushxCommand(client *c) {
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    pushxGenericCommand(c,NULL,c->argv[2],LIST_TAIL);
}


//在列表元素的前/后 插入元素
void linsertCommand(client *c) {
    c->argv[4] = tryObjectEncoding(c->argv[4]);
    if (strcasecmp(c->argv[2]->ptr,"after") == 0) {
        pushxGenericCommand(c,c->argv[3],c->argv[4],LIST_TAIL);
    } else if (strcasecmp(c->argv[2]->ptr,"before") == 0) {
        pushxGenericCommand(c,c->argv[3],c->argv[4],LIST_HEAD);
    } else {
        addReply(c,shared.syntaxerr);
    }
}

void llenCommand(client *c) {
    robj *o = lookupKeyReadOrReply(c,c->argv[1],shared.czero);
    if (o == NULL || checkType(c,o,OBJ_LIST)) return;
    addReplyLongLong(c,listTypeLength(o));
}

//index对应元素
void lindexCommand(client *c) {
    robj *o = lookupKeyReadOrReply(c,c->argv[1],shared.nullbulk);
    if (o == NULL || checkType(c,o,OBJ_LIST)) return;
    long index;
    robj *value = NULL;

    if ((getLongFromObjectOrReply(c, c->argv[2], &index, NULL) != C_OK))
        return;

    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklistEntry entry;
        if (quicklistIndex(o->ptr, index, &entry)) {
            if (entry.value) {
                value = createStringObject((char*)entry.value,entry.sz);
            } else {
                value = createStringObjectFromLongLong(entry.longval);
            }
            addReplyBulk(c,value);
            decrRefCount(value);
        } else {
            addReply(c,shared.nullbulk);
        }
    } else {
        serverPanic("Unknown list encoding");
    }
}

//set index 的元素
void lsetCommand(client *c) {
    robj *o = lookupKeyWriteOrReply(c,c->argv[1],shared.nokeyerr);
    if (o == NULL || checkType(c,o,OBJ_LIST)) return;
    long index;
    robj *value = c->argv[3];

    if ((getLongFromObjectOrReply(c, c->argv[2], &index, NULL) != C_OK))
        return;

    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklist *ql = o->ptr;
        int replaced = quicklistReplaceAtIndex(ql, index,
                                               value->ptr, sdslen(value->ptr));
        if (!replaced) {
            addReply(c,shared.outofrangeerr);
        } else {
            addReply(c,shared.ok);
            signalModifiedKey(c->db,c->argv[1]);
            notifyKeyspaceEvent(NOTIFY_LIST,"lset",c->argv[1],c->db->id);
            server.dirty++;
        }
    } else {
        serverPanic("Unknown list encoding");
    }
}

//POP 命令
void popGenericCommand(client *c, int where) {
    robj *o = lookupKeyWriteOrReply(c,c->argv[1],shared.nullbulk);
    if (o == NULL || checkType(c,o,OBJ_LIST)) return;

    //从where 弹出一个value
    robj *value = listTypePop(o,where);
    if (value == NULL) {//如果value为空,则发送空信息
        addReply(c,shared.nullbulk);
    } else {
        char *event = (where == LIST_HEAD) ? "lpop" : "rpop";
        //发送value给client
        addReplyBulk(c,value);
        //释放value对象
        decrRefCount(value);
        notifyKeyspaceEvent(NOTIFY_LIST,event,c->argv[1],c->db->id);
        if (listTypeLength(o) == 0) {//pop一个元素后,为空
            notifyKeyspaceEvent(NOTIFY_GENERIC,"del",
                                c->argv[1],c->db->id);
            dbDelete(c->db,c->argv[1]);//删除key
        }
        signalModifiedKey(c->db,c->argv[1]);
        server.dirty++;
    }
}

void lpopCommand(client *c) {
    popGenericCommand(c,LIST_HEAD);
}

void rpopCommand(client *c) {
    popGenericCommand(c,LIST_TAIL);
}

void lrangeCommand(client *c) {//range 获取
    robj *o;
    long start, end, llen, rangelen;

    if ((getLongFromObjectOrReply(c, c->argv[2], &start, NULL) != C_OK) ||
        (getLongFromObjectOrReply(c, c->argv[3], &end, NULL) != C_OK)) return;

    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptymultibulk)) == NULL
         || checkType(c,o,OBJ_LIST)) return;
    llen = listTypeLength(o);

    /* convert negative indexes */
    if (start < 0) start = llen+start;
    if (end < 0) end = llen+end;
    if (start < 0) start = 0;

    /* Invariant: start >= 0, so this test will be true when end < 0.
     * The range is empty when start > end or start >= length. */
    if (start > end || start >= llen) {
        addReply(c,shared.emptymultibulk);
        return;
    }
    if (end >= llen) end = llen-1;
    rangelen = (end-start)+1;

    /* Return the result in form of a multi-bulk reply */
    addReplyMultiBulkLen(c,rangelen);
    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
        listTypeIterator *iter = listTypeInitIterator(o, start, LIST_TAIL);

        while(rangelen--) {
            listTypeEntry entry;
            listTypeNext(iter, &entry);
            quicklistEntry *qe = &entry.entry;
            if (qe->value) {
                addReplyBulkCBuffer(c,qe->value,qe->sz);
            } else {
                addReplyBulkLongLong(c,qe->longval);
            }
        }
        listTypeReleaseIterator(iter);
    } else {
        serverPanic("List encoding is not QUICKLIST!");
    }
}

void ltrimCommand(client *c) {//移出左右 range
    robj *o;
    long start, end, llen, ltrim, rtrim;

    if ((getLongFromObjectOrReply(c, c->argv[2], &start, NULL) != C_OK) ||
        (getLongFromObjectOrReply(c, c->argv[3], &end, NULL) != C_OK)) return;

    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.ok)) == NULL ||
        checkType(c,o,OBJ_LIST)) return;
    llen = listTypeLength(o);

    /* convert negative indexes */
    if (start < 0) start = llen+start;
    if (end < 0) end = llen+end;
    if (start < 0) start = 0;

    /* Invariant: start >= 0, so this test will be true when end < 0.
     * The range is empty when start > end or start >= length. */
    if (start > end || start >= llen) {
        /* Out of range start or start > end result in empty list */
        ltrim = llen;
        rtrim = 0;
    } else {
        if (end >= llen) end = llen-1;
        ltrim = start;
        rtrim = llen-end-1;
    }

    /* Remove list elements to perform the trim */
    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklistDelRange(o->ptr,0,ltrim);
        quicklistDelRange(o->ptr,-rtrim,rtrim);
    } else {
        serverPanic("Unknown list encoding");
    }

    notifyKeyspaceEvent(NOTIFY_LIST,"ltrim",c->argv[1],c->db->id);
    if (listTypeLength(o) == 0) {
        dbDelete(c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",c->argv[1],c->db->id);
    }
    signalModifiedKey(c->db,c->argv[1]);
    server.dirty++;
    addReply(c,shared.ok);
}

void lremCommand(client *c) {//移出range
    robj *subject, *obj;
    obj = c->argv[3];
    long toremove;
    long removed = 0;

    if ((getLongFromObjectOrReply(c, c->argv[2], &toremove, NULL) != C_OK))
        return;

    subject = lookupKeyWriteOrReply(c,c->argv[1],shared.czero);
    if (subject == NULL || checkType(c,subject,OBJ_LIST)) return;

    listTypeIterator *li;
    if (toremove < 0) {
        toremove = -toremove;
        li = listTypeInitIterator(subject,-1,LIST_HEAD);
    } else {
        li = listTypeInitIterator(subject,0,LIST_TAIL);
    }

    listTypeEntry entry;
    while (listTypeNext(li,&entry)) {
        if (listTypeEqual(&entry,obj)) {
            listTypeDelete(li, &entry);
            server.dirty++;
            removed++;
            if (toremove && removed == toremove) break;
        }
    }
    listTypeReleaseIterator(li);

    if (removed) {
        signalModifiedKey(c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_GENERIC,"lrem",c->argv[1],c->db->id);
    }

    if (listTypeLength(subject) == 0) {
        dbDelete(c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",c->argv[1],c->db->id);
    }

    addReplyLongLong(c,removed);
}

/* This is the semantic of this command:
 *  RPOPLPUSH srclist dstlist:
 *    IF LLEN(srclist) > 0
 *      element = RPOP srclist
 *      LPUSH dstlist element
 *      RETURN element
 *    ELSE
 *      RETURN nil
 *    END
 *  END
 *
 * The idea is to be able to get an element from a list in a reliable way
 * since the element is not just returned but pushed against another list
 * as well. This command was originally proposed by Ezra Zygmuntowicz.
 */

void rpoplpushHandlePush(client *c, robj *dstkey, robj *dstobj, robj *value) {
    /* Create the list if the key does not exist */
    if (!dstobj) {
        dstobj = createQuicklistObject();
        quicklistSetOptions(dstobj->ptr, server.list_max_ziplist_size,
                            server.list_compress_depth);
        dbAdd(c->db,dstkey,dstobj);
    }
    signalModifiedKey(c->db,dstkey);
    //push 到头部
    listTypePush(dstobj,value,LIST_HEAD);
    notifyKeyspaceEvent(NOTIFY_LIST,"lpush",dstkey,c->db->id);
    /* Always send the pushed value to the client. */
    addReplyBulk(c,value);
}
//rpoplpush 命令
//移除列表的最后一个元素,并将该元素添加到另一个列表并返回
void rpoplpushCommand(client *c) {
    robj *sobj, *value;
    if ((sobj = lookupKeyWriteOrReply(c,c->argv[1],shared.nullbulk)) == NULL ||
        checkType(c,sobj,OBJ_LIST)) return;

    if (listTypeLength(sobj) == 0) {
        /* This may only happen after loading very old RDB files. Recent
         * versions of Redis delete keys of empty lists. */
        addReply(c,shared.nullbulk);
    } else {
        robj *dobj = lookupKeyWrite(c->db,c->argv[2]);
        robj *touchedkey = c->argv[1];

        if (dobj && checkType(c,dobj,OBJ_LIST)) return;
        value = listTypePop(sobj,LIST_TAIL);
        /* We saved touched key, and protect it, since rpoplpushHandlePush
         * may change the client command argument vector (it does not
         * currently). */
        incrRefCount(touchedkey);
        rpoplpushHandlePush(c,c->argv[2],dobj,value);

        /* listTypePop returns an object with its refcount incremented */
        decrRefCount(value);

        /* Delete the source list when it is empty */
        notifyKeyspaceEvent(NOTIFY_LIST,"rpop",touchedkey,c->db->id);
        if (listTypeLength(sobj) == 0) {
            dbDelete(c->db,touchedkey);
            notifyKeyspaceEvent(NOTIFY_GENERIC,"del",
                                touchedkey,c->db->id);
        }
        signalModifiedKey(c->db,touchedkey);
        decrRefCount(touchedkey);
        server.dirty++;
    }
}

/*-----------------------------------------------------------------------------
 * Blocking POP operations
 *----------------------------------------------------------------------------*/

/* This is how the current blocking POP works, we use BLPOP as example:
 * - If the user calls BLPOP and the key exists and contains a non empty list
 *   then LPOP is called instead. So BLPOP is semantically the same as LPOP
 *   if blocking is not required.
 * - If instead BLPOP is called and the key does not exists or the list is
 *   empty we need to block. In order to do so we remove the notification for
 *   new data to read in the client socket (so that we'll not serve new
 *   requests if the blocking request is not served). Also we put the client
 *   in a dictionary (db->blocking_keys) mapping keys to a list of clients
 *   blocking for this keys.
 * - If a PUSH operation against a key with blocked clients waiting is
 *   performed, we mark this key as "ready", and after the current command,
 *   MULTI/EXEC block, or script, is executed, we serve all the clients waiting
 *   for this list, from the one that blocked first, to the last, accordingly
 *   to the number of elements we have in the ready list.
 */

/* Set a client in blocking mode for the specified key, with the specified
 * timeout */


/****
 * 阻塞的操作
 * 
 * 
 * 
 * 
 * 如果所有给定的key中不存在,或者key中包含的是空列表,
 * 那么 BLPOP 或 BLPOP 命令将会被阻塞连接,
 * 直到另一个client对这些key中执行 [LR]PUSH 命令将一个新数据出现在任意key的列表中,
 * 那么这个命令会解除调用BLPOP 或 BLPOP 命令的client的阻塞状态。
 * 
 * 
 * ****/



// keys是一个key的数组，个数为numkeys个
// timeout保存超时时间
// target保存PUSH入元素的键，也就是dstkey，用于BRPOPLPUSH函数
// 根据给定的key将client阻塞
void blockForKeys(client *c, robj **keys, int numkeys, mstime_t timeout, robj *target) {
    dictEntry *de;
    list *l;
    int j;


    //设置超时时间和target
    c->bpop.timeout = timeout;
    c->bpop.target = target;

    if (target != NULL) incrRefCount(target);


    //当前client与numkeys造成client阻塞的键关联起来
    for (j = 0; j < numkeys; j++) {
        /* If the key already exists in the dict ignore it. */

        //阻塞的key 放入c->bpop.keys 的dict中
        if (dictAdd(c->bpop.keys,keys[j],NULL) != DICT_OK) continue;
        //当前的key引用计数加1
        incrRefCount(keys[j]);

        /* And in the other "side", to map keys -> clients */
        // c->db->blocking_keys keys与因其阻塞的client 的list的映射 
        de = dictFind(c->db->blocking_keys,keys[j]);
        if (de == NULL) {
            int retval;

            /* For every key we take a list of clients blocked for it */
            //创建一个双向链表
            l = listCreate();
            //将造成阻塞的键和列表添加到db->blocking_keys字典中
            retval = dictAdd(c->db->blocking_keys,keys[j],l);
            incrRefCount(keys[j]);
            serverAssertWithInfo(c,keys[j],retval == DICT_OK);
        } else {
            l = dictGetVal(de);
        }
        //将当前client加入到阻塞的client的列表
        listAddNodeTail(l,c);
    }
    blockClient(c,BLOCKED_LIST);//阻塞client
}

/* Unblock a client that's waiting in a blocking operation such as BLPOP.
 * You should never call this function directly, but unblockClient() instead. */

//解阻塞一个正在阻塞的client
void unblockClientWaitingData(client *c) {
    dictEntry *de;
    dictIterator *di;
    list *l;

    serverAssertWithInfo(c,NULL,dictSize(c->bpop.keys) != 0);
    di = dictGetIterator(c->bpop.keys);
    /* The client may wait for multiple keys, so unblock it for every key. */
    while((de = dictNext(di)) != NULL) {
        robj *key = dictGetKey(de);

        /* Remove this client from the list of clients waiting for this key. */
        //删除c->db->blocking_keys 的list中的 client
        l = dictFetchValue(c->db->blocking_keys,key);
        serverAssertWithInfo(c,key,l != NULL);
        listDelNode(l,listSearchKey(l,c));
        /* If the list is empty we need to remove it to avoid wasting memory */
        if (listLength(l) == 0)
            dictDelete(c->db->blocking_keys,key);
    }
    dictReleaseIterator(di);

    /* Cleanup the client structure */
    //清空 c->bpop.keys
    dictEmpty(c->bpop.keys,NULL);
    if (c->bpop.target) {
        decrRefCount(c->bpop.target);
        c->bpop.target = NULL;
    }
}

/* If the specified key has clients blocked waiting for list pushes, this
 * function will put the key reference into the server.ready_keys list.
 * Note that db->ready_keys is a hash table that allows us to avoid putting
 * the same key again and again in the list in case of multiple pushes
 * made by a script or in the context of MULTI/EXEC.
 *
 * The list will be finally processed by handleClientsBlockedOnLists() */

void signalListAsReady(redisDb *db, robj *key) {
    readyList *rl;

    /* No clients blocking for this key? No need to queue it. */
    if (dictFind(db->blocking_keys,key) == NULL) return;

    /* Key was already signaled? No need to queue it again. */
    if (dictFind(db->ready_keys,key) != NULL) return;

    /* Ok, we need to queue this key into server.ready_keys. */
    rl = zmalloc(sizeof(*rl));
    rl->key = key;
    rl->db = db;
    incrRefCount(key);
    listAddNodeTail(server.ready_keys,rl);

    /* We also add the key in the db->ready_keys dictionary in order
     * to avoid adding it multiple times into a list with a simple O(1)
     * check. */
    incrRefCount(key);
    serverAssert(dictAdd(db->ready_keys,key,NULL) == DICT_OK);
}

/* This is a helper function for handleClientsBlockedOnLists(). It's work
 * is to serve a specific client (receiver) that is blocked on 'key'
 * in the context of the specified 'db', doing the following:
 *
 * 1) Provide the client with the 'value' element.
 * 2) If the dstkey is not NULL (we are serving a BRPOPLPUSH) also push the
 *    'value' element on the destination list (the LPUSH side of the command).
 * 3) Propagate the resulting BRPOP, BLPOP and additional LPUSH if any into
 *    the AOF and replication channel.
 *
 * The argument 'where' is LIST_TAIL or LIST_HEAD, and indicates if the
 * 'value' element was popped fron the head (BLPOP) or tail (BRPOP) so that
 * we can propagate the command properly.
 *
 * The function returns C_OK if we are able to serve the client, otherwise
 * C_ERR is returned to signal the caller that the list POP operation
 * should be undone as the client was not served: This only happens for
 * BRPOPLPUSH that fails to push the value to the destination key as it is
 * of the wrong type. */

int serveClientBlockedOnList(client *receiver, robj *key, robj *dstkey, redisDb *db, robj *value, int where)
{
    robj *argv[3];

    if (dstkey == NULL) {
        /* Propagate the [LR]POP operation. */
        argv[0] = (where == LIST_HEAD) ? shared.lpop :
                                          shared.rpop;
        argv[1] = key;
        propagate((where == LIST_HEAD) ?
            server.lpopCommand : server.rpopCommand,
            db->id,argv,2,PROPAGATE_AOF|PROPAGATE_REPL);

        /* BRPOP/BLPOP */
        addReplyMultiBulkLen(receiver,2);
        addReplyBulk(receiver,key);
        addReplyBulk(receiver,value);
    } else {
        /* BRPOPLPUSH */
        robj *dstobj =
            lookupKeyWrite(receiver->db,dstkey);
        if (!(dstobj &&
             checkType(receiver,dstobj,OBJ_LIST)))
        {
            /* Propagate the RPOP operation. */
            argv[0] = shared.rpop;
            argv[1] = key;
            propagate(server.rpopCommand,
                db->id,argv,2,
                PROPAGATE_AOF|
                PROPAGATE_REPL);
            rpoplpushHandlePush(receiver,dstkey,dstobj,
                value);
            /* Propagate the LPUSH operation. */
            argv[0] = shared.lpush;
            argv[1] = dstkey;
            argv[2] = value;
            propagate(server.lpushCommand,
                db->id,argv,3,
                PROPAGATE_AOF|
                PROPAGATE_REPL);
        } else {
            /* BRPOPLPUSH failed because of wrong
             * destination type. */
            return C_ERR;
        }
    }
    return C_OK;
}

/* This function should be called by Redis every time a single command,
 * a MULTI/EXEC block, or a Lua script, terminated its execution after
 * being called by a client.
 *
 * All the keys with at least one client blocked that received at least
 * one new element via some PUSH operation are accumulated into
 * the server.ready_keys list. This function will run the list and will
 * serve clients accordingly. Note that the function will iterate again and
 * again as a result of serving BRPOPLPUSH we can have new blocking clients
 * to serve because of the PUSH side of BRPOPLPUSH. */
void handleClientsBlockedOnLists(void) {
    while(listLength(server.ready_keys) != 0) {
        list *l;

        /* Point server.ready_keys to a fresh list and save the current one
         * locally. This way as we run the old list we are free to call
         * signalListAsReady() that may push new elements in server.ready_keys
         * when handling clients blocked into BRPOPLPUSH. */
        l = server.ready_keys;
        server.ready_keys = listCreate();

        while(listLength(l) != 0) {
            listNode *ln = listFirst(l);
            readyList *rl = ln->value;

            /* First of all remove this key from db->ready_keys so that
             * we can safely call signalListAsReady() against this key. */
            dictDelete(rl->db->ready_keys,rl->key);

            /* If the key exists and it's a list, serve blocked clients
             * with data. */
            robj *o = lookupKeyWrite(rl->db,rl->key);
            if (o != NULL && o->type == OBJ_LIST) {
                dictEntry *de;

                /* We serve clients in the same order they blocked for
                 * this key, from the first blocked to the last. */
                de = dictFind(rl->db->blocking_keys,rl->key);
                if (de) {
                    list *clients = dictGetVal(de);
                    int numclients = listLength(clients);

                    while(numclients--) {
                        listNode *clientnode = listFirst(clients);
                        client *receiver = clientnode->value;
                        robj *dstkey = receiver->bpop.target;
                        int where = (receiver->lastcmd &&
                                     receiver->lastcmd->proc == blpopCommand) ?
                                    LIST_HEAD : LIST_TAIL;
                        robj *value = listTypePop(o,where);

                        if (value) {
                            /* Protect receiver->bpop.target, that will be
                             * freed by the next unblockClient()
                             * call. */
                            if (dstkey) incrRefCount(dstkey);
                            unblockClient(receiver);

                            if (serveClientBlockedOnList(receiver,
                                rl->key,dstkey,rl->db,value,
                                where) == C_ERR)
                            {
                                /* If we failed serving the client we need
                                 * to also undo the POP operation. */
                                    listTypePush(o,value,where);
                            }

                            if (dstkey) decrRefCount(dstkey);
                            decrRefCount(value);
                        } else {
                            break;
                        }
                    }
                }

                if (listTypeLength(o) == 0) {
                    dbDelete(rl->db,rl->key);
                }
                /* We don't call signalModifiedKey() as it was already called
                 * when an element was pushed on the list. */
            }

            /* Free this item. */
            decrRefCount(rl->key);
            zfree(rl);
            listDelNode(l,ln);
        }
        listRelease(l); /* We have the new list on place at this point. */
    }
}

/* Blocking RPOP/LPOP */

//阻塞的 RPOP/LPOP 命令
void blockingPopGenericCommand(client *c, int where) {
    robj *o;
    mstime_t timeout;
    int j;

    // 以秒为单位保存timeout值
    if (getTimeoutFromObjectOrReply(c,c->argv[c->argc-1],&timeout,UNIT_SECONDS)
        != C_OK) return;


    //遍历所有的key，如果key中列表有值，则执行完这个循环一定能直接返回
    for (j = 1; j < c->argc-1; j++) {
        o = lookupKeyWrite(c->db,c->argv[j]);
        if (o != NULL) {
            if (o->type != OBJ_LIST) {
                addReply(c,shared.wrongtypeerr);
                return;
            } else {
                if (listTypeLength(o) != 0) {
                    /* Non empty list, this is like a non normal [LR]POP. */
                    char *event = (where == LIST_HEAD) ? "lpop" : "rpop";
                    robj *value = listTypePop(o,where);
                    serverAssert(value != NULL);

                    addReplyMultiBulkLen(c,2);
                    addReplyBulk(c,c->argv[j]);
                    addReplyBulk(c,value);
                    decrRefCount(value);
                    //lpop/rpop 事件通知
                    notifyKeyspaceEvent(NOTIFY_LIST,event,
                                        c->argv[j],c->db->id);
                    if (listTypeLength(o) == 0) {//pop 后length为0,删除
                        dbDelete(c->db,c->argv[j]);
                        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",
                                            c->argv[j],c->db->id);
                    }
                    signalModifiedKey(c->db,c->argv[j]);
                    server.dirty++;

                    /* Replicate it as an [LR]POP instead of B[LR]POP. */
                    // 传播一个[LR]POP 而不是B[LR]POP，修改client原来的命令参数
                    rewriteClientCommandVector(c,2,
                        (where == LIST_HEAD) ? shared.lpop : shared.rpop,
                        c->argv[j]);
                    return;
                }
            }
        }
    }

    /* If we are inside a MULTI/EXEC and the list is empty the only thing
     * we can do is treating it as a timeout (even with timeout 0). */
    // 如果命令在一个 事务 中执行，则发送一个空回复以避免死等待，因为要执行完整个事务块
    if (c->flags & CLIENT_MULTI) {
        addReply(c,shared.nullmultibulk);
        return;
    }

    /* If the list is empty or the key does not exists we must block */

    //所有的key 要么不存在,要么list为空,阻塞这些key
    blockForKeys(c, c->argv + 1, c->argc - 2, timeout, NULL);
}

void blpopCommand(client *c) {
    blockingPopGenericCommand(c,LIST_HEAD);
}

void brpopCommand(client *c) {
    blockingPopGenericCommand(c,LIST_TAIL);
}


// BRPOPLPUSH命令
void brpoplpushCommand(client *c) {
    mstime_t timeout;

    //以秒为单位取出超时时间
    if (getTimeoutFromObjectOrReply(c,c->argv[3],&timeout,UNIT_SECONDS)
        != C_OK) return;

    //以写操作读取出 source的值
    robj *key = lookupKeyWrite(c->db, c->argv[1]);

    //key为空 阻塞
    if (key == NULL) {
        if (c->flags & CLIENT_MULTI) {// 如果命令在一个事务中执行，则发送一个空回复以避免死等待
            /* Blocking against an empty list in a multi state
             * returns immediately. */
            addReply(c, shared.nullbulk);
        } else {
            /* The list is empty and the client blocks. */
            // 列表为空，则将client阻塞
            blockForKeys(c, c->argv + 1, 1, timeout, c->argv[2]);
        }
    } else {
        if (key->type != OBJ_LIST) {
            addReply(c, shared.wrongtypeerr);
        } else {
            /* The list exists and has elements, so
             * the regular rpoplpushCommand is executed. */
            serverAssertWithInfo(c,key,listTypeLength(key) > 0);
            rpoplpushCommand(c);
        }
    }
}
