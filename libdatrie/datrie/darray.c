/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * libdatrie - Double-Array Trie Library
 * Copyright (C) 2006  Theppitak Karoonboonyanan <thep@linux.thai.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

/*
 * darray.c - Double-array trie structure
 * Created: 2006-08-13
 * Author:  Theppitak Karoonboonyanan <thep@linux.thai.net>
 */

#include <string.h>
#include <stdlib.h>
#ifndef _MSC_VER /* for SIZE_MAX */
# include <stdint.h>
#endif
#include <stdio.h>

#include "trie-private.h"
#include "darray.h"
#include "fileutils.h"

/*----------------------------------*
 *    INTERNAL TYPES DECLARATIONS   *
 *----------------------------------*/

typedef struct _Symbols Symbols;

struct _Symbols {
    short       num_symbols;
    TrieChar    symbols[256];
};

static Symbols *    symbols_new ();
static void         symbols_free (Symbols *syms);
static void         symbols_add (Symbols *syms, TrieChar c);

#define symbols_num(s)          ((s)->num_symbols)
#define symbols_get(s,i)        ((s)->symbols[i])
#define symbols_add_fast(s,c)   ((s)->symbols[(s)->num_symbols++] = c)

/*-----------------------------------*
 *    PRIVATE METHODS DECLARATIONS   *
 *-----------------------------------*/

#define da_get_free_list(d)      (1)

static Bool         da_check_free_cell (DArray         *d,
                                        TrieIndex       s);

static Bool         da_has_children    (DArray         *d,
                                        TrieIndex       s);

static Symbols *    da_output_symbols  (const DArray   *d,
                                        TrieIndex       s);

static TrieChar *   da_get_state_key   (const DArray   *d,
                                        TrieIndex       state);

static TrieIndex    da_find_free_base  (DArray         *d,
                                        const Symbols  *symbols);

static Bool         da_fit_symbols     (DArray         *d,
                                        TrieIndex       base,
                                        const Symbols  *symbols);

static void         da_relocate_base   (DArray         *d,
                                        TrieIndex       s,
                                        TrieIndex       new_base);

static Bool         da_extend_pool     (DArray         *d,
                                        TrieIndex       to_index);

static void         da_alloc_cell      (DArray         *d,
                                        TrieIndex       cell);

static void         da_free_cell       (DArray         *d,
                                        TrieIndex       cell);

//static Bool         da_enumerate_recursive (const DArray   *d,
//                                            TrieIndex       state,
//                                            DAEnumFunc      enum_func,
//                                            void           *user_data);

/* ==================== BEGIN IMPLEMENTATION PART ====================  */

/*------------------------------------*
 *   INTERNAL TYPES IMPLEMENTATIONS   *
 *------------------------------------*/

static Symbols *
symbols_new ()
{
    Symbols *syms;

    syms = (Symbols *) malloc (sizeof (Symbols));

    if (!syms)
        return NULL;

    syms->num_symbols = 0;

    return syms;
}

static void
symbols_free (Symbols *syms)
{
    free (syms);
}

static void
symbols_add (Symbols *syms, TrieChar c)
{
    short lower, upper;

    lower = 0;
    upper = syms->num_symbols;
    while (lower < upper) {
        short middle;

        middle = (lower + upper)/2;
        if (c > syms->symbols[middle])
            lower = middle + 1;
        else if (c < syms->symbols[middle])
            upper = middle;
        else
            return;
    }
    if (lower < syms->num_symbols) {
        memmove (syms->symbols + lower + 1, syms->symbols + lower,
                 syms->num_symbols - lower);
    }
    syms->symbols[lower] = c;
    syms->num_symbols++;
}

/*------------------------------*
 *    PRIVATE DATA DEFINITONS   *
 *------------------------------*/

typedef struct {
    TrieIndex   base;
    TrieIndex   check;
} DACell;

struct _DArray {
    TrieIndex   num_cells;
    DACell     *cells;
};

/*-----------------------------*
 *    METHODS IMPLEMENTAIONS   *
 *-----------------------------*/

#define DA_SIGNATURE 0xDAFCDAFC

/* DA Header:
 * - Cell 0: SIGNATURE, number of cells
 * - Cell 1: free circular-list pointers
 * - Cell 2: root node
 * - Cell 3: DA pool begin
 */
#define DA_POOL_BEGIN 3

/**
 * @brief Create a new double-array object
 *
 * Create a new empty doubla-array object.
 */
