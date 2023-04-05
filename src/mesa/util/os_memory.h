/**************************************************************************
 *
 * Copyright 2010 VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/


/*
 * OS memory management abstractions
 */


#ifndef _OS_MEMORY_H_
#define _OS_MEMORY_H_

<<<<<<< HEAD:src/gallium/auxiliary/os/os_memory.h

#include "pipe/p_config.h"
#include "pipe/p_compiler.h"

#ifdef _WIN32
#define DEBUG_MEMORY_IMPLEMENTATION
#endif

#if defined(PIPE_SUBSYSTEM_EMBEDDED)
=======
#if defined(EMBEDDED_DEVICE)
>>>>>>> 8c140709bc0f1f58ea84d3d609354952cf4caaf1:src/mesa/util/os_memory.h

#ifdef __cplusplus
extern "C" {
#endif

void *
os_malloc(size_t size);

void *
os_calloc(size_t count, size_t size);

void
os_free(void *ptr);

void *
os_realloc(void *ptr, size_t old_size, size_t new_size);

void *
os_malloc_aligned(size_t size, size_t alignment);

void
os_free_aligned(void *ptr);

void *
os_realloc_aligned(void *ptr, size_t oldsize, size_t newsize, size_t alignemnt);

#ifdef __cplusplus
}
#endif

#else

#  include "os_memory_stdc.h"

#endif

#endif /* _OS_MEMORY_H_ */
