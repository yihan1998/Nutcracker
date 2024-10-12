/*
 * Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES, ALL RIGHTS RESERVED.
 *
 * This software product is a proprietary product of NVIDIA CORPORATION &
 * AFFILIATES (the "Company") and all right, title, and interest in and to the
 * software product, including all associated intellectual property rights, are
 * and shall remain exclusively with the Company.
 *
 * This software product is governed by the End User License Agreement
 * provided with the software product.
 *
 */

#ifndef VXLAN_FWD_H_
#define VXLAN_FWD_H_

#include <stdint.h>
#include <stdbool.h>

#include <doca_flow.h>

#include "vxlan_fwd_pkt.h"
#include "vxlan_fwd_port.h"

#define VXLAN_FWD_PORTS 	(2)		/* Number of ports used by the application */
#define VXLAN_FWD_MAX_FLOWS	(8096)	/* Maximum number of flows used/added by the application at a given time */

/* Application resources, such as flow table, pipes and hairpin peers */
struct vxlan_fwd_app {
	struct vxlan_fwd_ft *ft;					/* Flow table, used for stprng flows */
	uint16_t hairpin_peer[VXLAN_FWD_PORTS];			/* Binded pair ports array*/
	struct doca_flow_port *ports[VXLAN_FWD_PORTS];			/* DOCA Flow ports array used by the application */
	struct doca_flow_pipe *pipe_vxlan[VXLAN_FWD_PORTS];		/* VXLAN pipe of each port */
	struct doca_flow_pipe *pipe_control[VXLAN_FWD_PORTS];		/* control pipe of each port */
	struct doca_flow_pipe *pipe_hairpin[VXLAN_FWD_PORTS];		/* hairpin pipe for non-VxLAN/GRE/GTP traffic */
	struct doca_flow_pipe *pipe_rss[VXLAN_FWD_PORTS];		/* RSS pipe, matches every packet and forwards to SW */
	struct doca_flow_pipe *pipe_monitor[VXLAN_FWD_PORTS];	/* vxlan encap pipe on the egress domain */
	int16_t nb_queues;						/* flow age query item buffer */
	struct doca_flow_aged_query *query_array[0];			/* buffer for flow aged query items */
};

/* VXLAN FWD flow entry representation */
struct vxlan_fwd_pipe_entry {
	bool is_hw;				/* Wether the entry in HW or not */
	uint64_t total_pkts;			/* Total number of packets matched the flow */
	uint64_t total_bytes;			/* Total number of bytes matched the flow */
	uint16_t pipe_queue;			/* Pipe queue of the flow entry */
	struct doca_flow_pipe_entry *hw_entry;	/* a pointer for the flow entry in hw */
};

extern struct doca_flow_pipe_entry * monitor_entry;

/*
 * fills struct app_vnf with init/destroy, process and other needed pointer functions.
 *
 * @return: a pointer to struct app_vnf which contains all needed pointer functions
 */
struct app_vnf *vxlan_fwd_get_vnf(void);

#endif /* VXLAN_FWD_H_ */