DArray *
da_new ()
{
    DArray     *d;

    d = (DArray *) malloc (sizeof (DArray));
    if (!d)
        return NULL;

    d->num_cells = DA_POOL_BEGIN;
    d->cells     = (DACell *) malloc (d->num_cells * sizeof (DACell));
    if (!d->cells)
        goto exit_da_created;
    d->cells[0].base = DA_SIGNATURE;
    d->cells[0].check = d->num_cells;
    d->cells[1].base = -1;
    d->cells[1].check = -1;
    d->cells[2].base = DA_POOL_BEGIN;
    d->cells[2].check = 0;

    return d;

exit_da_created:
    free (d);
    return NULL;
}

/**
 * @brief Read double-array data from file
 *
 * @param file : the file to read
 *
 * @return a pointer to the openned double-array, NULL on failure
 *
 * Read double-array data from the opened file, starting from the current
 * file pointer until the end of double array data block. On return, the
 * file pointer is left at the position after the read block.
 */
DArray *
da_fread (FILE *file)
{
    long        save_pos;
    DArray     *d = NULL;
    TrieIndex   n;

    /* check signature */
    save_pos = ftell (file);
    if (!file_read_int32 (file, &n) || DA_SIGNATURE != (uint32) n)
        goto exit_file_read;

    if (NULL == (d = (DArray *) malloc (sizeof (DArray))))
        goto exit_file_read;

    /* read number of cells */
    if (!file_read_int32 (file, &d->num_cells))
        goto exit_da_created;
    if (d->num_cells > SIZE_MAX / sizeof (DACell))
        goto exit_da_created;
    d->cells = (DACell *) malloc (d->num_cells * sizeof (DACell));
    if (!d->cells)
        goto exit_da_created;
    d->cells[0].base = DA_SIGNATURE;
    d->cells[0].check= d->num_cells;
    for (n = 1; n < d->num_cells; n++) {
        if (!file_read_int32 (file, &d->cells[n].base) ||
            !file_read_int32 (file, &d->cells[n].check))
        {
            goto exit_da_cells_created;
        }
    }

    return d;

exit_da_cells_created:
    free (d->cells);
exit_da_created:
    free (d);
exit_file_read:
    fseek (file, save_pos, SEEK_SET);
    return NULL;
}

/**
 * @brief Free double-array data
 *
 * @param d : the double-array data
 *
 * Free the given double-array data.
 */
void
da_free (DArray *d)
{
    free (d->cells);
    free (d);
}

/**
 * @brief Write double-array data
 *
 * @param d     : the double-array data
 * @param file  : the file to write to
 *
 * @return 0 on success, non-zero on failure
 *
 * Write double-array data to the given @a file, starting from the current
 * file pointer. On return, the file pointer is left after the double-array
 * data block.
 */
int
da_fwrite (const DArray *d, FILE *file)
{
    TrieIndex   i;

    for (i = 0; i < d->num_cells; i++) {
        if (!file_write_int32 (file, d->cells[i].base) ||
            !file_write_int32 (file, d->cells[i].check))
        {
            return -1;
        }
    }

    return 0;
}


/**
 * @brief Get root state
 *
 * @param d     : the double-array data
 *
 * @return root state of the @a index set, or TRIE_INDEX_ERROR on failure
 *
 * Get root state for stepwise walking.
 */
TrieIndex
da_get_root (const DArray *d)
{
    /* can be calculated value for multi-index trie */
    return 2;
}


/**
 * @brief Get BASE cell
 *
 * @param d : the double-array data
 * @param s : the double-array state to get data
 *
 * @return the BASE cell value for the given state
 *
 * Get BASE cell value for the given state.
 */
TrieIndex
da_get_base (const DArray *d, TrieIndex s)
{
    return (s < d->num_cells) ? d->cells[s].base : TRIE_INDEX_ERROR;
}

/**
 * @brief Get CHECK cell
 *
 * @param d : the double-array data
 * @param s : the double-array state to get data
 *
 * @return the CHECK cell value for the given state
 *
 * Get CHECK cell value for the given state.
 */
TrieIndex
da_get_check (const DArray *d, TrieIndex s)
{
    return (s < d->num_cells) ? d->cells[s].check : TRIE_INDEX_ERROR;
}


/**
 * @brief Set BASE cell
 *
 * @param d   : the double-array data
 * @param s   : the double-array state to get data
 * @param val : the value to set
 *
 * Set BASE cell for the given state to the given value.
 */
