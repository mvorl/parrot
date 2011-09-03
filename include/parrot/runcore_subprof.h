/* runcore_api.h
 *  Copyright (C) 2001-2009, Parrot Foundation.
 *  Overview:
 *     Functions and macros to dispatch opcodes.
 */

#ifndef PARROT_RUNCORE_SUBPROF_H_GUARD
#define PARROT_RUNCORE_SUBPROF_H_GUARD


#include <stdint.h>
#include "pmc/pmc_sub.h"

struct callinfo {
    struct subprofile *callee;
    int                count;
    unsigned int       ops;
    uint64_t           ticks;
};

struct subprofile {
    struct subprofile     *hnext;

    struct subprofile     *rnext;
    int                    rcnt;

    PMC                   *subpmc;
    Parrot_Sub_attributes *sub;

    struct callinfo       *calls;
    int                    ncalls;

    unsigned int           ops;
    uint64_t               ticks;

    /* call chain */
    struct subprofile     *caller;
    int                    calleri;
    PMC                   *ctx;
    unsigned int           callerops;
    uint64_t               callerticks;
};

struct subprofile *subprofilehash[32768];

static PMC *cursubpmc;
static PMC *curctx;
static struct subprofile *cursp;

uint64_t opstart;

uint64_t *tickadd;
uint64_t *tickadd2;
uint64_t starttick;

unsigned int totalops;
uint64_t totalticks;

/* HEADERIZER BEGIN: src/runcore/subprof.c */
/* Don't modify between HEADERIZER BEGIN / HEADERIZER END.  Your changes will be lost. */

void dump_profile_data(PARROT_INTERP)
        __attribute__nonnull__(1);

void profile(PARROT_INTERP, PMC *ctx, opcode_t *pc)
        __attribute__nonnull__(1);

static struct subprofile * sub2subprofile(PARROT_INTERP,
    PMC *ctx,
    PMC *subpmc)
        __attribute__nonnull__(1);

#define ASSERT_ARGS_dump_profile_data __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp))
#define ASSERT_ARGS_profile __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp))
#define ASSERT_ARGS_struct subprofile * sub2subprofile \
     __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp))
/* Don't modify between HEADERIZER BEGIN / HEADERIZER END.  Your changes will be lost. */
/* HEADERIZER END: src/runcore/subprof.c */

#endif /* PARROT_RUNCORE_SUBPROF_H_GUARD */

/*
 * Local variables:
 *   c-file-style: "parrot"
 * End:
 * vim: expandtab shiftwidth=4 cinoptions='\:2=2' :
 */
