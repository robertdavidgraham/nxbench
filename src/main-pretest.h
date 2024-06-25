/*
    Tests to make sure all the source IP addresses can reach
    all the destination IP addresses.
*/
#ifndef MAIN_PRETEST_H
#define MAIN_PRETEST_H
struct main_conf_t;

/**
 * Returns 0 on success, any other value if the test
 * fails.
 */
int pretest_connections(const struct main_conf_t *conf);


#endif