void
da_set_base (DArray *d, TrieIndex s, TrieIndex val)
{
    if (s < d->num_cells) {
        d->cells[s].base = val;
    }
}

/**
 * @brief Set CHECK cell
 *
 * @param d   : the double-array data
 * @param s   : the double-array state to get data
 * @param val : the value to set
 *
 * Set CHECK cell for the given state to the given value.
 */
void
da_set_check (DArray *d, TrieIndex s, TrieIndex val)
{
    if (s < d->num_cells) {
        d->cells[s].check = val;
    }
}

/**
 * @brief Walk in double-array structure
 *
 * @param d : the double-array structure
 * @param s : current state
 * @param c : the input character
 *
 * @return boolean indicating success
 *
 * Walk the double-array trie from state @a *s, using input character @a c.
 * If there exists an edge from @a *s with arc labeled @a c, this function
 * returns TRUE and @a *s is updated to the new state. Otherwise, it returns
 * FALSE and @a *s is left unchanged.
 */
Bool
da_walk (const DArray *d, TrieIndex *s, TrieChar c)
{
    TrieIndex   next;

    next = da_get_base (d, *s) + c;
    if (da_get_check (d, next) == *s) {
        *s = next;
        return TRUE;
    }
    return FALSE;
}

/**
 * @brief Insert a branch from trie node
 *
 * @param d : the double-array structure
 * @param s : the state to add branch to
 * @param c : the character for the branch label
 *
 * @return the index of the new node
 *
 * Insert a new arc labelled with character @a c from the trie node
 * represented by index @a s in double-array structure @a d.
 * Note that it assumes that no such arc exists before inserting.
 */
TrieIndex
da_insert_branch (DArray *d, TrieIndex s, TrieChar c)
{
    TrieIndex   base, next;

    base = da_get_base (d, s);

    if (base > 0) {
        next = base + c;

        /* if already there, do not actually insert */
        if (da_get_check (d, next) == s)
            return next;

        /* if (base + c) > TRIE_INDEX_MAX which means 'next' is overflow,
         * or cell [next] is not free, relocate to a free slot
         */
        if (base > TRIE_INDEX_MAX - c || !da_check_free_cell (d, next)) {
            Symbols    *symbols;
            TrieIndex   new_base;

            /* relocate BASE[s] */
            symbols = da_output_symbols (d, s);
            symbols_add (symbols, c);
            new_base = da_find_free_base (d, symbols);
            symbols_free (symbols);

            if (TRIE_INDEX_ERROR == new_base)
                return TRIE_INDEX_ERROR;

            da_relocate_base (d, s, new_base);
            next = new_base + c;
        }
    } else {
        Symbols    *symbols;
        TrieIndex   new_base;

        symbols = symbols_new ();
        symbols_add (symbols, c);
        new_base = da_find_free_base (d, symbols);
        symbols_free (symbols);

        if (TRIE_INDEX_ERROR == new_base)
            return TRIE_INDEX_ERROR;

        da_set_base (d, s, new_base);
        next = new_base + c;
    }
    da_alloc_cell (d, next);
    da_set_check (d, next, s);

    return next;
}

static Bool
da_check_free_cell (DArray         *d,
                    TrieIndex       s)
{
    return da_extend_pool (d, s) && da_get_check (d, s) < 0;
}

static Bool
da_has_children    (DArray         *d,
                    TrieIndex       s)
{
    TrieIndex   base;
    TrieIndex   c, max_c;

    base = da_get_base (d, s);
    if (TRIE_INDEX_ERROR == base || base < 0)
        return FALSE;

    max_c = MIN_VAL (TRIE_CHAR_MAX, TRIE_INDEX_MAX - base);
    for (c = 0; c < max_c; c++) {
        if (da_get_check (d, base + c) == s)
            return TRUE;
    }

    return FALSE;
}

static Symbols *
da_output_symbols  (const DArray   *d,
                    TrieIndex       s)
{
    Symbols    *syms;
    TrieIndex   base;
    TrieIndex   c, max_c;

    syms = symbols_new ();

    base = da_get_base (d, s);
    max_c = MIN_VAL (TRIE_CHAR_MAX, TRIE_INDEX_MAX - base);
    for (c = 0; c < max_c; c++) {
        if (da_get_check (d, base + c) == s)
            symbols_add_fast (syms, (TrieChar) c);
    }

    return syms;
}

