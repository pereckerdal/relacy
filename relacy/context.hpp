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
#include "context_addr_hash.hpp"
#include "context_base.hpp"
#include "data/condvar_data.hpp"
#include "data/event_data.hpp"
#include "data/generic_mutex_data.hpp"
#include "data/thread_info.hpp"
#include "data/var_data.hpp"
#include "foreach.hpp"
#include "history.hpp"
#include "memory.hpp"
#include "mutex_wrapper.hpp"
#include "random.hpp"
#include "slab_allocator.hpp"
#include "test_params.hpp"
#include "test_result.hpp"
#include "thread_local_ctx.hpp"

#include "random_scheduler.hpp"
#include "full_search_scheduler.hpp"
#include "context_bound_scheduler.hpp"



namespace rl
{

struct park_event
{
    bool is_timed_;
    bool allow_spurious_;

    void output(std::ostream& s) const
    {
        s << "blocking current thread" << (is_timed_ ? " [timed]" : "");
    }
};

struct unpark_event
{
    thread_id_t thread_;

    void output(std::ostream& s) const
    {
        s << "unblocking thread " << thread_;
    }
};

struct yield_event
{
    unsigned count_;

    void output(std::ostream& s) const
    {
        s << "yield(" << count_ << ")";
    }
};



template<typename test_t, typename scheduler_t>
class context_impl
    : thread_local_contxt_impl<context, test_t::params::thread_count>
{
private:
    typedef thread_local_contxt_impl<context, test_t::params::thread_count>
        base_t;
    typedef typename scheduler_t::shared_context_t shared_context_t;

    using base_t::params_;
    using base_t::history_;
    using base_t::threadx_;
    using base_t::disable_preemption_;
    using base_t::disable_alloc_;
    using base_t::invariant_executing;

    static thread_id_t const main_thread_id = -1;
    static thread_id_t const static_thread_count = test_t::params::static_thread_count;
    static thread_id_t const dynamic_thread_count = test_t::params::dynamic_thread_count;
    static thread_id_t const thread_count = test_t::params::thread_count;

    iteration_t                     current_iter_;
    test_result_e                   test_result_;
    string                          test_result_str_;
    fiber_t                         main_fiber_;
    bool                            special_function_executing;
    memory_mgr                      memory_;
    iteration_t                     start_iteration_;
    size_t                          sched_count_;
    scheduler_t                     sched_;
    shared_context_t&               sctx_;
    random_generator                rand_;
    test_t*                         current_test_suite;
    bool                            current_test_suite_constructed;
    bool                            first_thread_;
    timestamp_t                     seq_cst_fence_order_[thread_count];
    context_addr_hash               context_addr_hash_;

    aligned<thread_info> threads_ [thread_count];

    thread_info& threadi()
    {
        return *static_cast<thread_info*>(threadx_);
    }

    slab_allocator<atomic_data>*        atomic_alloc_;
    slab_allocator<var_data>*           var_alloc_;
    slab_allocator<generic_mutex_data>* mutex_alloc_;
    slab_allocator<condvar_data>*       condvar_alloc_;
    slab_allocator<sema_data>*          sema_alloc_;
    slab_allocator<event_data>*         event_alloc_;

    virtual size_t get_addr_hash(void const* p) override
    {
        return context_addr_hash_.get_addr_hash(p);
    }

    virtual atomic_data* atomic_ctor(void* ctx)
    {
        return new (atomic_alloc_->alloc(ctx)) atomic_data(thread_count);
    }

    virtual void atomic_dtor(atomic_data* data)
    {
        static_cast<atomic_data*>(data)->~atomic_data();
        atomic_alloc_->free(static_cast<atomic_data*>(data));
    }

    virtual var_data* var_ctor()
    {
        return new (var_alloc_->alloc()) var_data(thread_count);
    }

    virtual void var_dtor(var_data* data)
    {
        static_cast<var_data*>(data)->~var_data();
        var_alloc_->free(static_cast<var_data*>(data));
    }

    virtual unpark_reason wfmo_park(void** ws,
                                    win_waitable_object** wo,
                                    size_t count,
                                    bool wait_all,
                                    bool is_timed,
                                    debug_info_param info)
    {
			  return waitset::park_current(*this,
                                           reinterpret_cast<waitset**>(ws),
                                           wo, count, wait_all, is_timed, true, info);
    }

public:
    context_impl(test_params& params, shared_context_t& sctx)
        : base_t(thread_count, params)
        , current_iter_(0)
        , start_iteration_(1)
        , sched_(params, sctx, dynamic_thread_count)
        , sctx_(sctx)
    {
        this->context::seq_cst_fence_order_ = this->seq_cst_fence_order_;

        current_test_suite = (test_t*)(::malloc)(sizeof(test_t));
        current_test_suite_constructed = false;

        test_result_ = test_result_success;
        threadx_ = 0;
        special_function_executing = false;
        invariant_executing = false;

        create_main_fiber(main_fiber_);
        set_low_thread_prio();

        if (0 == val(thread_count))
        {
            throw std::logic_error("no threads created");
        }

        atomic_alloc_ = new slab_allocator<atomic_data>();
        var_alloc_ = new slab_allocator<var_data>();
        mutex_alloc_ = new slab_allocator<generic_mutex_data>();
        condvar_alloc_ = new slab_allocator<condvar_data>();
        sema_alloc_ = new slab_allocator<sema_data>();
        event_alloc_ = new slab_allocator<event_data>();

        for (thread_id_t i = 0; i != thread_count; ++i)
        {
            new (&threads_[i]) thread_info(thread_count, i);
            threads_[i].ctx_ = this;
        }

        for (thread_id_t i = 0; i != thread_count; ++i)
        {
            create_fiber(threads_[i].fiber_, &context_impl::fiber_proc, (void*)(intptr_t)i);
        }

        disable_alloc_ = 0;
    }

    ~context_impl()
    {
        disable_alloc_ += 1;

        for (thread_id_t i = 0; i != thread_count; ++i)
        {
            delete_fiber(threads_[i].fiber_);
        }

        delete_main_fiber(main_fiber_);

        // there can be atomic loads and stores etc
        // it's not good place to calling user code
        //destroy_current_test_suite();
        //::free(current_test_suite);

        delete atomic_alloc_;
        delete var_alloc_;
        delete mutex_alloc_;
        delete condvar_alloc_;
        delete sema_alloc_;
        delete event_alloc_;
    }

    void construct_current_test_suite()
    {
        RL_VERIFY(false == current_test_suite_constructed);
        new (current_test_suite) test_t ();
        current_test_suite_constructed = true;
    }

    void destroy_current_test_suite()
    {
        if (current_test_suite_constructed)
        {
            current_test_suite->~test_t();
            current_test_suite_constructed = false;
        }
    }

    virtual void* alloc(size_t size, bool is_array, debug_info_param info)
    {
        disable_alloc_ += 1;
        void* p = memory_.alloc(size);
        disable_alloc_ -= 1;
        RL_HIST_CTX(memory_alloc_event) {p, size, is_array} RL_HIST_END();
        return p;
    }

    virtual void free(void* p, bool is_array, debug_info_param info)
    {
        RL_HIST_CTX(memory_free_event) {p, is_array} RL_HIST_END();
        bool const defer = (0 == sched_.rand(this->is_random_sched() ? 4 : 2, sched_type_mem_realloc));
        disable_alloc_ += 1;
        if (false == memory_.free(p, defer))
            fail_test("incorrect address passed to free() function", test_result_double_free, info);
        disable_alloc_ -= 1;
    }

    size_t prev_alloc_size_;
    debug_info last_info_;

    virtual void* alloc(size_t size)
    {
        if (disable_alloc_)
            return (::malloc)(size);

        prev_alloc_size_ = size;
        disable_alloc_ += 1;
        void* p = (memory_.alloc)(size);
        disable_alloc_ -= 1;
        return p;
    }

    virtual size_t prev_alloc_size()
    {
        size_t sz = prev_alloc_size_;
        prev_alloc_size_ = 0;
        return sz;
    }

    virtual void set_debug_info(debug_info_param info)
    {
        last_info_ = info;
    }

    virtual void free(void* p)
    {
        if (disable_alloc_)
        {
            (::free)(p);
            return;
        }

        disable_alloc_ += 1;
        debug_info const& info = last_info_;
        RL_HIST_CTX(memory_free_event) {p, false} RL_HIST_END();
        bool const defer = (0 == sched_.rand(this->is_random_sched() ? 4 : 2, sched_type_mem_realloc));
        if (false == memory_.free(p, defer))
            fail_test("incorrect address passed to free() function", test_result_double_free, info);
        disable_alloc_ -= 1;
    }

    virtual unpark_reason park_current_thread(bool is_timed,
                                              bool allow_spurious_wakeup,
                                              bool do_switch,
                                              debug_info_param info)
    {
        RL_VERIFY(false == special_function_executing);
        RL_VERIFY(threadx_->saved_disable_preemption_ == -1);
        unsigned dp = disable_preemption_;
        disable_preemption_ = 0;
        RL_HIST_CTX(park_event) {is_timed, allow_spurious_wakeup} RL_HIST_END();
        if (false == sched_.park_current_thread(is_timed, allow_spurious_wakeup))
        {
            fail_test("deadlock detected", test_result_deadlock, info);
        }
        schedule(1);
        // otherwise it's restored in switch_back()
        RL_VERIFY(threadx_->saved_disable_preemption_ == -1);
        if (do_switch == false || threadx_->unpark_reason_ != unpark_reason_normal)
            disable_preemption_ = dp;
        else
            threadx_->saved_disable_preemption_ = dp;
        unpark_reason reason = threadx_->unpark_reason_;
        return reason;
    }

    virtual void unpark_thread(thread_id_t th, bool do_switch, debug_info_param info)
    {
        RL_VERIFY(false == special_function_executing);
        RL_HIST_CTX(unpark_event) {th} RL_HIST_END();
        sched_.unpark_thread(th, do_switch);
        if (do_switch)
        {
            threads_[th].unpark_reason_ = unpark_reason_normal;
            threads_[th].temp_switch_from_ = threadx_->index_;
            switch_to_fiber(th);
        }
    }

    virtual void switch_back(debug_info_param info)
    {
//std::cout << "switching back from " << threadx_->index_ << " to " << threadx_->temp_switch_from_ << std::endl;
        (void)info;
        RL_VERIFY(threadx_->saved_disable_preemption_ != -1);
        RL_VERIFY(threadx_->temp_switch_from_ != -1);
        thread_id_t const tid = threadx_->temp_switch_from_;
        threadx_->temp_switch_from_ = -1;
        switch_to_fiber(tid);
        RL_VERIFY(threadx_->saved_disable_preemption_ != -1);
        disable_preemption_ = threadx_->saved_disable_preemption_;
        threadx_->saved_disable_preemption_ = -1;
    }

    void ensure(bool cond, char const* desc, test_result_e res, debug_info_param info)
    {
        if (false == cond)
            fail_test(desc, res, info);
    }

    virtual void fail_test(char const* desc, test_result_e res, debug_info_param info)
    {

        RL_DEBUGBREAK_ON_FAILURE_IMPL;

        RL_VERIFY(test_result_success != res);

        test_result_ = res;
        if (test_result_user_assert_failed == res && invariant_executing)
            test_result_ = test_result_user_invariant_failed;
        if (0 == desc || 0 == desc[0])
            test_result_str_ = test_result_str(test_result_);
        else
            test_result_str_ = string(test_result_str(test_result_)) + " (" + desc + ")";

        RL_HIST_CTX(user_event) {test_result_str_.c_str()} RL_HIST_END();

        switch_to_main_fiber();
    }

    virtual void rl_until(char const* desc, debug_info_param info)
    {
        RL_HIST_CTX(user_event) {desc} RL_HIST_END();
        test_result_ = test_result_until_condition_hit;
        switch_to_main_fiber();
    }

    static void fiber_proc(void* thread_index);

    virtual void fiber_proc_impl(int thread_index)
    {
        thread_info* param = &threads_[thread_index];
        debug_info info = $;
        for (;;)
        {
            if (first_thread_)
            {
                first_thread_ = false;
                special_function_executing = true;
                RL_HIST_CTX(user_event) {"[CTOR BEGIN]"} RL_HIST_END();
                construct_current_test_suite();
                RL_HIST_CTX(user_event) {"[CTOR END]"} RL_HIST_END();
                RL_HIST_CTX(user_event) {"[BEFORE BEGIN]"} RL_HIST_END();
                current_test_suite->before();
                RL_HIST_CTX(user_event) {"[BEFORE END]"} RL_HIST_END();
                rl_global_fence();
                invariant_executing = true;
                current_test_suite->invariant();
                invariant_executing = false;
                special_function_executing = false;
            }

//std::cout << "thread " << param->index_ << " started" << std::endl;
            param->on_start();

            if (param->index_ < static_thread_count)
            {
                current_test_suite->thread(param->index_);
            }
            else
            {
                if (param->dynamic_thread_func_)
                    param->dynamic_thread_func_(param->dynamic_thread_param_);
            }

//std::cout << "thread " << param->index_ << " finished" << std::endl;
            RL_HIST_CTX(user_event) {"[THREAD FINISHED]"} RL_HIST_END();
            RL_VERIFY(disable_preemption_ == 0);
            RL_VERIFY(threadx_->temp_switch_from_ == -1);
            RL_VERIFY(threadx_->saved_disable_preemption_ == -1);

            param->on_finish();

            thread_finish_result res = sched_.thread_finished();
//std::cout << "thread " << param->index_ << " finished res=" << res << std::endl;
            if (thread_finish_result_normal == res)
            {
                sched();
            }
            else if (thread_finish_result_last == res)
            {
                special_function_executing = true;
                invariant_executing = true;
                current_test_suite->invariant();
                invariant_executing = false;
                rl_global_fence();
                RL_HIST_CTX(user_event) {"[AFTER BEGIN]"} RL_HIST_END();
                current_test_suite->after();
                RL_HIST_CTX(user_event) {"[AFTER END]"} RL_HIST_END();
                RL_HIST_CTX(user_event) {"[DTOR BEGIN]"} RL_HIST_END();
                destroy_current_test_suite();
                RL_HIST_CTX(user_event) {"[DTOR END]"} RL_HIST_END();
                special_function_executing = false;

                ensure(memory_.iteration_end(), "memory leak detected", test_result_memory_leak, $);
                ensure(atomic_alloc_->iteration_end(), "atomic leak", test_result_resource_leak, $);
                ensure(var_alloc_->iteration_end(), "var leak", test_result_resource_leak, $);
                ensure(mutex_alloc_->iteration_end(), "mutex leak", test_result_resource_leak, $);
                ensure(condvar_alloc_->iteration_end(), "condition variable leak", test_result_resource_leak, $);
                ensure(sema_alloc_->iteration_end(), "semaphore leak", test_result_resource_leak, $);
                ensure(event_alloc_->iteration_end(), "event leak", test_result_resource_leak, $);

                switch_to_main_fiber();
            }
            else if (thread_finish_result_deadlock == res)
            {
                fail_test("deadlock detected", test_result_deadlock, info);
            }
            else
            {
                RL_VERIFY(false);
            }
        }
    }

    virtual win_waitable_object* create_thread(void*(*fn)(void*), void* ctx)
    {
        RL_VERIFY(fn);
        thread_id_t id = sched_.create_thread();
        threads_[id].dynamic_thread_func_ = fn;
        threads_[id].dynamic_thread_param_ = ctx;
        threads_[id].sync_object_.on_create();
        return &threads_[id].sync_object_;
    }

    virtual win_waitable_object* get_thread(thread_id_t id)
    {
        return &threads_[id].sync_object_;
    }

    virtual void yield(unsigned count, debug_info_param info)
    {
        RL_VERIFY(count);
        RL_HIST_CTX(yield_event) {count} RL_HIST_END();
        if (sched_count_++ > params_.execution_depth_limit)
            fail_test("livelock", test_result_livelock, RL_INFO);
        schedule(count);
    }

    virtual void sched()
    {
        if (sched_count_++ > params_.execution_depth_limit)
            fail_test("livelock", test_result_livelock, RL_INFO);
        if (disable_preemption_)
            return;
        schedule(0);
    }

    void schedule(unsigned yield)
    {
        RL_VERIFY(threadx_->temp_switch_from_ == -1);
        RL_VERIFY(disable_preemption_ == 0);
        if (special_function_executing)
        {
            threadx_->unpark_reason_ = unpark_reason_normal;
            return;
        }

        special_function_executing = true;
        invariant_executing = true;
        current_test_suite->invariant();
        invariant_executing = false;
        special_function_executing = false;

        if (yield)
            threadx_->last_yield_ = threadi().own_acq_rel_order_;

        unpark_reason reason = unpark_reason_normal;
        thread_id_t const th = sched_.schedule(reason, yield);
        threads_[th].unpark_reason_ = reason;

        switch_to_fiber(th);
        RL_VERIFY(0 == disable_preemption_);
    }

    test_result_e simulate(std::ostream& ss, std::istream& sss, bool second)
    {
        if (EOF != sss.peek())
        {
            sss >> start_iteration_;
            sched_.set_state(sss);
        }

        test_result_e const res = simulate2(second);

        if (test_result_success != res && false == params_.collect_history)
        {
            ss << params_.stop_iteration << " ";
            sched_.get_state(ss);
        }

        return res;
    }

    test_result_e simulate2(bool second)
    {
        debug_info info = $;

        current_iter_ = start_iteration_;
        for (; ; ++current_iter_)
        {
            rand_.seed(current_iter_);

            iteration(current_iter_);

            if (test_result_success != test_result_)
            {
                params_.test_result = test_result_;
                params_.stop_iteration = current_iter_;
                if (params_.collect_history)
                    output_history();
                return test_result_;
            }

            // If you hit assert here, then probably your test is non-deterministic
            // Check whether you are using functions like ::rand()
            // or static variables or values of object addresses (for hashing) in your test
            // Replace ::rand() with rl::rand(), eliminate static variables in the test
            RL_VERIFY(second == false);
            (void)second;

            RL_HIST_CTX(user_event) {"ITERATION END"} RL_HIST_END();

            if (sched_.iteration_end())
                break;
        }

        params_.test_result = test_result_success;
        params_.stop_iteration = current_iter_;
        return test_result_success;
    }

    RL_INLINE static void reset_thread(thread_info& ti)
    {
        std::fill(ti.acquire_fence_order_.begin(), ti.acquire_fence_order_.end(), 0);
        std::fill(ti.release_fence_order_.begin(), ti.release_fence_order_.end(), 0);
    }

    void iteration(iteration_t iter)
    {
        first_thread_ = true;
        disable_preemption_ = 0;
        sched_count_ = 0;

        foreach<thread_count>(
            threads_,
            &context_impl::reset_thread);

        foreach<thread_count>(
            seq_cst_fence_order_,
            &assign_zero);

        context_addr_hash_.iteration_begin();
        base_t::iteration_begin();

        for (thread_id_t i = 0; i != thread_count; ++i)
        {
            threads_[i].iteration_begin();
        }

        disable_alloc_ += 1;
        thread_id_t const th = sched_.iteration_begin(iter);
        disable_alloc_ -= 1;
        switch_to_fiber(th);

        if (0 == iter % progress_probe_period)
        {
            output_progress(iter);
        }
    }

private:
    void switch_to_fiber(thread_id_t th)
    {
        fiber_t& prev = threadx_ ? threadx_->fiber_ : main_fiber_;
        threadx_ = &threads_[th];
        ::switch_to_fiber(threadx_->fiber_, prev);
    }

    void switch_to_main_fiber()
    {
        fiber_t& prev = threadx_->fiber_;
        threadx_ = 0;
        ::switch_to_fiber(main_fiber_, prev);
    }

    void output_progress(iteration_t iter)
    {
        iteration_t const total = sched_.iteration_count();

        if (0 == iter % (progress_probe_period * 16))
        {
            disable_alloc_ += 1;
            *params_.progress_stream << iter * 100 / total << "% ("
                << iter << "/" << total << ")" << std::endl;
            disable_alloc_ -= 1;
        }
    }

    virtual unsigned rand(unsigned limit, sched_type t)
    {
        return sched_.rand(limit, t);
    }

    void output_history()
    {
        if (false == params_.output_history)
        {
            *params_.output_stream << test_result_str_ << std::endl;
            *params_.output_stream << "iteration: " << params_.stop_iteration << std::endl;
            *params_.output_stream << std::endl;
        }
        history_.print_exec_history(params_.output_history);

        if (test_result_memory_leak == test_result_)
        {
            memory_.output_allocs(*params_.output_stream);
        }

        //!!! output other leaked resources
        if (test_result_ == test_result_resource_leak
            && atomic_alloc_->iteration_end() == false)
        {
            *params_.output_stream << "leaked atomics:" << std::endl;
            atomic_alloc_->output_allocs(*params_.output_stream);
        }
    }

    void rl_global_fence()
    {
        timestamp_t max_acq_rel = 0;
        for (thread_id_t i = 0; i != thread_count; ++i)
        {
            if (threads_[i].acq_rel_order_[i] > max_acq_rel)
                max_acq_rel = threads_[i].acq_rel_order_[i];
        }

        for (thread_id_t i = 0; i != thread_count; ++i)
        {
            for (thread_id_t j = 0; j != thread_count; ++j)
            {
                threads_[i].acq_rel_order_[j] = max_acq_rel;
            }
        }
    }

    virtual void atomic_thread_fence_acquire()
    {
        threadi().atomic_thread_fence_acquire();
    }

    virtual void atomic_thread_fence_release()
    {
        threadi().atomic_thread_fence_release();
    }

    virtual void atomic_thread_fence_acq_rel()
    {
        threadi().atomic_thread_fence_acq_rel();
    }

    virtual void atomic_thread_fence_seq_cst()
    {
        sched();
        threadi().atomic_thread_fence_seq_cst(seq_cst_fence_order_);
    }

    virtual thread_id_t get_thread_count() const
    {
        return thread_count;
    }

    virtual generic_mutex_data* mutex_ctor(bool is_rw, bool is_exclusive_recursive, bool is_shared_recursive, bool failing_try_lock)
    {
        return new (mutex_alloc_->alloc()) generic_mutex_data(thread_count, is_rw, is_exclusive_recursive, is_shared_recursive, failing_try_lock);
    }

    virtual void mutex_dtor(generic_mutex_data* m)
    {
        generic_mutex_data* mm = static_cast<generic_mutex_data*>(m);
        mm->~generic_mutex_data();
        mutex_alloc_->free(mm);
    }

    virtual condvar_data* condvar_ctor(bool allow_spurious_wakeups)
    {
        return new (condvar_alloc_->alloc()) condvar_data(thread_count, allow_spurious_wakeups);
    }

    virtual void condvar_dtor(condvar_data* cv)
    {
        condvar_data* mm = static_cast<condvar_data*>(cv);
        mm->~condvar_data();
        condvar_alloc_->free(mm);
    }

    virtual sema_data* sema_ctor(bool spurious_wakeups, unsigned initial_count, unsigned max_count)
    {
        return new (sema_alloc_->alloc()) sema_data(thread_count, spurious_wakeups, initial_count, max_count);
    }

    virtual void sema_dtor(sema_data* cv)
    {
        sema_data* mm = static_cast<sema_data*>(cv);
        mm->~sema_data();
        sema_alloc_->free(mm);
    }

    virtual event_data* event_ctor(bool manual_reset, bool initial_state)
    {
        return new (event_alloc_->alloc()) event_data(thread_count, manual_reset, initial_state);
    }

    virtual void event_dtor(event_data* cv)
    {
        event_data* mm = static_cast<event_data*>(cv);
        mm->~event_data();
        event_alloc_->free(mm);
    }

    context_impl(context_impl const&);
    context_impl& operator = (context_impl const&);
};

/*
template<typename test_t, typename sched_t>
struct thread_params_t
{
    typedef context_impl<test_t, sched_t> context_t;

    //HANDLE                  handle;
    context_t*              ctx;
    ostringstream      oss;
    istringstream*     iss;

    //RL_NOCOPY(thread_params_t);
};


template<typename test_t, typename sched_t>
unsigned __stdcall thread_func(void * ctx)
{
    typedef thread_params_t<test_t, sched_t> params_t;
    params_t& p = *static_cast<params_t*>(ctx);
    p.ctx->simulate(p.oss, *p.iss, false);
    return 0;
}
*/

template<typename test_t, typename sched_t>
test_result_e run_test(test_params& params, std::ostream& oss, bool second)
{
    typedef context_impl<test_t, sched_t> context_t;
    typedef typename sched_t::shared_context_t shared_context_t;
    //typedef thread_params_t<test_t, sched_t> params_t;

    shared_context_t sctx;
    test_result_e res;

    //if (second == false)
    {
        istringstream iss (params.initial_state);
        res = context_t(params, sctx).simulate(oss, iss, second);
    }
    //else
    //{
    //    size_t const thread_count = 2;
    //    vector<params_t*>::type threads (thread_count);
    //    for (size_t i = 0; i != thread_count; i += 1)
    //    {
    //        threads[i] = new params_t;
    //        threads[i]->iss = new istringstream(params.initial_state);
    //        threads[i]->ctx = new context_t(params, sctx);
    //        threads[i]->handle = (HANDLE)(_beginthreadex)(0, 0, &thread_func<test_t, sched_t>, threads[i], 0, 0);
    //    }

    //    for (size_t i = 0; i != thread_count; i += 1)
    //    {
    //        (WaitForSingleObject)(threads[i]->handle, (INFINITE));
    //    }

    //    for (size_t i = 0; i != thread_count; i += 1)
    //    {
    //        delete threads[i]->ctx;
    //        delete threads[i]->iss;
    //        delete threads[i];
    //    }

    //    return test_result_success;
    //}

    return res;
}


template<typename test_t>
bool simulate(test_params& params)
{
    char const* test_name = typeid(test_t).name();
		while (test_name[0] >= '0' && test_name[0] <= '9')
        test_name += 1;
    params.test_name = test_name;
    *params.output_stream << params.test_name << std::endl;

    unsigned start_time = get_tick_count();

    ostringstream oss;
    //istringstream iss (params.initial_state);
    test_result_e res = test_result_success;
    if (random_scheduler_type == params.search_type)
        res = run_test<test_t, random_scheduler<test_t::params::thread_count> >(params, oss, false);
    else if (fair_full_search_scheduler_type == params.search_type)
        res = run_test<test_t, full_search_scheduler<test_t::params::thread_count> >(params, oss, false);
    else if (fair_context_bound_scheduler_type == params.search_type)
        res = run_test<test_t, context_bound_scheduler<test_t::params::thread_count> >(params, oss, false);
    else
        RL_VERIFY(false);

    if (test_result_success == res)
    {
        unsigned t = get_tick_count() - start_time;
        if (0 == t)
            t = 1;

        *params.output_stream << "iterations: " << params.stop_iteration << std::endl;
        *params.output_stream << "total time: " << t << std::endl;
        *params.output_stream << "throughput: " << (uint64_t)params.stop_iteration * 1000 / t << std::endl;
        *params.output_stream << std::endl;
    }
    else if (false == params.output_history && false == params.collect_history)
    {
        ostringstream oss2;
        params.initial_state = oss.str();
        //istringstream iss2 (oss.str());
        params.collect_history = true;
        params.final_state = oss.str();
        iteration_t const stop_iter = params.stop_iteration;
        test_result_e res2 = test_result_success;
        if (random_scheduler_type == params.search_type)
            res2 = run_test<test_t, random_scheduler<test_t::params::thread_count> >(params, oss2, true);
        else if (fair_full_search_scheduler_type == params.search_type)
            res2 = run_test<test_t, full_search_scheduler<test_t::params::thread_count> >(params, oss2, true);
        else if (fair_context_bound_scheduler_type == params.search_type)
            res2 = run_test<test_t, context_bound_scheduler<test_t::params::thread_count> >(params, oss2, true);
        else
            RL_VERIFY(false);

        // If you hit assert here, then probably your test is non-deterministic
        // Check whether you are using functions like ::rand()
        // or static variables or values of object addresses (for hashing) in your test
        // Replace ::rand() with rl::rand(), eliminate static variables in the test
        RL_VERIFY(res == res2);

        RL_VERIFY(params.stop_iteration == stop_iter);
        (void)stop_iter;
        (void)res2;
    }
    return test_t::params::expected_result == res;
}

template<typename test_t>
bool simulate()
{
    test_params params;
    return simulate<test_t>(params);
}

template<void(*func)(), size_t thread_count>
struct simulate_thunk : test_suite<simulate_thunk<func, thread_count>, 1>
{
    static size_t const dynamic_thread_count = thread_count;
    void thread(unsigned)
    {
        func();
    }
};

template<void(*func)(), size_t thread_count>
bool execute(test_params& params)
{
    return simulate<simulate_thunk<func, thread_count> >(params);
}

template<void(*func)(), size_t thread_count>
bool execute()
{
    return simulate<simulate_thunk<func, thread_count> >();
}

typedef bool (*simulate_f)(test_params&);


template<typename test_t, typename scheduler_t>
void context_impl<test_t, scheduler_t>::fiber_proc(void* thread_index)
{
    ctx().fiber_proc_impl((int)(intptr_t)thread_index);
}

template<typename type>
void dtor_arr_impl(void* pp)
{
    type* p = (type*)((char*)pp + alignment);
    size_t count = *(size_t*)pp;
    for (size_t i = 0; i != count; ++i)
    {
       p->~type();
       p += 1;
    }
}

template<typename type>
type* new_arr_impl(size_t count, rl::debug_info_param info)
{
    RL_VERIFY(alignment >= sizeof(size_t));
    context& c = ctx();
    void* mem = c.alloc(alignment + count * sizeof(type), true, info);
    *(size_t*)mem = count;
    size_t i = 0;
    char* begin = (char*)mem + alignment;
    char* pos = begin;
    try
    {
        for (; i != count; ++i)
        {
            new (pos) type;
            pos += sizeof(type);
        }
        return (type*)begin;
    }
    catch (...)
    {
        pos -= sizeof(type);
        i -= 1;
        for (; i < count; --i)
        {
            ((type*)pos)->~type();
            pos -= sizeof(type);
        }
        ctx().free(mem, true, info);
        throw;
    }
}

template<typename type>
void delete_arr_impl(type* p, debug_info_param info)
{
    if (p == 0)
        return;
    context& c = ctx();
    char* begin = (char*)p - alignment;
    size_t count = *(size_t*)begin;
    for (size_t i = 0; i != count; ++i)
    {
       p->~type();
       p += 1;
    }
    c.free(begin, true, info);
}

template<typename type>
void delete_impl(type* p, debug_info_param info)
{
    p->~type();
    ctx().free(p, false, info);
}

template<typename type>
void dtor_impl(void* p)
{
    static_cast<type*>(p)->~type();
}

inline unsigned rand(unsigned limit)
{
    return ctx().rand(limit, sched_type_user);
}

inline unsigned thread_index()
{
    return ctx().threadx_->index_;
}


struct new_proxy
{
    debug_info info;
    new_proxy(debug_info_param info)
        : info(info)
    {
        //printf(__FUNCSIG__ "\n");
    }

