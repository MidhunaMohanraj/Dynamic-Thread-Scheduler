/**
 * Tony Givargis
 * Copyright (C), 2023
 * University of California, Irvine
 *
 * CS 238P - Operating Systems
 * scheduler.c
 */

#undef _FORTIFY_SOURCE

#include <unistd.h>
#include <signal.h>
#include <setjmp.h>
#include "System.h"
#include "Scheduler.h"


struct thread {
    jmp_buf ctx;

    struct {
        char *memory;       /* real, aligned memory */
        char *memoryToFree; /* extra large chunk of memory */
    } stack;

    struct {
        void *arg;
        scheduler_fnc_t fnc;
    } code;

    enum {
        INIT,
        RUNNING,
        SLEEPING,
        TERMINATED
    } status;

    struct thread *link; /* pointer to thread */

};

static struct {
    struct thread *head;
    struct thread *currThread;
    jmp_buf ctx; /* reg state of main thread */
} threadList;

/* Finds and returns the next thread in the list which 
 *   is not TERMINATED (it can be in INIT or SLEEPING state).
 *   Returns NULL if all threads are in TERMINATED state
 * */
static struct thread *candidate(void) {
    /* temp pointer to thread */
    struct thread *tempThread =  threadList.currThread->link;

    while(tempThread != threadList.currThread) {
        if(tempThread->status != TERMINATED) {
            /* Thread found in INIT or SLEEPING state */
            return tempThread;
        }
        /* move to next thread and continue search */
        tempThread = tempThread->link;
    }

    /* No available threads */
    return NULL;
}

/* Deletes every thread stored in the threadList structure.
 * We start at the thread that is linked to the current thread
 * and continue to loop / delete every thread until we return
 * to currThread.
 * 
 * Note: frees the full memory that was allocated for the thread
 */
static void destroy(void) {
    struct thread *temp = threadList.currThread->link;

    while (temp != threadList.currThread) {
        struct thread *link = temp->link;
        free(temp->stack.memoryToFree); 
        free(temp);  
        temp = link; 
    }

    free(threadList.currThread->stack.memoryToFree);
    free(threadList.currThread);
    threadList.currThread = NULL;
    threadList.head = NULL;

    /* All threads have been destroyed */
}


/* Finds the next thread to run and, depending on it's state, will start running
 *   it or continue running from where it left off
 * Returns if all threads have been run 
 */
static void schedule(void) {    
    /* find the next thread to run */
    struct thread *nextThread = candidate();

    /* if candidate returns NULL, all threads have been run */
    if(nextThread == NULL) { return; }

    /* set currThread with the available thread returned by candidate()*/
    threadList.currThread = nextThread;

    /* Case 1: currThread is in INIT */
    if(threadList.currThread->status == INIT) {
        /* Update the stack pointer bc each thread needs its own portion of the stack*/
        uint64_t rsp = (uint64_t) threadList.currThread->stack.memoryToFree + SZ_STACK;
        __asm__ volatile("mov %[rs], %%rsp \n" : [rs] "+r" (rsp)::);

        /* Update thread's status to RUNNING & call fnc with args */
        threadList.currThread->status = RUNNING;
        threadList.currThread->code.fnc(threadList.currThread->code.arg);

        /* Thread function has executed -> marking thread as TERMINATED */
        threadList.currThread->status = TERMINATED;

        /* jumps back to schedueler_execute to start schedule() for new thread */
        longjmp(threadList.ctx, 1);
    }
    else {
        /* Thread was SLEEPING, switch to RUNNING and resume execution */
        threadList.currThread->status = RUNNING;
        longjmp(threadList.currThread->ctx, 1);
    }
}


/**
 * Creates a new user thread.
 *
 * fnc: the start function of the user thread (see scheduler_fnc_t)
 * arg: a pass-through pointer defining the context of the user thread
 *
 * return: 0 on success, otherwise error 
 * */
int scheduler_create(scheduler_fnc_t fnc, void *arg) {
    struct thread *newThread;
    
    /* dynamically allocate a block of memeory for thread */
    if(!(newThread = malloc(sizeof(*newThread)))) {
        printf("Out of memory for new thread");
        return -1;
    }

    /* Attribute Initializations */
    newThread->status = INIT;
    newThread->code.fnc = fnc;
    newThread->code.arg = arg;
    newThread->link = NULL;

    /* allocate enough memory for stack AND page_size to allow for page aligning */
    newThread->stack.memoryToFree = malloc(SZ_STACK + page_size());

    if (!newThread->stack.memoryToFree) {
        printf("Stack memory allocation failed\n");
        return -1;
    }

    /* pointer to location of page-aligned memory */
    newThread->stack.memory = memory_align(newThread->stack.memoryToFree, page_size());


    if(threadList.head == NULL) {
        threadList.head = newThread;
        threadList.currThread = newThread;
        newThread->link = newThread;
    } else {
        struct thread *currThreadPtr = threadList.currThread;
        struct thread *linkPtr = threadList.currThread->link;
        currThreadPtr->link = newThread;
        newThread->link = linkPtr;
    }
    return 0;
}

/**
 * Called to execute the user threads previously created by calling
 * scheduler_create().
 *
 * Notes:
 *   * This function should be called after a sequence of 0 or more
 *     scheduler_create() calls.
 *   * This function returns after all user threads (previously created)
 *     have terminated.
 *   * This function is not re-enterant.
 */

void scheduler_execute(void) {
    setjmp(threadList.ctx);
    handler();
    schedule();
    default_signal();
    destroy();
}


/**
 * Called from within a user thread by SIGALRM signal handler to yield 
 *   the CPU to another user thread.
 * Saves the ctx of the current thread then sets the state to SLEEPING.
 * Jumps back to the state of the scheduler
 */
void scheduler_yield(int signal) {
    /* exits function if SIGALRM was not the signal passed in */
    if (signal != SIGALRM) {
        return;
    }

    /* setjmp saves the state of the current thread */
    /* setjmp returns 0 when it is first called, an nonzero when returned
     * to from longjmp */
    if (setjmp(threadList.currThread->ctx) == 0) {
        threadList.currThread->status = SLEEPING;
        longjmp(threadList.ctx, 1); /* jumps back to state of scheduler */
    }
}

/* signal(SIGALRM, scheduler_yield) -> whenever the system sends a message 
 *    called SIGALRM, run the function scheduler_yield 
 * Send the SIGALRM signal after 1 second
 * */
void handler(void) {
    if (SIG_ERR == signal(SIGALRM, scheduler_yield)) {
        printf("Error setting signal handler");
        exit(1);
    }

    /* Set alarm to go off after 1 second -> switch threads */
    alarm(1); 
}

/* Once handler is no longer needed, this function cancels
 * and pending alarm and resets to default signal  */
void default_signal(void) {
    alarm(0);
    signal(SIGALRM, SIG_DFL); 
}
