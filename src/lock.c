/*
 * Copyright (C) 2003, 2004, 2005 Robert Lougher <rob@lougher.demon.co.uk>.
 *
 * This file is part of JamVM.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2,
 * or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/time.h>
#include <limits.h>

#include "jam.h"
#include "thread.h"
#include "hash.h"
#include "alloc.h"

/* Trace lock operations and inflation/deflation */
#ifdef TRACELOCK
#define TRACE(x) printf x
#else
#define TRACE(x)
#endif

#define UN_USED -1

#define HASHTABSZE 1<<5
#define HASH(obj) ((uintptr_t) obj >> LOG_OBJECT_GRAIN)
#define COMPARE(obj, mon, hash1, hash2) hash1 == hash2
#define PREPARE(obj) allocMonitor(obj)
#define FOUND(ptr) COMPARE_AND_SWAP(&ptr->entering, UN_USED, 0)

/* lockword format in "thin" mode
  31                                         0
   -------------------------------------------
  |              thread ID          | count |0|
   -------------------------------------------
                                             ^ shape bit

  lockword format in "fat" mode
  31                                         0
   -------------------------------------------
  |                 Monitor*                |1|
   -------------------------------------------
                                             ^ shape bit
*/

#define SHAPE_BIT   0x1
#define COUNT_SIZE  8
#define COUNT_SHIFT 1
#define COUNT_MASK  (((1<<COUNT_SIZE)-1)<<COUNT_SHIFT)

#define TID_SHIFT   (COUNT_SIZE+COUNT_SHIFT)
#define TID_SIZE    (32-TID_SHIFT)
#define TID_MASK    (((1<<TID_SIZE)-1)<<TID_SHIFT)

#define SCAVENGE(ptr)                                 \
({                                                    \
    Monitor *mon = (Monitor *)ptr;                    \
    char res = ATOMIC_READ(&mon->entering) == UN_USED;\
    if(res) {                                         \
        TRACE(("Scavenging monitor %p (obj %p)", mon, \
                                          mon->obj)); \
        mon->next = mon_free_list;                    \
        mon_free_list = mon;                          \
    }                                                 \
    res;                                              \
})

static Monitor *mon_free_list = NULL;
static HashTable mon_cache;

void monitorInit(Monitor *mon) {
    pthread_mutex_init(&mon->lock, NULL);
    pthread_cond_init(&mon->cv, NULL);
    mon->owner = 0;
    mon->count = 0;
    mon->waiting = 0;
    mon->notifying = 0;
    mon->interrupting = 0;
}

void monitorLock(Monitor *mon, Thread *self) {
    if(mon->owner == self)
        mon->count++;
    else {
        disableSuspend(self);
        self->state = WAITING;
        pthread_mutex_lock(&mon->lock);
        self->state = RUNNING;
        enableSuspend(self);
        mon->owner = self;
    }
}

int monitorTryLock(Monitor *mon, Thread *self) {
    if(mon->owner == self)
        mon->count++;
    else {
        if(pthread_mutex_trylock(&mon->lock))
            return FALSE;
        mon->owner = self;
    }

    return TRUE;
}

void monitorUnlock(Monitor *mon, Thread *self) {
    if(mon->owner == self) {
        if(mon->count == 0) {
            mon->owner = 0;
            pthread_mutex_unlock(&mon->lock);
        } else
            mon->count--;
    }
}

int monitorWait(Monitor *mon, Thread *self, long long ms, int ns) {
    char interrupted = 0;
    int old_count;
    char timed = (ms != 0) || (ns != 0);
    struct timespec ts;

    if(mon->owner != self)
        return FALSE;

    /* We own the monitor */

    disableSuspend(self);

    old_count = mon->count;
    mon->count = 0;
    mon->owner = NULL;
    mon->waiting++;

    if(timed) {
        struct timeval tv;
        long long seconds;

        gettimeofday(&tv, 0);

        seconds = tv.tv_sec + ms/1000;
        ts.tv_nsec = (tv.tv_usec + ((ms%1000)*1000))*1000 + ns;

        if(ts.tv_nsec > 999999999L) {
            seconds++;
            ts.tv_nsec -= 1000000000L;
        }

        /* If the number of seconds is too large just set
           it to the max value instead of truncating.
           Depending on result, we may not wait at all */
       ts.tv_sec = seconds > LONG_MAX ? LONG_MAX : seconds;
    }

    self->wait_mon = mon;
    self->state = WAITING;

    if(self->interrupted)
        interrupted = TRUE;
    else {

wait_loop:
        if(timed) {
            if(pthread_cond_timedwait(&mon->cv, &mon->lock, &ts) == ETIMEDOUT)
                goto out;

            /* Sigjmp/longjmp resets fpu control on i386 --
             * empty for sane platforms. */ 
            FPU_HACK;

        } else
            pthread_cond_wait(&mon->cv, &mon->lock);

        /* see why we were signalled... */

        if(self->interrupting) {
            interrupted = TRUE;
            self->interrupting = FALSE;
            mon->interrupting--;
        } else
            if(mon->notifying)
                mon->notifying--;
            else
                goto wait_loop;
    }
out:

    self->state = RUNNING;
    self->wait_mon = 0;

    mon->owner = self;
    mon->count = old_count;
    mon->waiting--;

    enableSuspend(self);

    if(interrupted) {
        self->interrupted = FALSE;
        signalException("java/lang/InterruptedException", NULL);
    }

    return TRUE;
}

