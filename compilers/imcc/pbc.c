/*
 * Copyright (C) 2007, The Perl Foundation.
 * $Id$
 */

#include "imc.h"
#include "pbc.h"
#include "parrot/packfile.h"

/*
 * pbc.c
 *
 * emit imcc instructions into Parrot interpreter
 *
 * the e_pbc_emit function is called per instruction
 *
 * Notes:
 *
 * I'm using existing data structures here (SymReg*) to store
 * various global items (currently only PMC constants).
 * The index in the constant table is in SymReg* ->color
 * data member. This looks odd, but the register number
 * from imc.c:allocate is also there for variables,
 * so it's a little bit consistent at least.
 *
 * So when reading color here it's either a constant table index
 * or a Parrot register number, depending on data type.
 *
 * TODO memory clean up
 *
 * -lt
 *
 */

/*
 * globals store the state between individual e_pbc_emit calls
 */

typedef struct subs_t {
    size_t         size;               /* code size in ops */
    int            ins_line;           /* line number for debug */
    int            n_basic_blocks;     /* block count */
    SymHash        fixup;              /* currently set_p_pc sub names only */
    IMC_Unit      *unit;
    int            pmc_const;          /* index in const table */
    struct subs_t *prev;
    struct subs_t *next;
} subs_t;

/* subs are kept per code segment */
typedef struct cs_t {
    PackFile_ByteCode *seg;          /* bytecode segment */
    PackFile_Segment  *jit_info;      /* bblocks, register usage */
    subs_t            *subs;          /* current sub data */
    subs_t            *first;         /* first sub of code segment */
    struct cs_t       *prev;          /* previous code segment */
    struct cs_t       *next;          /* next code segment */
    SymHash            key_consts;    /* cached key constants for this seg */
    int               pic_idx;        /* next index of PIC */
} cs_t;

static struct globals {
    cs_t *cs;                     /* current code segment */
    cs_t *first;                  /* first code segment */
    int   inter_seg_n;
} globals;

static int add_const_str(Interp *, SymReg *r);

static opcode_t build_key(Interp *interp, SymReg *reg);

static void
imcc_globals_destroy(Interp *interp, int ex, void *param)
{
    cs_t   *cs, *prev_cs;
    subs_t *s,  *prev_s;

    UNUSED(ex);
    UNUSED(param);
    UNUSED(interp);

    cs = globals.cs;

    while (cs) {
        s = cs->subs;

        while (s) {
            prev_s = s->prev;
            clear_sym_hash(&s->fixup);
            mem_sys_free(s);
            s      = prev_s;
        }

        clear_sym_hash(&cs->key_consts);
        prev_cs = cs->prev;
        mem_sys_free(cs);
        cs      = prev_cs;
    }

    globals.cs = NULL;
}

int
e_pbc_open(Interp *interp, void *param)
{
    cs_t *cs;

    /* make a new code segment */
    UNUSED(param);

    /* register cleanup code */
    if (!globals.cs)
        Parrot_on_exit(interp, imcc_globals_destroy, NULL);

    cs       = mem_allocate_zeroed_typed(cs_t);
    cs->prev = globals.cs;

    /* free previous cached key constants if any */
    if (globals.cs)
        clear_sym_hash( &globals.cs->key_consts );

    create_symhash(&cs->key_consts);

    cs->next     = NULL;
    cs->subs     = NULL;
    cs->first    = NULL;
    cs->jit_info = NULL;

    if (!globals.first)
        globals.first = cs;
    else
        cs->prev->next = cs;

    /* we need some segments */
    if (!interp->code) {
        PMC *self;
        int k;

        cs->seg = interp->code =
            PF_create_default_segs(interp,
                    IMCC_INFO(interp)->state->file, 1);

        /*
         * create a PMC constant holding the interpreter state
         *
         * see also ParrotInterpreter.thaw and .thawfinish
         * currently just HLL_info is saved/restored
         */
        self = VTABLE_get_pmc_keyed_int(interp, interp->iglobals,
                IGLOBALS_INTERPRETER);
        k    = PDB_extend_const_table(interp);

        interp->code->const_table->constants[k]->type  = PFC_PMC;
        interp->code->const_table->constants[k]->u.key = self;
    }

    globals.cs = cs;

    return 0;
}

#ifdef HAS_JIT

/* get size/line of bytecode in ops till now */
static int
old_blocks(void)
{
    size_t  size = 0;
    subs_t *s;

    for (s = globals.cs->subs; s; s = s->prev) {
        size += s->n_basic_blocks;
    }

    return size;
}

opcode_t *
make_jit_info(Interp *interp, IMC_Unit * unit)
{
    char  *name;
    size_t size, old;

    if (!globals.cs->jit_info) {
        name = malloc(strlen(globals.cs->seg->base.name) + 5);
        sprintf(name, "%s_JIT", globals.cs->seg->base.name);
        globals.cs->jit_info =
            PackFile_Segment_new_seg(interp,
                    interp->code->base.dir, PF_UNKNOWN_SEG, name, 1);
        free(name);
    }

    size = unit->n_basic_blocks + (old = old_blocks());

    /* store current size */
    globals.cs->subs->n_basic_blocks = unit->n_basic_blocks;

    /* offset of block start and end, 4 * registers_used */
    globals.cs->jit_info->data = realloc(globals.cs->jit_info->data,
            size * sizeof (opcode_t) * 6);

    globals.cs->jit_info->size = size * 6;

    return globals.cs->jit_info->data + old * 6;
}
#endif

