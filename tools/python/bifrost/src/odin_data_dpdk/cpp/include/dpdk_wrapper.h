#ifndef DPDK_WRAPPER_H
#define DPDK_WRAPPER_H

#include <rte_eal.h>
#include <rte_ring.h>

#ifdef __cplusplus
extern "C" {
#endif

int wrapper_rte_eal_init(int argc, char **argv);
struct rte_ring* wrapper_rte_ring_lookup(const char* name);
int wrapper_rte_ring_dequeue(struct rte_ring* r, void** obj_p);
int wrapper_rte_ring_enqueue(struct rte_ring* r, void* obj);
int wrapper_rte_eal_primary_proc_alive(const char *config_file_path);

#ifdef __cplusplus
}
#endif

#endif // DPDK_WRAPPER_H