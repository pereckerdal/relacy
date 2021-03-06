/*  Relacy Race Detector
 *  Copyright (c) 2008-2013, Dmitry S. Vyukov
 *  All rights reserved.
 *  This software is provided AS-IS with no warranty, either express or implied.
 *  This software is distributed under a license and may not be copied,
 *  modified or distributed except as expressly authorized under the
 *  terms of the license contained in the file LICENSE in this distribution.
 */

#pragma once

#include "base.hpp"
#include "context.hpp"
#include "atomic_fence.hpp"
#include "atomic.hpp"
#include "var.hpp"
#include "thread_local.hpp"
#include "test_suite.hpp"
#include "dyn_thread.hpp"

#include "stdlib/mutex.hpp"
#include "stdlib/condition_variable.hpp"
#include "stdlib/semaphore.hpp"
#include "stdlib/event.hpp"

#define VAR_T(x) rl::var<x>
#define TLS_T(T) rl::thread_local_var<T>
#define VAR(x) x($)

#define RL_ERRNO               (rl::get_errno())