/* allocate a new globals.cs->subs structure */
static void
make_new_sub(IMC_Unit * unit)
{
    subs_t *s    = mem_allocate_zeroed_typed(subs_t);

    s->prev      = globals.cs->subs;
    s->next      = NULL;
    s->unit      = unit;
    s->pmc_const = -1;

    if (globals.cs->subs)
        globals.cs->subs->next = s;

    if (!globals.cs->first)
        globals.cs->first = s;

    globals.cs->subs = s;

    create_symhash(&s->fixup);
}

/* get size/line of bytecode in ops till now */
static int
get_old_size(Interp *interp, int *ins_line)
{
    subs_t *s;
    size_t  size = 0;
    *ins_line    = 0;

    if (!globals.cs || interp->code->base.data == NULL)
        return 0;

    for (s = globals.cs->subs; s; s = s->prev) {
        size      += s->size;
        *ins_line += s->ins_line;
    }

    return size;
}

static void
store_sub_size(size_t size, size_t ins_line)
{
    globals.cs->subs->size     = size;
    globals.cs->subs->ins_line = ins_line;
}

static void
store_fixup(Interp *interp, SymReg *r, int pc, int offset)
{
    SymReg *fixup = _mk_address(interp, &globals.cs->subs->fixup,
            str_dup(r->name), U_add_all);

    if (r->set == 'p')
        fixup->set = 'p';

    if (r->type & VT_ENCODED)
        fixup->type |= VT_ENCODED;

    /* set_p_pc   = 2  */
    fixup->color  = pc;
    fixup->offset = offset;
}

static void
store_key_const(char *str, int idx)
{
    SymReg *c = _mk_const(&globals.cs->key_consts, str_dup(str), 0);
    c->color = idx;
}

/* store globals for later fixup
 * return size in ops */
static int
get_codesize(Interp *interp, IMC_Unit *unit, int *src_lines)
{
    Instruction *ins;
    int          code_size;

    /* run through instructions:
     * - sanity check
     * - calc code size
     * - calc nr of src lines for debug info
     * - remember addr of labels
     * - remember set_p_pc for global fixup
     */

    *src_lines = 0;

    for (code_size = 0, ins = unit->instructions; ins ; ins = ins->next) {
        if (ins->type & ITLABEL)
            ins->r[0]->color = code_size;

        if (ins->op && *ins->op) {
            (*src_lines)++;
            if (ins->opnum < 0)
                IMCC_fatal(interp, 1, "get_codesize: "
                        "no opnum ins#%d %I\n",
                        ins->index, ins);

            if (ins->opnum == PARROT_OP_set_p_pc) {
                /* set_p_pc opcode */
                IMCC_debug(interp, DEBUG_PBC_FIXUP, "PMC constant %s\n",
                        ins->r[1]->name);

                if (ins->r[1]->usage & U_FIXUP)
                    store_fixup(interp, ins->r[1], code_size, 2);
            }

            code_size += ins->opsize;
        }
        else if (ins->opsize)
            IMCC_fatal(interp, 1, "get_codesize: "
                    "non instruction with size found\n");
    }

    return code_size;
}


/* get a global label, return the pc (absolute) */
static subs_t *
find_global_label(char *name, subs_t *sym, int *pc)
{
    SymReg *r;
    subs_t *s;

    *pc = 0;

    for (s = globals.cs->first; s; s = s->next) {
        r  = s->unit->instructions->r[0];

        /* if names and namespaces are matching - ok */
        if (r && !strcmp(r->name, name) &&
                    ((sym->unit->_namespace && s->unit->_namespace &&
                     !strcmp(sym->unit->_namespace->name,
                         s->unit->_namespace->name))
                    || (!sym->unit->_namespace && !s->unit->_namespace))) {
            return s;
        }

        *pc += s->size;
    }

    return 0;
}

/* fix global stuff */
static void
fixup_globals(Interp *interp)
{
    SymHash     *hsh;
    Instruction *ins;
    SymReg      *r1;
    pcc_sub_t   *pcc_sub;
    SymReg      *fixup;
    subs_t      *s, *s1;

    int i, pc, addr, pmc_const;
    int jumppc = 0;

    for (s = globals.cs->first; s; s = s->next) {
        hsh = &s->fixup;

        for (i = 0; i < hsh->size; i++) {
            for (fixup = hsh->data[i]; fixup; fixup = fixup->next ) {
                addr = jumppc + fixup->color;

                /* check in matching namespace */
                s1 = find_global_label(fixup->name, s, &pc);

                /*
                 * if failed change opcode:
                 *   set_p_pc  => find_name p_sc
                 * if a sub label is found
                 *   convert to find_name, if the sub is a multi
                 */
                if (s1) {
                    assert(s1->unit);
                    if (s1->unit->type & IMC_PCCSUB) {
                        ins     = s1->unit->instructions;
                        assert(ins);

                        r1      = ins->r[0];
                        assert(r1);

                        pcc_sub = r1->pcc_sub;
                        assert(pcc_sub);

                        /* if the sub is multi, don't insert constant */
                        if (pcc_sub->nmulti)
                            s1 = NULL;
                    }
                }
                if (!s1) {
                    int op, col;
                    SymReg *nam;

                    op = interp->op_lib->op_code("find_name_p_sc", 1);
                    assert(op);

                    interp->code->base.data[addr] = op;

                    nam = mk_const(interp, str_dup(fixup->name),
                            fixup->type & VT_ENCODED ? 'U' : 'S');

                    if (nam->color >= 0)
                        col = nam->color;
                    else
                        col = nam->color = add_const_str(interp, nam);

                    interp->code->base.data[addr+2] = col;

                    IMCC_debug(interp, DEBUG_PBC_FIXUP,
                            "fixup const PMC"
                            " find_name sub '%s' const nr: %d\n",
                            fixup->name, col);
                    continue;
                }

                pmc_const = s1->pmc_const;

                if (pmc_const < 0) {
                    IMCC_fatal(interp, 1, "fixup_globals: "
                            "couldn't find sub 2 '%s'\n",
                            fixup->name);
                }

                interp->code->base.data[addr+fixup->offset] = pmc_const;
                IMCC_debug(interp, DEBUG_PBC_FIXUP, "fixup const PMC"
                        " sub '%s' const nr: %d\n", fixup->name,
                        pmc_const);
                continue;
            }
        }

        jumppc += s->size;
    }
}

