/* exit.h
 *  Copyright (C) 2001-2007, The Perl Foundation.
 *  SVN Info
 *     $Id$
 *  Overview:
 *
 *  Data Structure and Algorithms:
 *  History:
 *  Notes:
 *  References:
 *      exit.c
 */

#ifndef PARROT_EXIT_H_GUARD
#define PARROT_EXIT_H_GUARD

#include "parrot/compiler.h"    /* compiler capabilities */

typedef void (*exit_handler_f)(Interp *, int , void *);

typedef struct _handler_node_t {
    exit_handler_f function;
    void *arg;
    struct _handler_node_t *next;
} handler_node_t;

/* HEADERIZER BEGIN: src/exit.c */

PARROT_API
void Parrot_exit( Interp *interp /*NN*/, int status )
        __attribute__nonnull__(1)
        __attribute__noreturn__;

PARROT_API
int Parrot_on_exit( Interp *interp /*NN*/,
    exit_handler_f function /*NN*/,
    void *arg )
        __attribute__nonnull__(1)
        __attribute__nonnull__(2);

/* HEADERIZER END: src/exit.c */

#endif /* PARROT_EXIT_H_GUARD */

/*
 * Local variables:
 *   c-file-style: "parrot"
 * End:
 * vim: expandtab shiftwidth=4:
 */
