/*
 * ===============================================================
 *    Description:  Implementation of shard queue manager.
 *
 *        Created:  2014-02-20 16:53:14
 *
 *         Author:  Ayush Dubey, dubey@cs.cornell.edu
 *
 * Copyright (C) 2013-2014, Cornell University, see the LICENSE
 *                     file for licensing agreement
 * ===============================================================
 */

#include <unordered_map>

#define weaver_debug_
#include "common/event_order.h"
#include "db/queue_manager.h"

using db::queue_order;
using db::queue_manager;
using db::queued_request;

queue_manager :: queue_manager()
    : rd_queues(NUM_VTS, pqueue_t())
    , wr_queues(NUM_VTS, pqueue_t())
    , last_clocks(NUM_VTS, vc::vclock_t(NUM_VTS, 0))
    , qts(NUM_VTS, 0)
{ }

void
queue_manager :: enqueue_read_request(uint64_t vt_id, queued_request *t)
{
    queue_mutex.lock();
    rd_queues[vt_id].push(t);
    queue_mutex.unlock();
}

// check if the read request received in thread loop can be executed without waiting
bool
queue_manager :: check_rd_request(vc::vclock_t &clk)
{
    queue_mutex.lock();
    bool check = check_rd_req_nonlocking(clk);
    queue_mutex.unlock();
    return check;
}

void
queue_manager :: enqueue_write_request(uint64_t vt_id, queued_request *t)
{
    queue_mutex.lock();
    wr_queues[vt_id].push(t);
    queue_mutex.unlock();
}

// check if write request received in thread loop can be executed without waiting
// this does not call Kronos, so if vector clocks cannot be compared, the request will be pushed on write queue
enum queue_order
queue_manager :: check_wr_request(vc::vclock &vclk, uint64_t qt)
{
    enum queue_order ret = FUTURE;
    queue_mutex.lock();
    enum queue_order cur_order = check_wr_queues_timestamps(vclk.vt_id, qt);
    if (cur_order == PAST) {
        ret = PAST;
    } else if (cur_order == PRESENT) {
        // all write queues (possibly except vt_id) good to go
        if (NUM_VTS == 1) {
            ret = PRESENT;
        } else {
            // compare vector clocks, NO Kronos call
            std::vector<vc::vclock> timestamps;
            timestamps.reserve(NUM_VTS);
            for (uint64_t i = 0; i < NUM_VTS; i++) {
                if (i == vclk.vt_id) {
                    timestamps.emplace_back(vclk);
                } else {
                    timestamps.emplace_back(wr_queues[i].top()->vclock);
                }
            }
            std::vector<bool> large;
            int64_t small_idx = INT64_MAX;
            order::compare_vts_no_kronos(timestamps, large, small_idx);
            if ((uint64_t)small_idx == vclk.vt_id) {
                ret = PRESENT;
            } else {
                ret = FUTURE;
            }
        }
    } else {
        ret = FUTURE;
    }
    queue_mutex.unlock();
    return ret;
}

// check all read and write queues
// execute a single queued request which can be run now, and return true
// else return false
bool
queue_manager :: exec_queued_request(db::hyper_stub *hstub)
{
    queue_mutex.lock(); // prevent more jobs from being added
    queued_request *req = get_rw_req();
    queue_mutex.unlock();
    if (req == NULL) {
        return false;
    }
    (*req->func)(hstub, req->arg);
    // queue timestamp is incremented by the thread, upon enqueueing this tx on node queue
    // when to increment qts depends on the tx components
    delete req;
    return true;
}

// increment queue timestamp for a tx which has been ordered
void
queue_manager :: increment_qts(uint64_t vt_id, uint64_t incr)
{
    queue_mutex.lock();
    qts[vt_id] += incr;
    queue_mutex.unlock();
}