STRING *
IMCC_string_from_reg(Interp *interp, SymReg *r)
{
    char   *buf     = r->name;
    STRING *s       = NULL;
    char   *charset = NULL;

    if (r->type & VT_ENCODED) {
        char *p;
        /*
         * the lexer parses:   foo:"string"
         * get first part as charset, rest as string
         */
        p       = strchr(r->name, '"');
        assert(p && p[-1] == ':');

        p[-1]   = 0;
        charset = r->name;

        /* past delim */
        buf     = p + 1;
        s       = string_unescape_cstring(interp, buf, '"', charset);

        /* restore colon, as we may reuse this string */
        p[-1] = ':';
    }
    else if (*buf == '"') {
        buf++;
        s = string_unescape_cstring(interp, buf, '"', charset);
    }
    else if (*buf == '\'') {   /* TODO handle python raw strings */
        buf++;
        s = string_make(interp, buf, strlen(buf) - 1, "ascii",
                PObj_constant_FLAG);
    }
    else {
        /* unquoted bare name - ascii only dont't unescape it */
        s = string_make(interp, buf, strlen(buf), "ascii",
                PObj_constant_FLAG);
    }

    return s;
}

/* add constant string to constant_table */
static int
add_const_str(Interp *interp, SymReg *r)
{
    int     k = PDB_extend_const_table(interp);
    STRING *s = IMCC_string_from_reg(interp, r);

    interp->code->const_table->constants[k]->type     = PFC_STRING;
    interp->code->const_table->constants[k]->u.string = s;

    return k;
}

static int
add_const_num(Interp *interp, char *buf)
{
    int     k = PDB_extend_const_table(interp);
    STRING *s = string_from_cstring(interp, buf, 0);

    interp->code->const_table->constants[k]->type     = PFC_NUMBER;
    interp->code->const_table->constants[k]->u.number = string_to_num(interp,s);

    return k;
}

static PMC*
mk_multi_sig(Interp *interp, SymReg *r)
{
    PMC       *multi_sig = pmc_new(interp, enum_class_FixedPMCArray);
    pcc_sub_t *pcc_sub   = r->pcc_sub;
    INTVAL     n         = pcc_sub->nmulti;
    INTVAL     i;

    PMC                 *sig_pmc;
    PackFile_ConstTable *ct;

    VTABLE_set_integer_native(interp, multi_sig, n);

    /* :multi() n = 1, reg = NULL */
    if (!pcc_sub->multi[0]) {
        STRING *sig;
        sig     = string_from_cstring(interp, "__VOID", 0);
        sig_pmc = pmc_new(interp, enum_class_String);

        VTABLE_set_string_native(interp, sig_pmc, sig);
        VTABLE_set_pmc_keyed_int(interp, multi_sig, 0, sig_pmc);

        return multi_sig;
    }

    ct = interp->code->const_table;

    for (i = 0; i < n; ++i) {
        /* multi[i] can be a Key too -
         * store PMC constants instead of bare strings */
        r = pcc_sub->multi[i];

        if (r->set == 'S') {
            sig_pmc = pmc_new(interp, enum_class_String);
            VTABLE_set_string_native(interp, sig_pmc,
                    ct->constants[r->color]->u.string);
        }
        else {
            assert(r->set == 'K');
            sig_pmc = ct->constants[r->color]->u.key;
        }

        VTABLE_set_pmc_keyed_int(interp, multi_sig, i, sig_pmc);
    }

    return multi_sig;
}

typedef void (*decl_func_t)(Interp *, PMC*, STRING *, INTVAL);

static PMC*
create_lexinfo(Interp *interp, IMC_Unit *unit, PMC *sub, int need_lex)
{
    int                 i, k;
    SymReg             *r;
    STRING             *lex_name;
    decl_func_t         decl_func;

    PMC                *lex_info  = NULL;
    STRING             *decl_lex  = const_string(interp, "declare_lex_preg");
    SymHash            *hsh       = &unit->hash;
    PackFile_Constant **constants = interp->code->const_table->constants;
    INTVAL lex_info_id            = Parrot_get_ctx_HLL_type(interp,
                                        enum_class_LexInfo);
    PMC *lex_info_class           = interp->vtables[lex_info_id]->pmc_class;
    PMC *decl_lex_meth            = VTABLE_find_method(interp,
                                        lex_info_class, decl_lex);

    if (PMC_IS_NULL(decl_lex_meth))
        real_exception(interp, NULL, METH_NOT_FOUND,
                "Method '%Ss' not found", decl_lex);

    if (decl_lex_meth->vtable->base_type != enum_class_NCI)
        real_exception(interp, NULL, METH_NOT_FOUND,
                "Method '%Ss' is not a NCI", decl_lex);

    /*
     * I think letting this override in PASM/PIR would be a
     * can of worms - how do we call this if it declares .lex
     */
    decl_func = (decl_func_t) D2FPTR(PMC_struct_val(decl_lex_meth));

    for (i = 0; i < hsh->size; i++) {
        for (r = hsh->data[i]; r; r = r->next) {
            if (r->set == 'P' && r->usage & U_LEXICAL) {
                SymReg *n;
                if (!lex_info) {
                    lex_info = pmc_new_noinit(interp, lex_info_id);
                    VTABLE_init_pmc(interp, lex_info, sub);
                }

                /* at least one lexical name */
                n = r->reg;
                assert(n);

                while (n) {
                    k = n->color;
                    assert(k >= 0);

                    lex_name = constants[k]->u.string;
                    assert(PObj_is_string_TEST(lex_name));

                    IMCC_debug(interp, DEBUG_PBC_CONST,
                            "add lexical '%s' to sub name '%s'\n",
                            n->name, (char*)PMC_sub(sub)->name->strstart);

                    (decl_func)(interp, lex_info, lex_name, r->color);

                    /* next possible name */
                    n = n->reg;
                }
            }
        }
    }

    if (!lex_info && (unit->outer || need_lex)) {
        lex_info = pmc_new_noinit(interp, lex_info_id);
        VTABLE_init_pmc(interp, lex_info, sub);
    }

    return lex_info;
}

