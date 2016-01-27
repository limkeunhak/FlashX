#include <stdio.h>
#include <iostream>

#include <boost/assert.hpp>

#include "libgraph-algs/clusters.h"

constexpr unsigned NCOL = 4;
constexpr unsigned NCLUST = 5;
static const kmsvector zero {1,2,3,4};
static const kmsvector one {4,5,6,7,8};
static const kmsvector two {9,10,11,12};
static const kmsvector three {13,14,15,16};
static const kmsvector four {17,18,19,20};
static const clusters::ptr empty = clusters::create(NCLUST, NCOL);
static const std::vector<kmsvector> data {zero, one, two, three, four};
static const kmsvector kv {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20};

void test_clusters() {
    printf("Testing clusters ...\n");
    printf("Print no init:\n");
    empty->print_means();

    clusters::ptr cls = clusters::create(NCLUST, NCOL, kv);
    printf("Print after init:\n");
    cls->print_means();

    cls->clear();
    printf("After clear test:\n");
    BOOST_VERIFY(*cls == *empty);
    printf("Success ...\n");

    cls->set_mean(kv);
    printf("Print after all set-mean:\n");
    cls->print_means();

    cls->set_mean(zero, 2); cls->set_mean(zero, 4);
    printf("Print after all setting 0 to 2 & 4 :\n");
    cls->print_means();

    printf("Clearing all means & adding all data to all clusters:\n");
    cls->clear();
    BOOST_VERIFY(*cls == *empty);
    printf("Success ...\n");

    for (unsigned cl = 0; cl < NCLUST; cl++)
        for (unsigned i = 0; i < data.size(); i++)
            cls->add_member(&(data[i][0]), cl);
    printf("Before finalize all should be equal:\n");
    clusters::ptr old = clusters::create(NCLUST, NCOL);
    *old = *cls;

    cls->print_means();

    for (unsigned cl = 0; cl < NCLUST; cl++)
        cls->finalize(cl);
    printf("After finalize all should be equal as well:\n");
    cls->print_means();

    printf("Test unfinalize should return to original:\n");
    for (unsigned cl = 0; cl < NCLUST; cl++)
        cls->unfinalize(cl);
    BOOST_VERIFY(*old == *cls);
    printf("Success ...\n");

    printf("Removing members:\n");
    for (unsigned cl = 0; cl < NCLUST; cl++)
        for (unsigned i = 0; i < data.size(); i++)
            cls->remove_member(&(data[i][0]), cl);

    cls->print_means();
    BOOST_VERIFY(*cls == *empty);
    printf("Success ...\n");

    *cls += *empty;
    BOOST_VERIFY(*cls == *empty);
    printf("Success ...\n");
}

void test_prune_clusters() {
    printf("Testing prune_clusters ...\n");
    prune_clusters::ptr pcl = prune_clusters::create(NCLUST, NCOL);
    pcl->set_mean(kv);

    clusters::ptr cl = clusters::create(NCLUST, NCOL, kv);
    printf("Test *cl == *pcl after cast...\n");
    BOOST_VERIFY(*cl == *(std::static_pointer_cast<clusters, prune_clusters>(pcl)));
    pcl->clear();

    printf("Setting test cl to values == pcl ...\n");
    for (unsigned i = 0; i < NCLUST; i++) {
        pcl->add_member(&(data[i][0]), i);
        pcl->finalize(i);
        pcl->set_prev_mean(i);
    }

    printf("Printing prev_mean:\n");
    pcl->print_prev_means_v();

    printf("Testing prev_mean ..\n");

    for (unsigned i = 0; i < NCLUST; i++) {
        kmsiterator it = pcl->get_prev_mean(i);
        for (unsigned col = 0; col < NCOL; col++) {
            BOOST_VERIFY(*(it++) == data[i][col]);
        }
    }
    printf("Success ...\n");
}

int main() {
    test_clusters();
    test_prune_clusters();
    return EXIT_SUCCESS;
}