int monitorNotify(Monitor *mon, Thread *self) {
    if(mon->owner != self)
        return FALSE;

    if((mon->notifying + mon->interrupting) < mon->waiting) {
        mon->notifying++;
        pthread_cond_signal(&mon->cv);
    }

    return TRUE;
}

int monitorNotifyAll(Monitor *mon, Thread *self) {
    if(mon->owner != self)
        return FALSE;

    mon->notifying = mon->waiting - mon->interrupting;
    pthread_cond_broadcast(&mon->cv);

    return TRUE;
}

Monitor *allocMonitor(Object *obj) {
    Monitor *mon;

    if(mon_free_list != NULL) {
        mon = mon_free_list;
        mon_free_list = mon->next;      
    } else {
        mon = (Monitor *)sysMalloc(sizeof(Monitor));
        monitorInit(mon);
    }
    mon->obj = obj;
    /* No need to wrap in ATOMIC_WRITE as no thread should
     * be modifying it when it's on the free list */
    mon->entering = 0;
    return mon;
}

Monitor *findMonitor(Object *obj) {
    uintptr_t lockword = ATOMIC_READ(&obj->lock);

    if(lockword & SHAPE_BIT)
        return (Monitor*) (lockword & ~SHAPE_BIT);
    else {
        Monitor *mon;
        /* Add if absent, scavenge, locked */
        findHashEntry(mon_cache, obj, mon, TRUE, TRUE, TRUE);
        return mon;
    }
}

static void inflate(Object *obj, Monitor *mon, Thread *self) {
    TRACE(("Thread %p is inflating obj %p...\n", self, obj));
    clearFlcBit(obj);
    monitorNotifyAll(mon, self);
    ATOMIC_WRITE(&obj->lock, (uintptr_t) mon | SHAPE_BIT);
}

void objectLock(Object *obj) {
    Thread *self = threadSelf();
    uintptr_t thin_locked = self->id<<TID_SHIFT;
    uintptr_t entering, lockword;
    Monitor *mon;

    TRACE(("Thread %p lock on obj %p...\n", self, obj));

    if(COMPARE_AND_SWAP(&obj->lock, 0, thin_locked)) {
        /* This barrier is not needed for the thin-locking implementation --
           it's a requirement of the Java memory model. */
        JMM_LOCK_MBARRIER();
        return;
    }

    lockword = ATOMIC_READ(&obj->lock);
    if((lockword & (TID_MASK|SHAPE_BIT)) == thin_locked) {
        int count = lockword & COUNT_MASK;

        if(count < (((1<<COUNT_SIZE)-1)<<COUNT_SHIFT))
            ATOMIC_WRITE(&obj->lock, lockword + (1<<COUNT_SHIFT));
        else {
            mon = findMonitor(obj);
            monitorLock(mon, self);
            inflate(obj, mon, self);
            mon->count = 1<<COUNT_SIZE;
        }
        return;
    }

try_again:
    mon = findMonitor(obj);

try_again2:
    if((entering = ATOMIC_READ(&mon->entering)) == UN_USED)
        goto try_again;

    if(!(COMPARE_AND_SWAP(&mon->entering, entering, entering+1)))
        goto try_again2;

    if(mon->obj != obj) {
        while(entering = ATOMIC_READ(&mon->entering),
                        !(COMPARE_AND_SWAP(&mon->entering, entering, entering-1)));
        goto try_again;
    }

    monitorLock(mon, self);

    while(entering = ATOMIC_READ(&mon->entering),
                    !(COMPARE_AND_SWAP(&mon->entering, entering, entering-1)));

    while((ATOMIC_READ(&obj->lock) & SHAPE_BIT) == 0) {
        setFlcBit(obj);

        if(COMPARE_AND_SWAP(&obj->lock, 0, thin_locked))
            inflate(obj, mon, self);
        else
            monitorWait(mon, self, 0, 0);
    }
}