static PMC*
find_outer(Interp *interp, IMC_Unit *unit)
{
    subs_t *s;
    SymReg *sub;
    size_t  len;
    PMC    *current;
    STRING *cur_name;

    UNUSED(interp);

    if (!unit->outer)
        return NULL;

    /*
     * we need that the :outer sub is already compiled,
     * because we are freezing the outer Sub PMC along with this
     * one
     */

    len = strlen(unit->outer->name);

    if (!len)
        return NULL;

    for (s = globals.cs->first; s; s = s->next) {
        sub = s->unit->instructions->r[0];

        if (!strcmp(sub->name, unit->outer->name)) {
            PObj_get_FLAGS(s->unit->sub_pmc) |= SUB_FLAG_IS_OUTER;
            return s->unit->sub_pmc;
        }
    }

    /* could be eval too; check if :outer is the current sub */
    current = CONTEXT(interp->ctx)->current_sub;

    if (! current)
        IMCC_fatal(interp, 1,
                   "Undefined :outer sub '%s'.\n",
                   unit->outer->name);

    cur_name = PMC_sub(current)->name;

    if (cur_name->strlen == len &&
            !memcmp((char*)cur_name->strstart, unit->outer->name, len))
        return current;

    return NULL;
}

static int
add_const_pmc_sub(Interp *interp, SymReg *r,
        int offs, int end)
{
    int                  i, k;
    int                  ns_const = -1;
    INTVAL               type;
    INTVAL               vtable_index;
    char                *real_name;
    char                *c_name;
    PMC                 *ns_pmc;
    PMC                 *sub_pmc;
    struct Parrot_sub   *sub;
    PackFile_Constant   *pfc;
    PackFile_ConstTable *ct;
    SymReg              *ns;
    STRING              *vtable_name;

    IMC_Unit            *unit = globals.cs->subs->unit;

    if (unit->_namespace) {
        ns = unit->_namespace->reg;
        IMCC_debug(interp, DEBUG_PBC_CONST,
                "name space const = %d ns name '%s'\n", ns->color, ns->name);

        ns_const  = ns->color;

        /* strip namespace off from front */
        real_name = strrchr(r->name, '@');

        if (real_name) {
            char *p;
            ++real_name;
            p       = str_dup(real_name);
            free(r->name);
            r->name = p;
        }
    }

    ct                          = interp->code->const_table;
    k                           = PDB_extend_const_table(interp);
    pfc                         = ct->constants[k];
    globals.cs->subs->pmc_const = k;

    type = (r->pcc_sub->calls_a_sub & ITPCCYIELD) ?
        enum_class_Coroutine :
        unit->outer ? enum_class_Closure : enum_class_Sub;

    /* use a possible type mapping for the Sub PMCs */
    type = Parrot_get_ctx_HLL_type(interp, type);

    /* TODO create constant - see also src/packfile.c */
    sub_pmc                      = pmc_new(interp, type);
    PObj_get_FLAGS(sub_pmc)     |= (r->pcc_sub->pragma & SUB_FLAG_PF_MASK);
    Sub_comp_get_FLAGS(sub_pmc) |= (r->pcc_sub->pragma & SUB_COMP_FLAG_MASK);
    sub                          = PMC_sub(sub_pmc);

    r->color  = add_const_str(interp, r);
    sub->name = ct->constants[r->color]->u.string;

    ns_pmc    = NULL;

    if (ns_const >= 0 && ns_const < ct->const_count) {
        switch (ct->constants[ns_const]->type) {
            case PFC_KEY:
                ns_pmc = ct->constants[ns_const]->u.key;
                break;
            case PFC_STRING:
                ns_pmc = constant_pmc_new(interp, enum_class_String);
                PMC_str_val(ns_pmc) = ct->constants[ns_const]->u.string;
                break;
        }
    }

    sub->namespace_name = ns_pmc;
    sub->start_offs     = offs;
    sub->end_offs       = end;
    sub->HLL_id         = CONTEXT(interp->ctx)->current_HLL;

    for (i = 0; i < 4; ++i)
        sub->n_regs_used[i] = unit->n_regs_used[i];

    sub->lex_info     = create_lexinfo(interp, unit, sub_pmc,
            r->pcc_sub->pragma & P_NEED_LEX);
    sub->outer_sub    = find_outer(interp, unit);
    sub->vtable_index = -1;

    /* check if it's declared multi */
    if (r->pcc_sub->nmulti)
        sub->multi_signature = mk_multi_sig(interp, r);
    else
        sub->multi_signature = NULL;

    Parrot_store_sub_in_namespace(interp, sub_pmc);

    if (unit->is_vtable_method == 1) {
        /* Work out the name of the vtable method. */
        if (unit->vtable_name != NULL)
            vtable_name = string_from_cstring(interp, unit->vtable_name + 1,
                 strlen(unit->vtable_name) - 2);
        else
            vtable_name = sub->name;

        /* Check this is a valid vtable method to override. */
        vtable_index = Parrot_get_vtable_index(interp, vtable_name);
        c_name       = string_to_cstring(interp, vtable_name);

        if (vtable_index == -1) {
            IMCC_fatal(interp, 1,
                "'%s' is not a v-table method, but was used with :vtable.\n",
                c_name);
        }

        string_cstring_free(c_name);

        /* TODO check for duplicates */
        sub->vtable_index = vtable_index;
    }

    pfc->type     = PFC_PMC;
    pfc->u.key    = sub_pmc;
    unit->sub_pmc = sub_pmc;

    IMCC_debug(interp, DEBUG_PBC_CONST,
            "add_const_pmc_sub '%s' flags %d color %d (%s) "
            "lex_info %s :outer(%s)\n",
            r->name, r->pcc_sub->pragma, k,
            (char *) sub_pmc->vtable->whoami->strstart,
            sub->lex_info ? "yes" : "no",
            sub->outer_sub ?
                (char *)PMC_sub(sub->outer_sub)->name->strstart :
                "*none*"
            );
    /*
     * create entry in our fixup (=symbol) table
     * the offset is the index in the constant table of this Sub
     */
    PackFile_FixupTable_new_entry(interp, r->name, enum_fixup_sub, k);
    return k;
}