    template<typename T>
    T* operator % (T* p)
    {
        context& c = ctx();
        size_t sz = c.prev_alloc_size();
        if (sz)
        {
            RL_HIST(memory_alloc_event) {p, sz, false} RL_HIST_END();
        }
        return p;
    }
};

struct delete_proxy
{
    //debug_info info_;
    delete_proxy(debug_info_param info)
        //: info_(info)
    {
        ctx().set_debug_info(info);
        //printf(__FUNCSIG__ "\n");
    }
};

inline void* malloc(size_t sz, debug_info_param info)
{
    return ctx().alloc(sz, false, info);
}

inline void* calloc(size_t sz, size_t cnt, debug_info_param info)
{
    void* p = ctx().alloc(sz * cnt, false, info);
    memset(p, 0, sz * cnt);
    return p;
}

inline void* realloc(void* p, size_t sz, debug_info_param info)
{
    if (sz == 0)
    {
        ctx().free(p, false, info);
        return 0;
    }
    else
    {
        void* pp = ctx().alloc(sz, false, info);
        memcpy(pp, p, sz); //!!! how much memory to move?
        ctx().free(p, false, info);
        return pp;
    }
}

inline void free(void* p, debug_info_param info)
{
    ctx().free(p, false, info);
}

inline size_t hash_ptr(void const* p, size_t size)
{
    return ctx().get_addr_hash(p) % size;
}

inline void systemwide_fence(debug_info_param info)
{
    context& c = ctx();
    RL_HIST(user_msg_event) {"system-wide fence"} RL_HIST_END();
    c.rl_global_fence();
}

} // namespace rl