static TrieChar *
da_get_state_key   (const DArray   *d,
                    TrieIndex       state)
{
    TrieChar   *key;
    int         key_size, key_length;
    int         i;

    key_size = 20;
    key_length = 0;
    key = (TrieChar *) malloc (key_size);

    /* trace back to root */
    while (da_get_root (d) != state) {
        TrieIndex   parent;

        if (key_length + 1 >= key_size) {
            key_size += 20;
            key = (TrieChar *) realloc (key, key_size);
        }
        parent = da_get_check (d, state);
        key[key_length++] = (TrieChar) (state - da_get_base (d, parent));
        state = parent;
    }
    key[key_length] = '\0';

    /* reverse the string */
    for (i = 0; i < --key_length; i++) {
        TrieChar temp;

        temp = key[i];
        key[i] = key[key_length];
        key[key_length] = temp;
    }

    return key;
}

static TrieIndex
da_find_free_base  (DArray         *d,
                    const Symbols  *symbols)
{
    TrieChar        first_sym;
    TrieIndex       s;

    /* find first free cell that is beyond the first symbol */
    first_sym = symbols_get (symbols, 0);
    s = -da_get_check (d, da_get_free_list (d));
    while (s != da_get_free_list (d)
           && s < (TrieIndex) first_sym + DA_POOL_BEGIN)
    {
        s = -da_get_check (d, s);
    }
    if (s == da_get_free_list (d)) {
        for (s = first_sym + DA_POOL_BEGIN; ; ++s) {
            if (!da_extend_pool (d, s))
                return TRIE_INDEX_ERROR;
            if (da_get_check (d, s) < 0)
                break;
        }
    }

    /* search for next free cell that fits the symbols set */
    while (!da_fit_symbols (d, s - first_sym, symbols)) {
        /* extend pool before getting exhausted */
        if (-da_get_check (d, s) == da_get_free_list (d)) {
            if (!da_extend_pool (d, d->num_cells))
                return TRIE_INDEX_ERROR;
        }

        s = -da_get_check (d, s);
    }

    return s - first_sym;
}

static Bool
da_fit_symbols     (DArray         *d,
                    TrieIndex       base,
                    const Symbols  *symbols)
{
    int         i;

    for (i = 0; i < symbols_num (symbols); i++) {
        TrieChar    sym = symbols_get (symbols, i);

        /* if (base + sym) > TRIE_INDEX_MAX which means it's overflow,
         * or cell [base + sym] is not free, the symbol is not fit.
         */
        if (base > TRIE_INDEX_MAX - sym || !da_check_free_cell (d, base + sym))
            return FALSE;
    }
    return TRUE;
}

static void
da_relocate_base   (DArray         *d,
                    TrieIndex       s,
                    TrieIndex       new_base)
{
    TrieIndex   old_base;
    Symbols    *symbols;
    int         i;

    old_base = da_get_base (d, s);
    symbols = da_output_symbols (d, s);

    for (i = 0; i < symbols_num (symbols); i++) {
        TrieIndex   old_next, new_next, old_next_base;

        old_next = old_base + symbols_get (symbols, i);
        new_next = new_base + symbols_get (symbols, i);
        old_next_base = da_get_base (d, old_next);

        /* allocate new next node and copy BASE value */
        da_alloc_cell (d, new_next);
        da_set_check (d, new_next, s);
        da_set_base (d, new_next, old_next_base);

        /* old_next node is now moved to new_next
         * so, all cells belonging to old_next
         * must be given to new_next
         */
        /* preventing the case of TAIL pointer */
        if (old_next_base > 0) {
            TrieIndex   c, max_c;

            max_c = MIN_VAL (TRIE_CHAR_MAX, TRIE_INDEX_MAX - old_next_base);
            for  (c = 0; c < max_c; c++) {
                if (da_get_check (d, old_next_base + c) == old_next)
                    da_set_check (d, old_next_base + c, new_next);
            }
        }

        /* free old_next node */
        da_free_cell (d, old_next);
    }

    symbols_free (symbols);

    /* finally, make BASE[s] point to new_base */
    da_set_base (d, s, new_base);
}

