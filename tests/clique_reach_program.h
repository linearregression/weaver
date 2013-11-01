/*
 * ===============================================================
 *    Description:  Multiple reachability requests.
 *
 *        Created:  06/12/2013 12:00:39 AM
 *
 *         Author:  Ayush Dubey, dubey@cs.cornell.edu
 *
 * Copyright (C) 2013, Cornell University, see the LICENSE file
 *                     for licensing agreement
 * ===============================================================
 */

#include "common/clock.h"
#include "client/client.h"
#include "node_prog/node_prog_type.h"
#include "node_prog/reach_program.h"
#include "test_base.h"

#define CRP_REQUESTS 4000
#define NUM_CLIQUES 3
#define CLIQUE_SIZE 1000

void
clique_reach_prog(bool to_exit)
{
    client c(CLIENT_ID);
    int i, num_nodes;
    timespec start, t1, t2, dif;
    std::vector<uint64_t> nodes, edges;
    srand(time(NULL));
    num_nodes = NUM_CLIQUES * CLIQUE_SIZE;
    for (i = 0; i < num_nodes; i++) {
        WDEBUG << "Creating node " << (i+1) << std::endl;
        nodes.emplace_back(c.create_node());
    }
    // creating intra-clique edges
    WDEBUG << "Going to create edges now" << std::endl;
    for (int clique = 0; clique < NUM_CLIQUES; clique++) {
        int offset = clique * CLIQUE_SIZE;
        for (i = 0; i < CLIQUE_SIZE; i++) {
            for (int j = 0; j < CLIQUE_SIZE; j++) {
                if (i==j || (rand() % 10)>3) {
                    continue;
                }
                edges.emplace_back(c.create_edge(nodes[offset+i], nodes[offset+j]));
            }
        }
    }
    // creating inter-clique edges
    for (int i = 0; i < NUM_CLIQUES; i++) {
        for (int j = 0; j < NUM_CLIQUES; j++) {
            if (i==j) {
                continue;
            }
            edges.emplace_back(c.create_edge(nodes[CLIQUE_SIZE/2 + i*CLIQUE_SIZE],
                nodes[CLIQUE_SIZE/2 + j*CLIQUE_SIZE]));
        }
    }
    WDEBUG << "Created graph\n";
    c.commit_graph();
    WDEBUG << "Committed graph\n";
    node_prog::reach_params rp;
    rp.mode = false;
    rp.reachable = false;
    rp.prev_node.loc = COORD_ID;
    
    std::ofstream file, req_time;
    file.open("requests.rec");
    req_time.open("time.rec");
    wclock::get_clock(&t1);
    start = t1;
    // random reachability requests
    for (i = 0; i < CRP_REQUESTS; i++) {
        wclock::get_clock(&t2);
        dif = diff(t1, t2);
        WDEBUG << "Test: i = " << i << ", ";
        WDEBUG << dif.tv_sec << ":" << dif.tv_nsec << std::endl;
        if (i % 10 == 0) {
            dif = diff(start, t2);
            req_time << dif.tv_sec << '.' << dif.tv_nsec << std::endl;
        }
        t1 = t2;
        int first = rand() % num_nodes;
        int second = rand() % num_nodes;
        while (second == first) {
            second = rand() % num_nodes;
        }
        file << first << " " << second << std::endl;
        std::vector<std::pair<uint64_t, node_prog::reach_params>> initial_args;
        rp.dest = nodes[second];
        initial_args.emplace_back(std::make_pair(nodes[first], rp));
        std::unique_ptr<node_prog::reach_params> res = c.run_node_program(node_prog::REACHABILITY, initial_args);
        assert(res->reachable);
    }
    file.close();
    req_time.close();
    dif = diff(start, t2);
    WDEBUG << "Total time taken " << dif.tv_sec << "." << dif.tv_nsec << std::endl;
    std::ofstream stat_file;
    stat_file.open("stats.rec", std::ios::out | std::ios::app);
    stat_file << num_nodes << " " << dif.tv_sec << "." << dif.tv_nsec << std::endl;
    stat_file.close();
    if (to_exit)
        c.exit_weaver();
}
