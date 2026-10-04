/* Nasal lock/semaphore layer for PS3. FlightGear runs every Nasal context on
   the main thread, so locks never contend and a semaphore is never waited on
   by anyone who could be woken; plain counters are enough. */
#include "code.h"

struct naSem { int count; };

void* naNewLock() { return naAlloc(sizeof(int)); }
void naLock(void* lock) { (void)lock; }
void naUnlock(void* lock) { (void)lock; }

void* naNewSem()
{
    struct naSem* sem = naAlloc(sizeof(struct naSem));
    sem->count = 0;
    return sem;
}

void naSemDown(void* sh)
{
    struct naSem* sem = (struct naSem*)sh;
    if(sem->count > 0) sem->count--;
}

void naSemUpAll(void* sh, int count)
{
    ((struct naSem*)sh)->count = count;
}
