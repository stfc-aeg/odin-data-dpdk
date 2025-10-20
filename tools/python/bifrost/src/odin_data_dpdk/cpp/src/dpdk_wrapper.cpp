#include "dpdk_wrapper.h"

int wrapper_rte_eal_init(int argc, char **argv)
{
    return rte_eal_init(argc, argv);
}

struct rte_ring* wrapper_rte_ring_lookup(const char* name)
{
    return rte_ring_lookup(name);
}

int wrapper_rte_ring_dequeue(struct rte_ring* r, void** obj_p)
{
    return rte_ring_dequeue(r, obj_p);
}

int wrapper_rte_ring_enqueue(struct rte_ring* r, void* obj)
{
    return rte_ring_enqueue(r, obj);
}

int wrapper_rte_eal_primary_proc_alive(const char *config_file_path)
{
    return rte_eal_primary_proc_alive(config_file_path);
}