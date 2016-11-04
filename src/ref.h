/*
 * ref.h: reference counting macros
 *
 * Copyright (C) 2007 - 2009 Red Hat Inc.
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 *
 * Author: David Lutterkort <dlutter@redhat.com>
 */

#ifndef REF_H_
#define REF_H_

#include <stdlib.h>
#include <stddef.h>
#include <limits.h>
#include <assert.h>

/* Reference counting for pointers to structs with a REF field of type ref_t
 *
 * When a pointer to such a struct is passed into a function that stores
 * it, the function can either "receive ownership", meaning it does not
 * increment the reference count, or it can "take ownership", meaning it
 * increments the reference count. In the first case, the reference is now
 * owned by wherever the function stored it, and not the caller anymore; in
 * the second case, the caller and whereever the reference was stored both
 * own the reference.
 */
// FIXME: This is not threadsafe; incr/decr ref needs to be protected

#define REF_MAX UINT_MAX

typedef unsigned int ref_t;

/* Prevent this function from being inlined; the compiler thinks that the
 * assignment to PTRPTR breaks strict aliasing - it does not since we know
 * that ptrptr is already of type (T **)
 */
ATTRIBUTE_UNUSED ATTRIBUTE_RETURN_CHECK ATTRIBUTE_NOINLINE
static int ref_make_ref(void *ptrptr, size_t size, size_t ref_ofs) {
    *(void**) ptrptr = calloc(1, size);
    if (*(void **)ptrptr == NULL) {
        return -1;
    } else {
        void *ptr = *(void **)ptrptr;
        *((ref_t *) ((char*) ptr + ref_ofs)) = 1;
        return 0;
    }
}

#define make_ref(var)                                           \
    ref_make_ref(&(var), sizeof(*(var)), offsetof(typeof(*(var)), ref))

#define ref(s) (((s) == NULL || (s)->ref == REF_MAX) ? (s) : ((s)->ref++, (s)))

#define unref(s, t)                                                     \
    do {                                                                \
        if ((s) != NULL && (s)->ref != REF_MAX) {                       \
            assert((s)->ref > 0);                                       \
            if (--(s)->ref == 0) {                                      \
                /*memset(s, 255, sizeof(*s));*/                         \
                free_##t(s);                                            \
            }                                                           \
        }                                                               \
        (s) = NULL;                                                     \
    } while(0)

/* Make VAR uncollectable and pin it in memory for eternity */
#define ref_pin(var)   (var)->ref = REF_MAX

#endif