/* add constant key to constant_table */
static int
add_const_key(Interp *interp, opcode_t key[], int size, char *s_key)
{
    int                k;
    SymReg            *r;
    opcode_t          *rc;
    PackFile_Constant *pfc;

    if ( (r = _get_sym(&globals.cs->key_consts, s_key)) != 0)
        return r->color;

    pfc = mem_allocate_typed(PackFile_Constant);
    rc  = PackFile_Constant_unpack_key(interp,
            interp->code->const_table, pfc, key);

    if (!rc) {
        mem_sys_free(pfc);
        IMCC_fatal(interp, 1,
            "add_const_key: PackFile_Constant error\n");
    }

    k = PDB_extend_const_table(interp);

    interp->code->const_table->constants[k]->type  = PFC_KEY;
    interp->code->const_table->constants[k]->u.key = pfc->u.key;

    store_key_const(s_key, k);

    IMCC_debug(interp, DEBUG_PBC_CONST, "\t=> %s #%d size %d\n",
               s_key, k, size);
    IMCC_debug(interp, DEBUG_PBC_CONST, "\t %x /%x %x/ /%x %x/\n",
               key[0],key[1],key[2],key[3],key[4]);

    mem_sys_free(pfc);

    return k;
}

static char *
slice_deb(int bits) {
    if ((bits & VT_SLICE_BITS) == (VT_START_SLICE|VT_END_SLICE))
        return "start+end";

    if ((bits & VT_SLICE_BITS) == (VT_START_ZERO|VT_END_SLICE))
        return "..end";

    if ((bits & VT_SLICE_BITS) == (VT_START_SLICE|VT_END_INF))
        return "start..";

    if (bits & VT_START_SLICE)
        return "start";

    if (bits & VT_END_SLICE)
        return "end";

    return "";
}

/*
 * color is a Parrot register number or a constant table index
 *
 * for the rest, please consult PDD08_KEYS(1)
 *
 * additionally, I build a string representation of the key,
 * which gets cached in the globals.keys
 *
 */

static opcode_t
build_key(Interp *interp, SymReg *key_reg)
{
#define KEYLEN 21
    char      s_key[KEYLEN * 10];
    opcode_t  key[KEYLEN];
    opcode_t  size;
    int       key_length;     /* P0["hi;there"; S0; 2] has length 3 */
    int       k;
    SymReg   *r;
    SymReg *reg;
    int       var_type, slice_bits, type;

    /* 0 is length */
    opcode_t *pc = key + 1;

    /* stringified key */
    char     *s  = s_key;
    *s           = 0;

    reg = key_reg->set == 'K' ? key_reg->nextkey : key_reg;

    for (key_length = 0; reg ; reg = reg->nextkey, key_length++) {
        if ((pc - key - 2) >= KEYLEN)
            IMCC_fatal(interp, 1, "build_key:"
                    "key too complex increase KEYLEN\n");
        r = reg;

        /* if key is a register, the original sym is in r->reg */
        type = r->type;

        if (r->reg)
            r = r->reg;

        var_type   = type & ~VT_SLICE_BITS;
        slice_bits = type &  VT_SLICE_BITS;

        switch (var_type) {
            case VTIDENTIFIER:       /* P[S0] */
            case VTPASM:             /* P[S0] */
            case VTREG:              /* P[S0] */
                if (r->set == 'I')
                    *pc++ = PARROT_ARG_I | slice_bits;    /* register type */
                else if (r->set == 'S')
                    *pc++ = PARROT_ARG_S | slice_bits;
                else
                    IMCC_fatal(interp, 1, "build_key: wrong register set\n");

                /* don't emit mapped regs in key parts */
                if (r->color < 0)
                    *pc++ = -1 - r->color;
                else
                    *pc++ = r->color;

                sprintf(s+strlen(s), "%c%d", r->set, (int)r->color);

                IMCC_debug(interp, DEBUG_PBC_CONST,
                        " keypart reg %s %c%d slice %s\n",
                        r->name, r->set, (int)r->color,
                        slice_deb(slice_bits));
                break;
            case VT_CONSTP:
            case VTCONST:
            case VTCONST|VT_ENCODED:
                switch (r->set) {
                    case 'S':                       /* P["key"] */
                        /* str constant */
                        *pc++ = PARROT_ARG_SC | slice_bits;

                        /* constant idx */
                        *pc++ = r->color;

                        IMCC_debug(interp, DEBUG_PBC_CONST,
                                " keypart SC %s #%d slice %s\n",
                                r->name, r->color,
                                slice_deb(slice_bits));
                        break;
                    case 'I':                       /* P[;42;..] */
                        /* int constant */
                        *pc++ = PARROT_ARG_IC | slice_bits;

                        /* value */
                        *pc++ = r->color = atol(r->name);

                        IMCC_debug(interp, DEBUG_PBC_CONST,
                                " keypart IC %s #%d slice %s\n",
                                r->name, r->color,
                                slice_deb(slice_bits));
                        break;
                    default:
                        IMCC_fatal(interp, 1,"build_key: unknown set\n");
                }
                sprintf(s+strlen(s), "%cc" INTVAL_FMT, r->set, r->color);
                break;
            default:
                IMCC_fatal(interp, 1,"build_key: "
                    "unknown type 0x%x on %s\n", var_type, r->name);
        }
    }

    key[0] = key_length;
    size   = pc - key;

    /* now we have a packed key, which packfile can work on */
    /* XXX endianess? probably no, we pack/unpack on the very
     * same computer */
    k      = add_const_key(interp, key, size, s_key);

    /* single 'S' keys already have their color assigned */
    if (key_reg->set == 'K')
        key_reg->color = k;

    return k;
}

