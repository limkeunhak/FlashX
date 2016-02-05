/*
 * Copyright 2015 Open Connectome Project (http://openconnecto.me)
 * Written by Disa Mhembere (disa@jhu.edu)
 *
 * This file is part of FlashGraph.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY CURRENT_KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <signal.h>
#ifdef PROFILER
#include <gperftools/profiler.h>
#endif

#include "sem_kmeans.h"

using namespace fg;

namespace {

#if KM_TEST
    static prune_stats::ptr g_prune_stats;
#endif
    static unsigned g_io_reqs = 0;

    static bool g_prune_init = false;
    static dist_matrix::ptr g_cluster_dist;
    static std::vector<prune_cluster::ptr> g_clusters; // cluster means/centers

    static unsigned NUM_ROWS;
    static unsigned g_num_changed = 0;
    static struct timeval start, end;
    static init_type_t g_init; // May have to use
    static unsigned  g_kmspp_cluster_idx; // Used for kmeans++ init
    static unsigned g_kmspp_next_cluster; // Sample row selected as the next cluster
    static kmspp_stage_t g_kmspp_stage; // Either adding a mean / computing dist
    static kms_stage_t g_stage; // What phase of the algo we're in
    static unsigned g_iter;

    class kmeans_vertex: public base_kmeans_vertex
    {
        bool recalculated;
        double dist;

        public:
        kmeans_vertex(vertex_id_t id): base_kmeans_vertex(id) {
            recalculated = false;
            dist = std::numeric_limits<double>::max(); // Start @ max
        }

        void run(vertex_program &prog);

        void run(vertex_program& prog, const page_vertex &vertex) {
            switch (g_stage) {
                case INIT:
                    run_init(prog, vertex, g_init);
                    break;
                case ESTEP:
                    run_distance(prog, vertex);
                    break;
                default:
                    BOOST_ASSERT_MSG(0, "Unknown g_stage!");
            }
        }

        const double get_dist() const { return dist; }
        void set_dist(const double dist) { this->dist = dist; }

        void run_on_message(vertex_program& prog, const vertex_message& msg) { }
        void run_init(vertex_program& prog, const page_vertex &vertex, init_type_t init);
        void run_distance(vertex_program& prog, const page_vertex &vertex);
        double get_distance(unsigned cl, data_seq_iter& count_it);
        double dist_comp(const page_vertex &vertex, const unsigned cl);
    };

    class kmeans_vertex_program:
        public base_kmeans_vertex_program<kmeans_vertex, cluster>
    {
        unsigned num_reqs;
#if KM_TEST
        prune_stats::ptr pt_ps;
#endif

        public:
        typedef std::shared_ptr<kmeans_vertex_program> ptr;

        kmeans_vertex_program() {
            this->num_reqs = 0;
#if KM_TEST
            pt_ps = prune_stats::create(NUM_ROWS, K);
#endif
        }

        static ptr cast2(vertex_program::ptr prog) {
            return std::static_pointer_cast<kmeans_vertex_program, vertex_program>(prog);
        }

        void remove_member(const unsigned id, data_seq_iter& count_it) {
            get_pt_clusters()[id]->remove_member(count_it);
        }

        template <typename T>
            void swap_membership(const unsigned from_id, const unsigned to_id,
                    T& count_it) {
                vertex_id_t nid = 0;
                while(count_it.has_next()) {
                    double e = count_it.next();
                    (*(get_pt_clusters()[from_id]))[nid] -= e;
                    (*(get_pt_clusters()[to_id]))[nid++] += e;
                }

                get_pt_clusters()[from_id]->num_members_peq(-1);
                get_pt_clusters()[to_id]->num_members_peq(1);
            }

#if KM_TEST
        prune_stats::ptr get_ps() { return pt_ps; }
#endif
        void num_requests_pp() {
            num_reqs++;
        }
        const unsigned get_num_reqs() const { return num_reqs; }
    };

    class kmeans_vertex_program_creater: public vertex_program_creater
    {
        public:
            vertex_program::ptr create() const {
                return vertex_program::ptr(new kmeans_vertex_program());
            }
    };

    /* Used in kmeans++ initialization */
    class kmeanspp_vertex_program : public vertex_program_impl<kmeans_vertex>
    {
        double pt_cuml_sum;

        public:
        typedef std::shared_ptr<kmeanspp_vertex_program> ptr;

        kmeanspp_vertex_program() {
            pt_cuml_sum = 0.0;
        }

        static ptr cast2(vertex_program::ptr prog) {
            return std::static_pointer_cast<kmeanspp_vertex_program, vertex_program>(prog);
        }

        void pt_cuml_sum_peq (const double val) {
            pt_cuml_sum += val;
        }

        const double get_pt_cuml_sum() const {
            return pt_cuml_sum;
        }
    };

    class kmeanspp_vertex_program_creater: public vertex_program_creater
    {
        public:
            vertex_program::ptr create() const {
                return vertex_program::ptr(new kmeanspp_vertex_program());
            }
    };

    void kmeans_vertex::run(vertex_program &prog) {
        if (g_kmspp_stage == DIST) {
            if (get_cluster_id() != INVALID_CLUST_ID) {
                if (get_dist() <= g_cluster_dist->get(get_cluster_id(), g_kmspp_cluster_idx)) {
                    // No dist comp, but add my mean
                    ((kmeanspp_vertex_program&)prog).
                        pt_cuml_sum_peq(get_dist());
                    return;
                }
            }
        } else if (g_stage != INIT) {
            recalculated = false;
            if (!g_prune_init) {
                set_dist(get_dist() + g_clusters[get_cluster_id()]->get_prev_dist());

                if (get_dist() <= g_clusters[get_cluster_id()]->get_s_val()) {
#if KM_TEST
                    ((kmeans_vertex_program&) prog).get_ps()->pp_lemma1(K);
#endif
                    return; // Nothing changes -- no I/O request!
                }
                ((kmeans_vertex_program&) prog).num_requests_pp();
            }
        }

        vertex_id_t id = prog.get_vertex_id(*this);
        request_vertices(&id, 1);
    }

    std::string s (const double d) {
        if (d == std::numeric_limits<double>::max())
            return "max";
        else
            return std::to_string(d);
    }

    void kmeans_vertex::run_init(vertex_program& prog,
            const page_vertex &vertex, init_type_t init) {
        switch (g_init) {
            case RANDOM:
                {
                    unsigned new_cluster_id = random() % K;
                    kmeans_vertex_program& vprog = (kmeans_vertex_program&) prog;
#if VERBOSE
                    printf("Random init: v%u assigned to cluster: c%x\n",
                            prog.get_vertex_id(*this), new_cluster_id);
#endif
                    set_cluster_id(new_cluster_id);
                    data_seq_iter count_it = ((const page_row&)vertex).
                        get_data_seq_it<double>();
                    vprog.add_member(get_cluster_id(), count_it);
                }
                break;
            case FORGY:
                {
                    vertex_id_t my_id = prog.get_vertex_id(*this);
#if KM_TEST
                    printf("Forgy init: v%u setting cluster: c%x\n", my_id, g_init_hash[my_id]);
#endif
                    set_as_mean<prune_cluster>(vertex, my_id, g_init_hash[my_id], g_clusters);
                }
                break;
            case PLUSPLUS:
                {
                    data_seq_iter count_it = ((const page_row&)vertex).
                        get_data_seq_it<double>();

                    if (g_kmspp_stage == ADDMEAN) {
#if KM_TEST
                        vertex_id_t my_id = prog.get_vertex_id(*this);
                        printf("kms++ v%u making itself c%u\n", my_id, g_kmspp_cluster_idx);
#endif
                        g_clusters[g_kmspp_cluster_idx]->add_member(count_it);
                    } else if (g_kmspp_stage == DIST) {
                        vertex_id_t my_id = prog.get_vertex_id(*this);
                        double _dist = get_distance(g_kmspp_cluster_idx, count_it);
                        if (_dist < g_kmspp_distance[my_id]) {
#if VERBOSE
                            printf("kms++ v%u updating dist from: %s to %.3f\n",
                                    my_id, (s(g_kmspp_distance[my_id])).c_str(), _dist);
#endif
                            g_kmspp_distance[my_id] = _dist;
                            set_cluster_id(g_kmspp_cluster_idx);
                            set_dist(_dist);
                        }
                        ((kmeanspp_vertex_program&)prog).
                            pt_cuml_sum_peq(g_kmspp_distance[my_id]);
                    } else {
                        BOOST_ASSERT_MSG(0, "Unknown g_kmspp_stage type");
                    }
                }
                break;
            default:
                BOOST_ASSERT_MSG(0, "Unknown g_init type");
        }
    }

    double kmeans_vertex::get_distance(unsigned cl,
            data_seq_iter& count_it) {
        double dist = 0;
        double diff;
        vertex_id_t nid = 0;

        while(count_it.has_next()) {
            double e = count_it.next();
            diff = e - (*g_clusters[cl])[nid++];
            dist += diff*diff;
        }
        return sqrt(dist); // TODO: sqrt
    }

    double kmeans_vertex::dist_comp(const page_vertex &vertex, const unsigned cl) {
        data_seq_iter count_it =
            ((const page_row&)vertex).get_data_seq_it<double>();

        return get_distance(cl, count_it);
    }

    void kmeans_vertex::run_distance(vertex_program& prog, const page_vertex &vertex) {
        kmeans_vertex_program& vprog = (kmeans_vertex_program&) prog;
        unsigned old_cluster_id = get_cluster_id();

        if (g_prune_init) {
            for (unsigned cl = 0; cl < K; cl++) {
                if ((get_cluster_id() != INVALID_CLUST_ID) &&
                        get_dist() <= g_cluster_dist->get(get_cluster_id(), cl)) {
                    // Skip dist comp
                } else {
                    double udist = dist_comp(vertex, cl);
                    if (udist < get_dist()) {
                        set_dist(udist);
                        set_cluster_id(cl);
                    }
                }
            }
        } else {
            for (unsigned cl = 0; cl < K; cl++) {
                if (get_dist() <= g_cluster_dist->get(get_cluster_id(), cl)) {
#if KM_TEST
                    vprog.get_ps()->pp_3a();
#endif
                    continue;
                }

                // If not recalculated to my current cluster .. do so to tighten bounds
                if (!recalculated) {
                    double udist = dist_comp(vertex, get_cluster_id());
                    set_dist(udist);
                    recalculated = true;
                }

                if (get_dist() <= g_cluster_dist->get(get_cluster_id(), cl)) {
#if KM_TEST
                    vprog.get_ps()->pp_3c();
#endif
                    continue;
                }

                // Track 5
                double jdist = dist_comp(vertex, cl);
                if (jdist < get_dist()) {
                    set_dist(jdist);
                    set_cluster_id(cl);
                }
            }
        }

#if KM_TEST
        BOOST_VERIFY(get_cluster_id() >= 0 && get_cluster_id() < K);
#endif
        data_seq_iter count_it = ((const page_row&)vertex).get_data_seq_it<double>();

        if (g_prune_init) {
            vprog.pt_changed_pp(); // Add a vertex to the count of changed ones
            vprog.add_member(get_cluster_id(), count_it);
        } else if (old_cluster_id != get_cluster_id()) {
            vprog.pt_changed_pp(); // Add a vertex to the count of changed ones
            vprog.swap_membership(old_cluster_id, get_cluster_id(), count_it);
        }
    }

        static FG_vector<unsigned>::ptr get_membership(graph_engine::ptr mat) {
            FG_vector<unsigned>::ptr vec = FG_vector<unsigned>::create(mat);
            mat->query_on_all(vertex_query::ptr(new save_query<unsigned, kmeans_vertex>(vec)));
            return vec;
        }

        static void clear_clusters() {
            for (unsigned cl = 0; cl < g_clusters.size(); cl++) {
                g_clusters[cl]->set_prev_mean();

                if (g_prune_init) {
                    g_clusters[cl]->clear();
                } else {
                    g_clusters[cl]->unfinalize();
#if VERBOSE
                    std::cout << "Unfinalized g_clusters[thd] ==> ";
                    print_vector<double>(g_clusters[cl]->get_mean());
#endif
                }
            }
        }

        static void update_clusters(graph_engine::ptr mat,
                std::vector<unsigned>& num_members_v) {
            clear_clusters();
            std::vector<vertex_program::ptr> kms_clust_progs;
            mat->get_vertex_programs(kms_clust_progs);

            for (unsigned thd = 0; thd < kms_clust_progs.size(); thd++) {
                kmeans_vertex_program::ptr kms_prog =
                    kmeans_vertex_program::cast2(kms_clust_progs[thd]);
                std::vector<cluster::ptr> pt_clusters = kms_prog->get_pt_clusters();
                g_num_changed += kms_prog->get_pt_changed();

                g_io_reqs += kms_prog->get_num_reqs();

#if KM_TEST
                (*g_prune_stats) += (*kms_prog->get_ps());
#endif
                BOOST_VERIFY(g_num_changed <= NUM_ROWS);
                /* Merge the per-thread clusters */
                for (unsigned cl = 0; cl < K; cl++) {
                    *(g_clusters[cl]) += *(pt_clusters[cl]);
                    if (thd == kms_clust_progs.size()-1) {
                        g_clusters[cl]->finalize();
                        num_members_v[cl] = g_clusters[cl]->get_num_members();

                        double dist = eucl_dist
                            (&((g_clusters[cl]->get_mean())[0]),
                             &((g_clusters[cl]->get_prev_mean())[0]), NUM_COLS);
#if VERBOSE
                        BOOST_LOG_TRIVIAL(info) << "Distance to prev mean for c:"
                            << cl << " is " << dist;
                        BOOST_VERIFY(g_clusters[cl]->get_num_members() <= (int)NUM_ROWS);
#endif
                        g_clusters[cl]->set_prev_dist(dist);
                    }
                }
            }
#if KM_TEST
            int t_members = 0;
            unsigned cl = 0;
            BOOST_FOREACH(prune_cluster::ptr c , g_clusters) {
                t_members += c->get_num_members();
                if (t_members > (int) NUM_ROWS) {
                    BOOST_LOG_TRIVIAL(error) << "[FATAL]: Too many memnbers cluster: "
                        << cl << "/" << K << " at members = " << t_members;
                    BOOST_VERIFY(false);
                }
                cl++;
            }
#endif
        }

        /* During kmeans++ we select a new cluster each iteration
           This step get the next sample selected as a cluster center
           */
        static unsigned kmeanspp_get_next_cluster_id(graph_engine::ptr mat) {
#if KM_TEST
            BOOST_LOG_TRIVIAL(info) << "Assigning new cluster ...";
#endif
            std::vector<vertex_program::ptr> kmspp_progs;
            mat->get_vertex_programs(kmspp_progs);

            double cuml_sum = 0;
            BOOST_FOREACH(vertex_program::ptr vprog, kmspp_progs) {
                kmeanspp_vertex_program::ptr kmspp_prog =
                    kmeanspp_vertex_program::cast2(vprog);
                cuml_sum += kmspp_prog->get_pt_cuml_sum();
            }

            cuml_sum = (cuml_sum * ((double)random())) / (RAND_MAX-1.0);
            BOOST_ASSERT_MSG(cuml_sum != 0, "Cumulative sum == 0!");

            g_kmspp_cluster_idx++;

            for (unsigned row = 0; row < NUM_ROWS; row++) {
#if VERBOSE
                BOOST_LOG_TRIVIAL(info) << "cuml_sum = " << cuml_sum;
#endif
                cuml_sum -= g_kmspp_distance[row];
                if (cuml_sum <= 0) {
#if KM_TEST
                    BOOST_LOG_TRIVIAL(info) << "Choosing v:" << row
                        << " as center K = " << g_kmspp_cluster_idx;
#endif
                    return row;
                }
            }
            BOOST_ASSERT_MSG(false, "Cumulative sum of distances was > than distances!");
            exit(EXIT_FAILURE);
        }

        // Return all the cluster means only
        static void get_means(std::vector<std::vector<double>>& means) {
            for (std::vector<prune_cluster::ptr>::iterator it = g_clusters.begin();
                    it != g_clusters.end(); ++it) {
                means.push_back((*it)->get_mean());
            }
        }

        static inline bool fexists(const std::string& name) {
            struct stat buffer;
            return (stat (name.c_str(), &buffer) == 0);
        }
    }

    namespace fg
    {
        sem_kmeans_ret::ptr compute_min_triangle_sem_kmeans(FG_graph::ptr fg, const size_t k,
                const std::string init, const unsigned max_iters, const double tolerance,
                const unsigned num_rows, const unsigned num_cols, std::vector<double>* centers) {
#ifdef PROFILER
            ProfilerStart("libgraph-algs/min_tri_sem_kmeans.perf");
#endif
            K = k;

            // Check Initialization
            if ((NULL == centers) && init.compare("random") && init.compare("kmeanspp") &&
                    init.compare("forgy")) {
                BOOST_LOG_TRIVIAL(fatal)
                    << "[ERROR]: init must be one of: 'random', 'forgy', 'kmeanspp'.It is '"
                    << init << "'";
                exit(EXIT_FAILURE);
            }

            graph_index::ptr index = NUMA_graph_index<kmeans_vertex>::create(
                    fg->get_graph_header());
            graph_engine::ptr mat = fg->create_engine(index);

            NUM_ROWS = mat->get_max_vertex_id() + 1;
            NUM_COLS = num_cols;

            // Check k
            if (K > NUM_ROWS || K < 2 || K == (unsigned)-1) {
                BOOST_LOG_TRIVIAL(fatal)
                    << "'k' must be between 2 and the number of rows in the matrix " <<
                    "k = " << K;
                exit(EXIT_FAILURE);
            }

            BOOST_VERIFY(num_cols > 0);

            BOOST_LOG_TRIVIAL(info) << "Matrix has rows = " << NUM_ROWS << ", cols = " <<
                NUM_COLS;
#if KM_TEST
            g_prune_stats = prune_stats::create(NUM_ROWS, K);
#endif

            gettimeofday(&start , NULL);
            /*** Begin VarInit of data structures ***/
            if (centers) {
                set_clusters<prune_cluster>(centers, g_clusters, K, NUM_COLS);
            } else {
                init_clusters<prune_cluster>(g_clusters, K, NUM_COLS);
            }

            FG_vector<unsigned>::ptr cluster_assignments; // Which cluster a sample is in
            std::vector<unsigned> num_members_v(K);

            BOOST_LOG_TRIVIAL(info) << "Init of g_cluster_dist";
            // Distance to everyone other than yourself
            g_cluster_dist = dist_matrix::create(K);
            /*** End VarInit ***/

            if (!centers) {
                g_stage = INIT;

                if (init == "random") {
                    BOOST_LOG_TRIVIAL(info) << "Running init: '"<< init <<"' ...";
                    g_init = RANDOM;

                    mat->start_all(vertex_initializer::ptr(),
                            vertex_program_creater::ptr(new kmeans_vertex_program_creater()));
                    mat->wait4complete();

                    update_clusters(mat, num_members_v);
                }
                if (init == "forgy") {
                    BOOST_LOG_TRIVIAL(info) << "Deterministic Init is: '"<< init <<"'";
                    g_init = FORGY;

                    // Select K in range NUM_ROWS
                    std::vector<vertex_id_t> init_ids; // Used to start engine
                    for (unsigned cl = 0; cl < K; cl++) {
                        vertex_id_t id = random() % NUM_ROWS;
                        g_init_hash[id] = cl; // <vertex_id, cluster_id>
                        init_ids.push_back(id);
                    }
                    mat->start(&init_ids.front(), K);
                    mat->wait4complete();
                } else if (init == "kmeanspp") {
                    BOOST_LOG_TRIVIAL(info) << "Init is '"<< init <<"'";
                    g_init = PLUSPLUS;

                    // Init g_kmspp_distance to max distance
                    g_kmspp_distance.assign(NUM_ROWS, std::numeric_limits<double>::max());

                    g_kmspp_cluster_idx = 0;
                    g_kmspp_next_cluster = random() % NUM_ROWS; // 0 - (NUM_ROWS - 1)

                    BOOST_LOG_TRIVIAL(info) << "Assigning v:" << g_kmspp_next_cluster
                        << " as first cluster";
                    g_kmspp_distance[g_kmspp_next_cluster] = 0;

                    // Fire up K engines with 2 iters/engine
                    while (true) {
                        // TODO: Start 1 vertex which will activate all
                        g_kmspp_stage = ADDMEAN;

                        g_io_reqs++;

                        mat->start(&g_kmspp_next_cluster, 1);
                        mat->wait4complete();

                        // Compute distance matrix
                        g_cluster_dist->compute_dist(g_clusters, g_kmspp_cluster_idx+1);
#if VERBOSE
                        BOOST_LOG_TRIVIAL(info) << "Printing clusters after sample set_mean ...";
                        print_clusters<prune_cluster>(g_clusters);
#endif
                        // skip distance comp since we picked clusters
                        if (g_kmspp_cluster_idx+1 == K) { break; }
                        g_kmspp_stage = DIST;

                        g_io_reqs += NUM_ROWS;

                        mat->start_all(vertex_initializer::ptr(),
                                vertex_program_creater::ptr(new kmeanspp_vertex_program_creater()));
                        mat->wait4complete();
                        g_kmspp_next_cluster = kmeanspp_get_next_cluster_id(mat);
                    }
                }
            } else
                print_clusters<prune_cluster>(g_clusters);


            if (init == "forgy" || init == "kmeanspp" || centers) {
                g_prune_init = true; // set
                g_stage = ESTEP;
                BOOST_LOG_TRIVIAL(info) << "Init: Computing cluster distance matrix ...";
                g_cluster_dist->compute_dist(g_clusters, K);
#if KM_TEST
                BOOST_LOG_TRIVIAL(info) << "Printing inited cluster distance matrix ...";
                g_cluster_dist->print();
#endif

                BOOST_LOG_TRIVIAL(info) << "Init: Running an engine for PRUNE since init is "
                    << init;

                mat->start_all(vertex_initializer::ptr(),
                        vertex_program_creater::ptr(new kmeans_vertex_program_creater()));
                mat->wait4complete();
                BOOST_LOG_TRIVIAL(info) << "Init: M-step Updating cluster means ...";

                update_clusters(mat, num_members_v);
#if KM_TEST
                BOOST_LOG_TRIVIAL(info) << "Init: printing cluster counts:";
                print_vector<unsigned>(num_members_v);
#endif
                g_prune_init = false; // reset
                g_num_changed = 0; // reset
            }

            g_stage = ESTEP;
            BOOST_LOG_TRIVIAL(info) << "SEM-K||means starting ...";

            bool converged = false;

            std::string str_iters = max_iters == std::numeric_limits<unsigned>::max() ?
                "until convergence ...":
                std::to_string(max_iters) + " iterations ...";
            BOOST_LOG_TRIVIAL(info) << "Computing " << str_iters;
            g_iter = 1;

            while (g_iter < max_iters) {
                BOOST_LOG_TRIVIAL(info) << "E-step Iteration " << g_iter <<
                    " . Computing cluster assignments ...";
                BOOST_LOG_TRIVIAL(info) << "Main: Computing cluster distance matrix ...";
                g_cluster_dist->compute_dist(g_clusters, K);

                for (unsigned cl = 0; cl < K; cl++)
                    BOOST_LOG_TRIVIAL(info) << "cl:" << cl << " get_s_val: "
                        << g_clusters[cl]->get_s_val();
#if VERBOSE
                BOOST_LOG_TRIVIAL(info) << "Cluster distance matrix ...";
                g_cluster_dist->print();
                BOOST_LOG_TRIVIAL(info) << "Printing Clusters:";
                print_clusters<prune_cluster>(g_clusters);
#endif
                mat->start_all(vertex_initializer::ptr(),
                        vertex_program_creater::ptr(new kmeans_vertex_program_creater()));
                mat->wait4complete();
                BOOST_LOG_TRIVIAL(info) << "Main: M-step Updating cluster means ...";
                update_clusters(mat, num_members_v);

#if VERBOSE
                BOOST_LOG_TRIVIAL(info) << "Printing cluster means:";
                print_clusters<prune_cluster>(g_clusters);

                BOOST_LOG_TRIVIAL(info) << "Getting cluster membership ...";
                get_membership(mat)->print(NUM_ROWS);
#endif

                BOOST_LOG_TRIVIAL(info) << "Printing cluster counts ...";
                print_vector<unsigned>(num_members_v);

                BOOST_LOG_TRIVIAL(info) << "** Samples changes cluster: "
                    << g_num_changed << " **\n";

                if (g_num_changed == 0 || ((g_num_changed/(double)NUM_ROWS)) <= tolerance) {
                    converged = true;
                    break;
                } else {
                    g_num_changed = 0;
                }
                g_iter++;

#if KM_TEST
                g_prune_stats->finalize();
#endif
            }

#if KM_TEST
            g_prune_stats->get_stats();
#endif
            gettimeofday(&end, NULL);
            BOOST_LOG_TRIVIAL(info) << "\n\nAlgorithmic time taken = " <<
                time_diff(start, end) << " sec\n";

#ifdef PROFILER
            ProfilerStop();
#endif
            BOOST_LOG_TRIVIAL(info) << "\n******************************************\n";
            printf("Total # of IO requests: %u\nTotal bytes requested: %lu\n\n",
                    g_io_reqs, (g_io_reqs*(sizeof(double))*NUM_COLS));

            if (converged) {
                BOOST_LOG_TRIVIAL(info) <<
                    "K-means converged in " << g_iter << " iterations";
            } else {
                BOOST_LOG_TRIVIAL(warning) << "[Warning]: K-means failed to converge in "
                    << g_iter << " iterations";
            }
            BOOST_LOG_TRIVIAL(info) << "\n******************************************\n";

            print_vector<unsigned>(num_members_v);

            std::vector<std::vector<double>> means;
            get_means(means);
            cluster_assignments = get_membership(mat);
            return sem_kmeans_ret::create(cluster_assignments, means, num_members_v, g_iter);
        }
    }