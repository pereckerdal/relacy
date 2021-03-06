/*  Relacy Race Detector
 *  Copyright (c) 2008-2013, Dmitry S. Vyukov
 *  All rights reserved.
 *  This software is provided AS-IS with no warranty, either express or implied.
 *  This software is distributed under a license and may not be copied,
 *  modified or distributed except as expressly authorized under the
 *  terms of the license contained in the file LICENSE in this distribution.
 */

#include "var_data.hpp"

namespace rl
{

var_data::var_data(thread_id_t thread_count)
    : load_acq_rel_timestamp_(thread_count)
    , store_acq_rel_timestamp_(thread_count)
{
}

void var_data::init(thread_info& th)
{
    th.own_acq_rel_order_ += 1;
    store_acq_rel_timestamp_[th.index_] = th.own_acq_rel_order_;
}

bool var_data::store(thread_info& th)
{
    const auto thread_count = store_acq_rel_timestamp_.size();
    for (thread_id_t i = 0; i != thread_count; ++i)
    {
        if (th.acq_rel_order_[i] < store_acq_rel_timestamp_[i])
            return false;
        if (th.acq_rel_order_[i] < load_acq_rel_timestamp_[i])
            return false;
    }

    th.own_acq_rel_order_ += 1;
    store_acq_rel_timestamp_[th.index_] = th.own_acq_rel_order_;
    return true;
}

bool var_data::load(thread_info& th)
{
    const auto thread_count = store_acq_rel_timestamp_.size();
    for (thread_id_t i = 0; i != thread_count; ++i)
    {
        if (th.acq_rel_order_[i] < store_acq_rel_timestamp_[i])
            return false;
    }

    th.own_acq_rel_order_ += 1;
    load_acq_rel_timestamp_[th.index_] = th.own_acq_rel_order_;
    return true;
}

}