INTVAL
IMCC_int_from_reg(Interp *interp, SymReg *r)
{
    INTVAL i;

    UNUSED(interp);
    errno = 0;

    if (r->type & VT_CONSTP)
        r = r->reg;

    if (r->name[0] == '0' && (r->name[1] == 'x' || r->name[1] == 'X'))
        i = strtoul(r->name+2, 0, 16);
    else if (r->name[0] == '0' && (r->name[1] == 'O' || r->name[1] == 'o'))
        i = strtoul(r->name+2, 0, 8);
    else if (r->name[0] == '0' &&
            (r->name[1] == 'b' || r->name[1] == 'B'))
        i = strtoul(r->name+2, 0, 2);
    else
        i = strtol(r->name, 0, 10);

    /*
     * TODO
     * - is this portable?
     * - there are some more atol()s in this file
     */
    if (errno == ERANGE)
        IMCC_fatal(interp, 1, "add_1_const:"
                "Integer overflow '%s'", r->name);

    return i;
}

static void
make_pmc_const(Interp *interp, SymReg *r)
{
    STRING *s;
    PMC    *p, *_class;
    int     k;

    if (*r->name == '"')
        s = string_unescape_cstring(interp, r->name + 1, '"', NULL);
    else
    if (*r->name == '\'')
        s = string_unescape_cstring(interp, r->name + 1, '\'', NULL);
    else
        s = string_unescape_cstring(interp, r->name, 0, NULL);

    _class  = interp->vtables[r->pmc_type]->pmc_class;
    p       = VTABLE_new_from_string(interp, _class, s, PObj_constant_FLAG);

    /* append PMC constant */
    k       = PDB_extend_const_table(interp);

    interp->code->const_table->constants[k]->type  = PFC_PMC;
    interp->code->const_table->constants[k]->u.key = p;

    r->color = k;
}

static void
add_1_const(Interp *interp, SymReg *r)
{
    SymReg *key;

    if (r->color >= 0)
        return;

    if (r->use_count <= 0)
        return;

    switch (r->set) {
        case 'I':
            r->color = IMCC_int_from_reg(interp, r);
            break;
        case 'S':
            if (r->type & VT_CONSTP)
                r = r->reg;
            r->color = add_const_str(interp, r);
            break;
        case 'N':
            r->color = add_const_num(interp, r->name);
            break;
        case 'K':
            key = r;
            for (r = r->nextkey; r; r = r->nextkey)
                if (r->type & VTCONST)
                    add_1_const(interp, r);
            build_key(interp, key);
            break;
        case 'P':
            make_pmc_const(interp, r);
            IMCC_debug(interp, DEBUG_PBC_CONST,
                    "PMC const %s\tcolor %d\n",
                    r->name, r->color);
            break;
        default:
            break;
    }

    if (r)
        IMCC_debug(interp, DEBUG_PBC_CONST,
                "const %s\tcolor %d use_count %d\n",
                r->name, r->color, r->use_count);
}

/* store a constants idx for later reuse */
static void
constant_folding(Interp *interp, IMC_Unit * unit)
{
    int      i;
    SymReg  *r, *n;
    SymHash *hsh = &IMCC_INFO(interp)->ghash;

    /* go through all consts of current sub */
    for (i = 0; i < hsh->size; i++) {
        /* normally constants are in ghash ... */
        for (r = hsh->data[i]; r; r = r->next) {
            if (r->type & (VTCONST|VT_CONSTP))
                add_1_const(interp, r);

            if (r->usage & U_LEXICAL) {
                n = r->reg;

                /* r->reg is a chain of names for the same lex sym */
                while (n) {
                    /* lex_name */
                    add_1_const(interp, n);
                    n = n->reg;
                }
            }
        }
    }

    /* ... but keychains 'K' are in local hash, they may contain
     * variables and constants
     */
    hsh = &unit->hash;

    for (i = 0; i < hsh->size; i++) {
        /* normally constants are in ghash ... */
        for (r = hsh->data[i]; r; r = r->next) {
            if (r->type & VTCONST)
                add_1_const(interp, r);
        }
    }

    /* and finally, there may be an outer Sub */
    if (unit->outer)
        add_1_const(interp, unit->outer);
}

int
e_pbc_new_sub(Interp *interp, void *param, IMC_Unit *unit)
{
    UNUSED(param);
    UNUSED(interp);

    if (!unit->instructions)
        return 0;

    /* we start a new compilation unit */
    make_new_sub(unit);

    return 0;
}