void objectUnlock(Object *obj) {
    Thread *self = threadSelf();
    uintptr_t lockword = ATOMIC_READ(&obj->lock);
    uintptr_t thin_locked = self->id<<TID_SHIFT;

    TRACE(("Thread %p unlock on obj %p...\n", self, obj));

    if(lockword == thin_locked) {
        /* This barrier is not needed for the thin-locking implementation --
           it's a requirement of the Java memory model. */
        JMM_UNLOCK_MBARRIER();
        ATOMIC_WRITE(&obj->lock, 0);

        /* Required by thin-locking mechanism. */
        UNLOCK_MBARRIER();

retry:
        if(testFlcBit(obj)) {
            Monitor *mon = findMonitor(obj);

            if(!monitorTryLock(mon, self)) {
                threadYield(self);
                goto retry;
            }

            if(testFlcBit(obj) && (mon->obj == obj))
                monitorNotify(mon, self);

            monitorUnlock(mon, self);
        }
    } else {
        if((lockword & (TID_MASK|SHAPE_BIT)) == thin_locked)
            ATOMIC_WRITE(&obj->lock, lockword - (1<<COUNT_SHIFT));
        else
            if((lockword & SHAPE_BIT) != 0) {
                Monitor *mon = (Monitor*) (lockword & ~SHAPE_BIT);

                if((mon->count == 0) && (ATOMIC_READ(&mon->entering) == 0) &&
                                (mon->waiting == 0)) {
                    TRACE(("Thread %p is deflating obj %p...\n", self, obj));

                    /* This barrier is not needed for the thin-locking implementation --
                       it's a requirement of the Java memory model. */
                    JMM_UNLOCK_MBARRIER();

                    ATOMIC_WRITE(&obj->lock, 0);
                    COMPARE_AND_SWAP(&mon->entering, 0, UN_USED);
                }

                monitorUnlock(mon, self);
            }
    }
}

void objectWait(Object *obj, long long ms, int ns) {
    uintptr_t lockword = ATOMIC_READ(&obj->lock);
    Thread *self = threadSelf();
    Monitor *mon;

    TRACE(("Thread %p Wait on obj %p...\n", self, obj));

    if((lockword & SHAPE_BIT) == 0) {
        int tid = (lockword&TID_MASK)>>TID_SHIFT;
        if(tid == self->id) {
            mon = findMonitor(obj);
            monitorLock(mon, self);
            inflate(obj, mon, self);
            mon->count = (lockword&COUNT_MASK)>>COUNT_SHIFT;
        } else
            goto not_owner;
    } else
        mon = (Monitor*) (lockword & ~SHAPE_BIT);

    if(monitorWait(mon, self, ms, ns))
        return;

not_owner:
    signalException("java/lang/IllegalMonitorStateException", "thread not owner");
}

void objectNotify(Object *obj) {
    uintptr_t lockword = ATOMIC_READ(&obj->lock);
    Thread *self = threadSelf();

    TRACE(("Thread %p Notify on obj %p...\n", self, obj));

    if((lockword & SHAPE_BIT) == 0) {
        int tid = (lockword&TID_MASK)>>TID_SHIFT;
        if(tid == self->id)
            return;
    } else {
        Monitor *mon = (Monitor*) (lockword & ~SHAPE_BIT);
        if(monitorNotify(mon, self))
            return;
    }

    signalException("java/lang/IllegalMonitorStateException", "thread not owner");
}

void objectNotifyAll(Object *obj) {
    uintptr_t lockword = ATOMIC_READ(&obj->lock);
    Thread *self = threadSelf();

    TRACE(("Thread %p NotifyAll on obj %p...\n", self, obj));

    if((lockword & SHAPE_BIT) == 0) {
        int tid = (lockword&TID_MASK)>>TID_SHIFT;
        if(tid == self->id)
            return;
    } else {
        Monitor *mon = (Monitor*) (lockword & ~SHAPE_BIT);
        if(monitorNotifyAll(mon, self))
            return;
    }

    signalException("java/lang/IllegalMonitorStateException", "thread not owner");
}

int objectLockedByCurrent(Object *obj) {
    uintptr_t lockword = ATOMIC_READ(&obj->lock);
    Thread *self = threadSelf();

    if((lockword & SHAPE_BIT) == 0) {
        int tid = (lockword&TID_MASK)>>TID_SHIFT;
        if(tid == self->id)
            return TRUE;
    } else {
        Monitor *mon = (Monitor*) (lockword & ~SHAPE_BIT);
        if(mon->owner == self)
            return TRUE;
    }
    return FALSE;
}

void initialiseMonitor() {
    /* Init hash table, create lock */
    initHashTable(mon_cache, HASHTABSZE, TRUE);
}