inline void* operator new (size_t size, rl::debug_info_param info)
{
    return rl::ctx().alloc(size, false, info);
}

inline void* operator new [] (size_t size, rl::debug_info_param info)
{
    return rl::ctx().alloc(size, false, info);
}

inline void operator delete (void* p, rl::debug_info_param info)
{
    rl::ctx().free(p, false, info);
}

inline void operator delete [] (void* p, rl::debug_info_param info)
{
    rl::ctx().free(p, false, info);
}

inline void* operator new (size_t size) RL_THROW_SPEC(std::bad_alloc)
{
    if (rl::has_ctx())
        return rl::ctx().alloc(size);
    else
        return (::malloc)(size);
}

inline void* operator new [] (size_t size) RL_THROW_SPEC(std::bad_alloc)
{
    if (rl::has_ctx())
        return rl::ctx().alloc(size);
    else
        return (::malloc)(size);
}

inline void operator delete (void* p) throw()
{
    if (rl::has_ctx())
        rl::ctx().free(p);
    else
        (::free)(p);
}

inline void operator delete [] (void* p) throw()
{
    if (rl::has_ctx())
        rl::ctx().free(p);
    else
        (::free)(p);
}

#define RL_NEW rl::new_proxy($) % new
#define RL_DELETE rl::delete_proxy($) , delete