int
e_pbc_end_sub(Interp *interp, void *param, IMC_Unit *unit)
{
    Instruction *ins;
    int          pragma;

    UNUSED(param);

    if (!unit->instructions)
        return 0;

    /*
     * if the sub was marked IMMEDIATE, we run it now
     * This is *dangerous*: all possible global state can be messed
     * up, e.g. when that sub start loading bytecode
     */
    ins   = unit->instructions;

    /* we run only PCC subs */
    if (!ins->r[0] || !ins->r[0]->pcc_sub)
        return 0;

    pragma = ins->r[0]->pcc_sub->pragma;

    if (pragma & P_IMMEDIATE) {
        IMCC_debug(interp, DEBUG_PBC, "immediate sub '%s'",
                ins->r[0]->name);
        PackFile_fixup_subs(interp, PBC_IMMEDIATE, NULL);
    }

    return 0;
}

/*
 * - check if any get_ argument contains constants
 * - fill in type bits for argument types and constants, if missing
 */

static void
verify_signature(Interp *interp, Instruction *ins, opcode_t *pc)
{
    INTVAL  i, n, sig;
    SymReg *r;
    int     no_consts, k;

    int     needed      = 0;
    PMC    *changed_sig = NULL;
    PMC    *sig_arr     = interp->code->const_table->constants[pc[-1]]->u.key;

    assert(PObj_is_PMC_TEST(sig_arr));
    assert(sig_arr->vtable->base_type == enum_class_FixedIntegerArray);

    no_consts = (ins->opnum == PARROT_OP_get_results_pc ||
        ins->opnum == PARROT_OP_get_params_pc);

    n = VTABLE_elements(interp, sig_arr);

    for (i = 0; i < n; ++i) {
        r   = ins->r[i + 1];
        sig = VTABLE_get_integer_keyed_int(interp, sig_arr, i);

        if (! (sig & PARROT_ARG_NAME) &&
                no_consts && (r->type & VTCONST))
            IMCC_fatal(interp, 1, "e_pbc_emit: "
                    "constant argument '%s' in get param/result\n", r->name);

        if ((r->type & VTCONST) && !(sig & PARROT_ARG_CONSTANT)) {
            if (!changed_sig)
                changed_sig = VTABLE_clone(interp, sig_arr);

            sig |= PARROT_ARG_CONSTANT;

            VTABLE_set_integer_keyed_int(interp, changed_sig, i, sig);
        }

        switch (r->set) {
            case 'I': needed = PARROT_ARG_INTVAL; break;
            case 'S': needed = PARROT_ARG_STRING; break;
            case 'P': needed = PARROT_ARG_PMC; break;
            case 'N': needed = PARROT_ARG_FLOATVAL; break;
        }

        if (needed != (sig & PARROT_ARG_TYPE_MASK)) {
            if (!changed_sig)
                changed_sig = VTABLE_clone(interp, sig_arr);

            sig &= ~PARROT_ARG_TYPE_MASK;
            sig |= needed;

            VTABLE_set_integer_keyed_int(interp, changed_sig, i, sig);
        }
    }

    if (changed_sig) {
        /* append PMC constant */
        k      = PDB_extend_const_table(interp);

        interp->code->const_table->constants[k]->type  = PFC_PMC;
        interp->code->const_table->constants[k]->u.key = changed_sig;

        pc[-1] = k;
    }
}

