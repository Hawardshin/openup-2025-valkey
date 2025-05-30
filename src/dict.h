/* Hash Tables Implementation.
 *
 * This file implements in-memory hash tables with insert/del/replace/find/
 * get-random-element operations. Hash tables will auto-resize if needed
 * tables of power of two in size are used, collisions are handled by
 * chaining. See the source code for more information... :)
 *
 * Copyright (c) 2006-2012, Redis Ltd.
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

#ifndef __DICT_H
#define __DICT_H

#include "mt19937-64.h"
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

#define DICT_OK 0
#define DICT_ERR 1

/* Hash table parameters */
#define HASHTABLE_MIN_FILL 8 /* Minimal hash table fill 12.5%(100/8) */

typedef struct dictEntry dictEntry; /* opaque */
typedef struct dict dict;

typedef struct dictType {
    /* Callbacks */
    uint64_t (*hashFunction)(const void *key);
    void *(*keyDup)(const void *key);
    int (*keyCompare)(const void *key1, const void *key2);
    void (*keyDestructor)(void *key);
    void (*valDestructor)(void *obj);
    int (*resizeAllowed)(size_t moreMem, double usedRatio);
    /* Invoked at the start of dict initialization/rehashing (old and new ht are already created) */
    void (*rehashingStarted)(dict *d);
    /* Invoked at the end of dict initialization/rehashing of all the entries from old to new ht. Both ht still exists
     * and are cleaned up after this callback.  */
    void (*rehashingCompleted)(dict *d);
    /* Allow a dict to carry extra caller-defined metadata. The
     * extra memory is initialized to 0 when a dict is allocated. */
    size_t (*dictMetadataBytes)(dict *d);
    /* Method for copying a given key into a buffer of buf_len. Also used for
     * computing the length of the key + header when buf is NULL. */
    size_t (*embedKey)(unsigned char *buf, size_t buf_len, const void *key, unsigned char *header_size);

    /* Flags */
    /* The 'no_value' flag, if set, indicates that values are not used, i.e. the
     * dict is a set. When this flag is set, it's not possible to access the
     * value of a dictEntry and it's also impossible to use dictSetKey(). Entry
     * metadata can also not be used. */
    unsigned int no_value : 1;
    /* If no_value = 1 and all keys are odd (LSB=1), setting keys_are_odd = 1
     * enables one more optimization: to store a key without an allocated
     * dictEntry. */
    unsigned int keys_are_odd : 1;
    /* If embedded_entry flag is set, it indicates that a copy of the key is created and the key is embedded
     * as part of the dict entry. */
    unsigned int embedded_entry : 1;
    /* Perform rehashing during resizing instead of incrementally rehashing across multiple steps */
    unsigned int no_incremental_rehash : 1;
} dictType;

#define DICTHT_SIZE(exp) ((exp) == -1 ? 0 : (unsigned long)1 << (exp))
#define DICTHT_SIZE_MASK(exp) ((exp) == -1 ? 0 : (DICTHT_SIZE(exp)) - 1)

struct dict {
    dictType *type;

    dictEntry **ht_table[2];
    unsigned long ht_used[2];

    long rehashidx; /* rehashing not in progress if rehashidx == -1 */

    /* Keep small vars at end for optimal (minimal) struct padding */
    int16_t pauserehash;        /* If >0 rehashing is paused (<0 indicates coding error) */
    signed char ht_size_exp[2]; /* exponent of size. (size = 1<<exp) */
    int16_t pauseAutoResize;    /* If >0 automatic resizing is disallowed (<0 indicates coding error) */
    void *metadata[];
};

/* If safe is set to 1 this is a safe iterator, that means, you can call
 * dictAdd, dictFind, and other functions against the dictionary even while
 * iterating. Otherwise it is a non safe iterator, and only dictNext()
 * should be called while iterating. */
typedef struct dictIterator {
    dict *d;
    long index;
    int table, safe;
    dictEntry *entry, *nextEntry;
    /* unsafe iterator fingerprint for misuse detection. */
    unsigned long long fingerprint;
} dictIterator;

typedef struct dictStats {
    int htidx;
    unsigned long buckets;
    unsigned long maxChainLen;
    unsigned long totalChainLen;
    unsigned long htSize;
    unsigned long htUsed;
    unsigned long *clvector;
} dictStats;

typedef void(dictScanFunction)(void *privdata, const dictEntry *de);
typedef void *(dictDefragAllocFunction)(void *ptr);
typedef void(dictDefragEntryCb)(void *privdata, void *ptr);
typedef struct {
    dictDefragAllocFunction *defragAlloc;   /* Used for entries etc. */
    dictDefragAllocFunction *defragKey;     /* Defrag-realloc keys (optional) */
    dictDefragAllocFunction *defragVal;     /* Defrag-realloc values (optional) */
    dictDefragEntryCb *defragEntryStartCb;  /* Callback invoked prior to the start of defrag of dictEntry. */
    dictDefragEntryCb *defragEntryFinishCb; /* Callback invoked after the defrag of dictEntry is tried. */
} dictDefragFunctions;

/* This is the initial size of every hash table */
#define DICT_HT_INITIAL_EXP 2
#define DICT_HT_INITIAL_SIZE (1 << (DICT_HT_INITIAL_EXP))

/* ------------------------------- Macros ------------------------------------*/
static inline int dictCompareKeys(dict *d, const void *key1, const void *key2) {
    if (d->type->keyCompare) {
        return d->type->keyCompare(key1, key2);
    } else {
        return (key1 == key2);
    }
}

#define dictMetadata(d) (&(d)->metadata)
#define dictMetadataSize(d) ((d)->type->dictMetadataBytes ? (d)->type->dictMetadataBytes(d) : 0)