// record the vclk for last completed write tx
void
queue_manager :: record_completed_tx(vc::vclock &tx_clk)
{
    queue_mutex.lock();
    if (last_clocks[tx_clk.vt_id][tx_clk.vt_id] < tx_clk.clock[tx_clk.vt_id]) {
        last_clocks[tx_clk.vt_id] = tx_clk.clock;
    }
    queue_mutex.unlock();
}

// initialize queue manager qts from backup
// last clocks is not made persistent because nops will reinitialize them
void
queue_manager :: restore_backup(std::unordered_map<uint64_t, uint64_t> &map_qts)
{
    for (uint64_t i = 0; i < NUM_VTS; i++) {
        assert(map_qts.find(i) != map_qts.end());
        qts[i] = map_qts[i];
    }
}

void
queue_manager :: set_qts(uint64_t vt_id, uint64_t rec_qts)
{
    queue_mutex.lock();
    if (qts[vt_id] < rec_qts) {
        WDEBUG << "Current qts for vt " << vt_id << " is " << qts[vt_id] << ", setting to " << rec_qts << std::endl;
        qts[vt_id] = rec_qts;
    } else {
        WDEBUG << "set qts from vt " << vt_id << " not needed, current qts " << qts[vt_id] << ", rec qts " << rec_qts << std::endl;
    }
    queue_mutex.unlock();
}

bool
queue_manager :: check_rd_req_nonlocking(vc::vclock_t &clk)
{
    for (uint64_t i = 0; i < NUM_VTS; i++) {
        if (order::compare_two_clocks(clk, last_clocks[i]) != 0) {
            return false;
        }
    }
    return true;
}

queued_request*
queue_manager :: get_rd_req()
{
    queued_request *req;
    for (uint64_t vt_id = 0; vt_id < NUM_VTS; vt_id++) {
        pqueue_t &pq = rd_queues[vt_id];
        // execute read request after all write queues have processed write which happens after this read
        if (!pq.empty()) {
            req = pq.top();
            if (check_rd_req_nonlocking(req->vclock.clock)) {
                pq.pop();
                return req;
            }
        }
    }
    return NULL;
}

enum queue_order
queue_manager :: check_wr_queues_timestamps(uint64_t vt_id, uint64_t qt)
{
    // check each write queue ready to go
    for (uint64_t i = 0; i < NUM_VTS; i++) {
        if (vt_id == i) {
            if (qt <= qts[i]) {
                return PAST;
            } else if (qt != (qts[i]+1)) {
                return FUTURE;
            }
            //if ((qts[i] + 1) != qt) {
            //    return false;
            //}
        } else {
            pqueue_t &pq = wr_queues[i];
            if (pq.empty()) { // can't go on if one of the pq's is empty
                return FUTURE;
            } else {
                // check for correct ordering of queue timestamp (which is priority for thread)
                if ((qts[i] + 1) != pq.top()->priority) {
                    return FUTURE;
                }
            }
        }
    }
    return PRESENT;
}

queued_request*
queue_manager :: get_wr_req()
{
    enum queue_order queue_status = check_wr_queues_timestamps(UINT64_MAX, UINT64_MAX);
    assert(queue_status != PAST);
    if (queue_status == FUTURE) {
        return NULL;
    }

    // all write queues are good to go
    uint64_t exec_vt_id;
    if (NUM_VTS == 1) {
        exec_vt_id = 0; // only one timestamper
    } else {
        // compare timestamps, may call Kronos
        std::vector<vc::vclock> timestamps;
        timestamps.reserve(NUM_VTS);
        for (uint64_t vt_id = 0; vt_id < NUM_VTS; vt_id++) {
            timestamps.emplace_back(wr_queues[vt_id].top()->vclock);
            assert(timestamps.back().clock.size() == NUM_VTS);
        }
        exec_vt_id = order::compare_vts(timestamps);
    }
    queued_request *req = wr_queues[exec_vt_id].top();
    wr_queues[exec_vt_id].pop();
    return req;
}


queued_request*
queue_manager :: get_rw_req()
{
    queued_request *req = get_rd_req();
    if (req == NULL) {
        req = get_wr_req();
    }
    return req;
}