/* now let the fun begin, actually emit code for one ins */
int
e_pbc_emit(Interp *interp, void *param, IMC_Unit * unit, Instruction * ins)
{
    int        op, i;
    int        ok = 0;
    op_info_t *op_info;

    /* XXX move these statics into IMCC_INFO */
    static PackFile_Debug *debug_seg;
    static int             ins_line;
    static opcode_t       *pc;
    static opcode_t        npc;
    /* XXX end */

#if IMC_TRACE_HIGH
    PIO_eprintf(NULL, "e_pbc_emit\n");
#endif

    UNUSED(param);

    /* first instruction, do initialisation ... */
    if (ins == unit->instructions) {
        int code_size, ins_size, oldsize, bytes;

        oldsize   = get_old_size(interp, &ins_line);
        code_size = get_codesize(interp, unit, &ins_size);

        IMCC_debug(interp, DEBUG_PBC, "code_size(ops) %d  oldsize %d\n",
                code_size, oldsize);

        constant_folding(interp, unit);
        store_sub_size(code_size, ins_size);

        bytes = (oldsize + code_size) * sizeof (opcode_t);

        /*
         * allocate code and pic_index
         *
         * pic_index is half the size of the code, as one PIC-cachable opcode
         * is at least two opcodes wide - see below how to further decrease
         * this storage
         */
        if (interp->code->base.data) {
            interp->code->base.data =
                mem_sys_realloc(interp->code->base.data, bytes);
            interp->code->pic_index->data =
                mem_sys_realloc(interp->code->pic_index->data, bytes/2);
        }
        else {
            interp->code->base.data = mem_sys_allocate(bytes);
            interp->code->pic_index->data = mem_sys_allocate(bytes/2);
        }
        interp->code->base.size       = oldsize + code_size;
        interp->code->pic_index->size = (oldsize + code_size) / 2;

        pc  = (opcode_t *)interp->code->base.data + oldsize;
        npc = 0;

        /* add debug if necessary */
        if (!IMCC_INFO(interp)->optimizer_level ||
            IMCC_INFO(interp)->optimizer_level == OPT_PASM) {
            const char *sourcefile;
            sourcefile = unit->file;

            /* FIXME length and multiple subs */
            debug_seg  = Parrot_new_debug_seg(interp,
                    interp->code, (size_t) ins_line + ins_size + 1);

            Parrot_debug_add_mapping(interp, debug_seg, ins_line,
                     PF_DEBUGMAPPINGTYPE_FILENAME, sourcefile, 0);
        }
        else
            debug_seg = NULL;

        /* if item is a PCC_SUB entry then store it constants */
        if (ins->r[0] && ins->r[0]->pcc_sub) {
#if IMC_TRACE
            PIO_eprintf(NULL, "pbc.c: e_pbc_emit (pcc_sub=%s)\n",
                        ins->r[0]->name);
#endif
            add_const_pmc_sub(interp, ins->r[0], oldsize, oldsize + code_size);
        }
        else {
            /* need a dummy to hold register usage */
            SymReg *r;
            r          = mk_sub_label(interp, str_dup("(null)"));
            r->type    = VT_PCC_SUB;
            r->pcc_sub = calloc(1, sizeof (pcc_sub_t));

            add_const_pmc_sub(interp, r, oldsize, oldsize + code_size);
        }
    }

    /* if this is not the first sub then store the sub */
    if (npc && unit->pasm_file && ins->r[0] && ins->r[0]->pcc_sub) {
        /* we can only set the offset for PASM code */
        add_const_pmc_sub(interp, ins->r[0], npc, npc);
    }

    if (ins->op && *ins->op) {
        SymReg *addr, *r;
        opcode_t last_label;
        last_label = 1;
#if IMC_TRACE_HIGH
        PIO_eprintf(NULL, "emit_pbc: op [%d %s]\n", ins->opnum, ins->op);
#endif
        if ((ins->type & ITBRANCH) &&
                (addr = get_branch_reg(ins)) != 0 &&
                !(addr->type & VTREGISTER)) {
            /* fixup local jumps - calc offset */
            if (addr->color == -1)
                IMCC_fatal(interp, 1, "e_pbc_emit: "
                        "no label offset defined for '%s'\n", addr->name);
            last_label = addr->color - npc;
            IMCC_debug(interp, DEBUG_PBC_FIXUP,
                    "branch label at pc %d addr %d %s %d\n",
                    npc, addr->color, addr->name, last_label);
        }

        /* add debug line info */
        if (debug_seg)
            debug_seg->base.data[ins_line++] = (opcode_t) ins->line;

        op = (opcode_t)ins->opnum;

        /* add PIC idx */
        if (parrot_PIC_op_is_cached(interp, op)) {
            size_t offs;
            offs = pc - interp->code->base.data;
            /*
             * for pic_idx fitting into a short, we could
             * further reduce the size by storing shorts
             * the relation code_size / pic_index_size could
             * indicate the used storage
             *
             * drawback: if we reach 0xffff, we'd have to resize again
             */
            interp->code->pic_index->data[offs / 2] = ++globals.cs->pic_idx;
        }

        /* Start generating the bytecode */
        *pc++   = op;

        /* Get the info for that opcode */
        op_info = &interp->op_info_table[op];

        IMCC_debug(interp, DEBUG_PBC, "%d %s", npc, op_info->full_name);

        for (i = 0; i < op_info->op_count-1; i++) {
            switch (op_info->types[i]) {
                case PARROT_ARG_IC:
                    /* branch instruction */
                    if (op_info->labels[i]) {
                        if (last_label == 1)
                            /* we don't have a branch with offset 1 !? */
                            IMCC_fatal(interp, 1, "e_pbc_emit: "
                                    "no label offset found\n");
                        *pc++      = last_label;
                        last_label = 1;
                        break;
                        /* else fall through */
                    }
                case PARROT_ARG_I:
                case PARROT_ARG_N:
                case PARROT_ARG_S:
                case PARROT_ARG_P:
                case PARROT_ARG_K:
                case PARROT_ARG_KI:
                case PARROT_ARG_KIC:
                case PARROT_ARG_SC:
                case PARROT_ARG_NC:
                case PARROT_ARG_PC:
                    r     = ins->r[i];

                    if (r->type & VT_CONSTP)
                        r = r->reg;

                    *pc++ = (opcode_t) r->color;
                    IMCC_debug(interp, DEBUG_PBC," %d", r->color);
                    break;
                case PARROT_ARG_KC:
                    r = ins->r[i];
                    if (r->set == 'K') {
                        assert(r->color >= 0);
                        *pc++ = r->color;
                    }
                    else {
                        *pc++ = build_key(interp, r);
                    }
                    IMCC_debug(interp, DEBUG_PBC," %d", pc[-1]);
                    break;
                default:
                    IMCC_fatal(interp, 1, "e_pbc_emit:"
                            "unknown argtype in parrot op\n");
                    break;
            }
        }
        if (ins->opnum == PARROT_OP_set_args_pc ||
                ins->opnum == PARROT_OP_get_results_pc ||
                ins->opnum == PARROT_OP_get_params_pc ||
                ins->opnum == PARROT_OP_set_returns_pc) {
            /* TODO get rid of verify_signature - PIR call sigs are already fixed
             *      PASM still needs it
             */
            verify_signature(interp, ins, pc);

            /* emit var_args part */
            for (; i < ins->opsize - 1; ++i) {
                r = ins->r[i];
                if (r->type & VT_CONSTP)
                    r = r->reg;
                *pc++ = (opcode_t) r->color;
                IMCC_debug(interp, DEBUG_PBC," %d", r->color);
            }
        }

        IMCC_debug(interp, DEBUG_PBC, "\t%I\n", ins);
        npc += ins->opsize;
    }

    return ok;
}

int
e_pbc_close(Interp *interp, void *param)
{
    UNUSED(param);
    fixup_globals(interp);

    return 0;
}

/*
 * Local variables:
 *   c-file-style: "parrot"
 * End:
 * vim: expandtab shiftwidth=4:
 */