static Bool
da_extend_pool     (DArray         *d,
                    TrieIndex       to_index)
{
    TrieIndex   new_begin;
    TrieIndex   i;
    TrieIndex   free_tail;

    if (to_index <= 0 || TRIE_INDEX_MAX <= to_index)
        return FALSE;

    if (to_index < d->num_cells)
        return TRUE;

    d->cells = (DACell *) realloc (d->cells, (to_index + 1) * sizeof (DACell));
    new_begin = d->num_cells;
    d->num_cells = to_index + 1;

    /* initialize new free list */
    for (i = new_begin; i < to_index; i++) {
        da_set_check (d, i, -(i + 1));
        da_set_base (d, i + 1, -i);
    }

    /* merge the new circular list to the old */
    free_tail = -da_get_base (d, da_get_free_list (d));
    da_set_check (d, free_tail, -new_begin);
    da_set_base (d, new_begin, -free_tail);
    da_set_check (d, to_index, -da_get_free_list (d));
    da_set_base (d, da_get_free_list (d), -to_index);

    /* update header cell */
    d->cells[0].check = d->num_cells;

    return TRUE;
}

/**
 * @brief Prune the single branch
 *
 * @param d : the double-array structure
 * @param s : the dangling state to prune off
 *
 * Prune off a non-separate path up from the final state @a s.
 * If @a s still has some children states, it does nothing. Otherwise,
 * it deletes the node and all its parents which become non-separate.
 */
void
da_prune (DArray *d, TrieIndex s)
{
    da_prune_upto (d, da_get_root (d), s);
}

/**
 * @brief Prune the single branch up to given parent
 *
 * @param d : the double-array structure
 * @param p : the parent up to which to be pruned
 * @param s : the dangling state to prune off
 *
 * Prune off a non-separate path up from the final state @a s to the
 * given parent @a p. The prunning stop when either the parent @a p
 * is met, or a first non-separate node is found.
 */
void
da_prune_upto (DArray *d, TrieIndex p, TrieIndex s)
{
    while (p != s && !da_has_children (d, s)) {
        TrieIndex   parent;

        parent = da_get_check (d, s);
        da_free_cell (d, s);
        s = parent;
    }
}

static void
da_alloc_cell      (DArray         *d,
                    TrieIndex       cell)
{
    TrieIndex   prev, next;

    prev = -da_get_base (d, cell);
    next = -da_get_check (d, cell);

    /* remove the cell from free list */
    da_set_check (d, prev, -next);
    da_set_base (d, next, -prev);
}

static void
da_free_cell       (DArray         *d,
                    TrieIndex       cell)
{
    TrieIndex   i, prev;

    /* find insertion point */
    i = -da_get_check (d, da_get_free_list (d));
    while (i != da_get_free_list (d) && i < cell)
        i = -da_get_check (d, i);

    prev = -da_get_base (d, i);

    /* insert cell before i */
    da_set_check (d, cell, -i);
    da_set_base (d, cell, -prev);
    da_set_check (d, prev, -cell);
    da_set_base (d, i, -cell);
}

/**
 * @brief Enumerate entries stored in double-array structure
 *
 * @param d          : the double-array structure
 * @param enum_func  : the callback function to be called on each separate node
 * @param user_data  : user-supplied data to send as an argument to @a enum_func
 *
 * @return boolean value indicating whether all the keys are visited
 *
 * Enumerate all keys stored in double-array structure. For each entry, the
 * user-supplied @a enum_func callback function is called, with the entry key,
 * the separate node, and user-supplied data. Returning FALSE from such
 * callback will stop enumeration and return FALSE.
 */
Bool
da_enumerate (const DArray *d, DAEnumFunc enum_func, void *user_data)
{
    return da_enumerate_recursive (d, da_get_root (d), enum_func, user_data);
}

Bool
da_enumerate_recursive (const DArray   *d,
                        TrieIndex       state,
                        DAEnumFunc      enum_func,
                        void           *user_data)
{
    Bool        ret;
    TrieIndex   base;

    base = da_get_base (d, state);

    if (base < 0) {
        TrieChar   *key;

        key = da_get_state_key (d, state);
        ret = (*enum_func) (key, state, user_data);
        free (key);
    } else {
        Symbols *symbols;
        int      i;

        ret = TRUE;
        symbols = da_output_symbols (d, state);
        for (i = 0; ret && i < symbols_num (symbols); i++) {
            ret = da_enumerate_recursive (d, base + symbols_get (symbols, i),
                                          enum_func, user_data);
        }

        symbols_free (symbols);
    }

    return ret;
}

/*
vi:ts=4:ai:expandtab
*/