#define dictHashKey(d, key) ((d)->type->hashFunction(key))
#define dictBuckets(d) (DICTHT_SIZE((d)->ht_size_exp[0]) + DICTHT_SIZE((d)->ht_size_exp[1]))
#define dictSize(d) ((d)->ht_used[0] + (d)->ht_used[1])
#define dictIsEmpty(d) ((d)->ht_used[0] == 0 && (d)->ht_used[1] == 0)
#define dictIsRehashing(d) ((d)->rehashidx != -1)
#define dictPauseRehashing(d) ((d)->pauserehash++)
#define dictResumeRehashing(d) ((d)->pauserehash--)
#define dictIsRehashingPaused(d) ((d)->pauserehash > 0)
#define dictPauseAutoResize(d) ((d)->pauseAutoResize++)
#define dictResumeAutoResize(d) ((d)->pauseAutoResize--)

/* If our unsigned long type can store a 64 bit number, use a 64 bit PRNG. */
#if ULONG_MAX >= 0xffffffffffffffff
#define randomULong() ((unsigned long)genrand64_int64())
#else
#define randomULong() random()
#endif

typedef enum {
    DICT_RESIZE_ENABLE,
    DICT_RESIZE_AVOID,
    DICT_RESIZE_FORBID,
} dictResizeEnable;

/* API */
dict *dictCreate(dictType *type);
int dictExpand(dict *d, unsigned long size);
int dictTryExpand(dict *d, unsigned long size);
int dictShrink(dict *d, unsigned long size);
int dictAdd(dict *d, void *key, void *val);
dictEntry *dictAddRaw(dict *d, void *key, dictEntry **existing);
void *dictFindPositionForInsert(dict *d, const void *key, dictEntry **existing);
dictEntry *dictInsertAtPosition(dict *d, void *key, void *position);
dictEntry *dictAddOrFind(dict *d, void *key);
int dictReplace(dict *d, void *key, void *val);
int dictDelete(dict *d, const void *key);
dictEntry *dictUnlink(dict *d, const void *key);
void dictFreeUnlinkedEntry(dict *d, dictEntry *he);
dictEntry *dictTwoPhaseUnlinkFind(dict *d, const void *key, dictEntry ***plink, int *table_index);
void dictTwoPhaseUnlinkFree(dict *d, dictEntry *he, dictEntry **plink, int table_index);
void dictRelease(dict *d);
dictEntry *dictFind(dict *d, const void *key);
void *dictFetchValue(dict *d, const void *key);
int dictShrinkIfNeeded(dict *d);
int dictExpandIfNeeded(dict *d);
void dictSetKey(dict *d, dictEntry *de, void *key);
void dictSetVal(dict *d, dictEntry *de, void *val);
void dictSetSignedIntegerVal(dictEntry *de, int64_t val);
void dictSetUnsignedIntegerVal(dictEntry *de, uint64_t val);
void dictSetDoubleVal(dictEntry *de, double val);
int64_t dictIncrSignedIntegerVal(dictEntry *de, int64_t val);
uint64_t dictIncrUnsignedIntegerVal(dictEntry *de, uint64_t val);
double dictIncrDoubleVal(dictEntry *de, double val);
void *dictGetKey(const dictEntry *de);
void *dictGetVal(const dictEntry *de);
int64_t dictGetSignedIntegerVal(const dictEntry *de);
uint64_t dictGetUnsignedIntegerVal(const dictEntry *de);
double dictGetDoubleVal(const dictEntry *de);
double *dictGetDoubleValPtr(dictEntry *de);
size_t dictMemUsage(const dict *d);
size_t dictEntryMemUsage(dictEntry *de);
dictIterator *dictGetIterator(dict *d);
dictIterator *dictGetSafeIterator(dict *d);
void dictInitIterator(dictIterator *iter, dict *d);
void dictInitSafeIterator(dictIterator *iter, dict *d);
void dictResetIterator(dictIterator *iter);
dictEntry *dictNext(dictIterator *iter);
dictEntry *dictGetNext(const dictEntry *de);
void dictReleaseIterator(dictIterator *iter);
dictEntry *dictGetRandomKey(dict *d);
dictEntry *dictGetFairRandomKey(dict *d);
unsigned int dictGetSomeKeys(dict *d, dictEntry **des, unsigned int count);
void dictGetStats(char *buf, size_t bufsize, dict *d, int full);
uint64_t dictGenHashFunction(const void *key, size_t len);
uint64_t dictGenCaseHashFunction(const unsigned char *buf, size_t len);
void dictEmpty(dict *d, void(callback)(dict *));
void dictSetResizeEnabled(dictResizeEnable enable);
int dictRehash(dict *d, int n);
int dictRehashMicroseconds(dict *d, uint64_t us);
void dictSetHashFunctionSeed(uint8_t *seed);
uint8_t *dictGetHashFunctionSeed(void);
unsigned long dictScan(dict *d, unsigned long v, dictScanFunction *fn, void *privdata);
unsigned long
dictScanDefrag(dict *d, unsigned long v, dictScanFunction *fn, const dictDefragFunctions *defragfns, void *privdata);
uint64_t dictGetHash(dict *d, const void *key);
void dictRehashingInfo(dict *d, unsigned long long *from_size, unsigned long long *to_size);

size_t dictGetStatsMsg(char *buf, size_t bufsize, dictStats *stats, int full);
dictStats *dictGetStatsHt(dict *d, int htidx, int full);
void dictCombineStats(dictStats *from, dictStats *into);
void dictFreeStats(dictStats *stats);

#endif /* __DICT_H */
