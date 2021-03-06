/*  Relacy Race Detector
 *  Copyright (c) 2008-2013, Dmitry S. Vyukov
 *  All rights reserved.
 *  This software is provided AS-IS with no warranty, either express or implied.
 *  This software is distributed under a license and may not be copied,
 *  modified or distributed except as expressly authorized under the
 *  terms of the license contained in the file LICENSE in this distribution.
 */

#pragma once

#ifndef _XOPEN_SOURCE
#    define _XOPEN_SOURCE
#endif

#include <csetjmp>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sys/times.h>
#include <unistd.h>
#include <ucontext.h>

inline unsigned get_tick_count()
{
    struct tms tms;
    return ((unsigned)(times (&tms) * (1000 / sysconf(_SC_CLK_TCK))));
}

inline void set_low_thread_prio()
{
}

struct fiber_t
{
    ucontext_t  fib;
    jmp_buf     jmp;
};

struct fiber_ctx_t
{
    void(*      fnc)(void*);
    void*       ctx;
    jmp_buf*    cur;
    ucontext_t* prv;
};

static void fiber_start_fnc(void* p)
{
    fiber_ctx_t* ctx = (fiber_ctx_t*)p;
    void (*volatile ufnc)(void*) = ctx->fnc;
    void* volatile uctx = ctx->ctx;
    if (_setjmp(*ctx->cur) == 0)
    {
        ucontext_t tmp;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        swapcontext(&tmp, ctx->prv);
#pragma clang diagnostic pop
    }
    ufnc(uctx);
}

inline void create_main_fiber(fiber_t& fib)
{
    memset(&fib, 0, sizeof(fib));
}

inline void delete_main_fiber(fiber_t& fib)
{
    (void)fib;
}

inline void create_fiber(fiber_t& fib, void(*ufnc)(void*), void* uctx)
{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    size_t const stack_size = 64*1024;
    getcontext(&fib.fib);
    fib.fib.uc_stack.ss_sp = (::malloc)(stack_size);
    fib.fib.uc_stack.ss_size = stack_size;
    fib.fib.uc_link = 0;
    ucontext_t tmp;
    fiber_ctx_t ctx = {ufnc, uctx, &fib.jmp, &tmp};
    makecontext(&fib.fib, (void(*)())fiber_start_fnc, 1, &ctx);
    swapcontext(&tmp, &fib.fib);
#pragma clang diagnostic pop
}

inline void delete_fiber(fiber_t& fib)
{
    //(::free)(fib.uc_stack.ss_sp);
}

inline void switch_to_fiber(fiber_t& fib, fiber_t& prv)
{
    if (_setjmp(prv.jmp) == 0)
        _longjmp(fib.jmp, 1);
}

#ifdef _MSC_VER
    typedef unsigned __int64 uint64_t;
#   define RL_INLINE __forceinline
#   define RL_NOINLINE __declspec(noinline)
#   define RL_STRINGIZE(text) RL_STRINGIZE_A((text))
#   define RL_STRINGIZE_I(text) #text
#   define RL_STRINGIZE_A(arg) RL_STRINGIZE_I arg
#   define RL_STDCALL __stdcall
#   define RL_THROW_SPEC(ex)
#else
#   define RL_INLINE inline
#   define RL_NOINLINE
#   define RL_STRINGIZE_I(text) #text
#   define RL_STRINGIZE(text) RL_STRINGIZE_I(text)
#   define RL_STDCALL
#   define RL_THROW_SPEC(ex) throw(ex)
#endif


#if defined (_MSC_VER) && (_MSC_VER >= 1400)
#   define RL_RESTRICT __restrict
#else
#   define RL_RESTRICT
#endif
