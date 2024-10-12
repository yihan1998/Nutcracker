#ifndef VXLAN_FWD_VNF_CORE_H_
#define VXLAN_FWD_VNF_CORE_H_

#include <offload_rules.h>

#include "app_vnf.h"

/* Simple FWD VNF application configuration */
struct vxlan_fwd_config {
	struct application_dpdk_config *dpdk_cfg;	/* DPDK configurations */
	uint16_t rx_only;				/* Whether or not to work in "receive mode" only, where the application does not send received packets */
	uint16_t hw_offload;				/* Whether or not HW steering is used */
	uint64_t stats_timer;				/* The time between periodic stats prints */
	bool is_hairpin;				/* Number of hairpin queues */
};

/* Simple FWD VNF parameters to be passed when starting processing packets */
struct vxlan_fwd_process_pkts_params {
	struct vxlan_fwd_config *cfg;	/* Application configuration */
	struct app_vnf *vnf;		/* Holder for all functions pointers used by the appplication */
};

/*
 * Registers all flags used bu the users when running the application, such as "aging-thread" flag.
 * This is needed so that the parsing by DOCA argument parser work as expected.
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t register_vxlan_fwd_params(void);

/*
 * Process received packets, mainly retrieving packet's key, then checking if there is an entry found
 * matching the generated key, in the entries table.
 * If no entry found, the function will create and add new one.
 * In addition, this function handles aging as well
 *
 * @process_pkts_params [in]: an argument containing the mapping  between queues and cores/lcores
 * @return: 0 on success and non-zero value on failure
 *
 * @NOTE: This function is a thread safe
 */
int vxlan_fwd_process_pkts(void *process_pkts_params);

/*
 * Stops the application from proccesing further packets
 */
void vxlan_fwd_process_pkts_stop(void);

/*
 * Maps queues to cores/lcores and vice versa
 *
 * @nb_queues [in]: number of queues to map
 */
void vxlan_fwd_map_queue(uint16_t nb_queues);

/*
 * Destroys all allocated resources used by the application
 *
 * @vnf [in]: application allocated resources such as ports, pipes and entries.
 */
void vxlan_fwd_destroy(struct app_vnf *vnf);

#endif /* VXLAN_FWD_VNF_CORE_H_ */
