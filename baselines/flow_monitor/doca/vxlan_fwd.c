#include <stdint.h>
#include <signal.h>

#include <rte_cycles.h>
#include <rte_launch.h>
#include <rte_ethdev.h>

#include <doca_argp.h>
#include <doca_log.h>

#include <dpdk_utils.h>
#include <utils.h>

#include "app_vnf.h"
#include "vxlan_fwd.h"
#include "vxlan_fwd_ft.h"

DOCA_LOG_REGISTER(VXLAN_FWD);

#define MAX_PORT_STR (128)	/* Maximum length of the string name of the port */

/* Convert IPv4 address to big endian */
#define BE_IPV4_ADDR(a, b, c, d) \
	(RTE_BE32(((uint32_t)a<<24) + (b<<16) + (c<<8) + d))

/* Set the MAC address with respect to the given 6 bytes */
#define SET_MAC_ADDR(addr, a, b, c, d, e, f)\
do {\
	addr[0] = a & 0xff;\
	addr[1] = b & 0xff;\
	addr[2] = c & 0xff;\
	addr[3] = d & 0xff;\
	addr[4] = e & 0xff;\
	addr[5] = f & 0xff;\
} while (0)

/* Set match l4 port */
#define SET_L4_PORT(layer, port, value) \
do {\
	if (match->layer.l4_type_ext == DOCA_FLOW_L4_TYPE_EXT_TCP)\
		match->layer.tcp.l4_port.port = (value);\
	else if (match->layer.l4_type_ext == DOCA_FLOW_L4_TYPE_EXT_UDP)\
		match->layer.udp.l4_port.port = (value);\
} while (0)

/*
 * Currently we only can get the ft_entry ctx, but for the aging,
 * we need get the ft_entry pointer, add destroy the ft entry.
 */
#define GET_FT_ENTRY(ctx) \
	container_of(ctx, struct vxlan_fwd_ft_entry, user_ctx)

#define BUILD_VNI(uint24_vni) (RTE_BE32((uint32_t)uint24_vni << 8))	/* Converting VNI to big endian */
#define AGE_QUERY_BURST 128						/* Aging query burst */
#define PULL_TIME_OUT 10000						/* Maximum timeout for pulling */
#define MAX_TRY 10							/* Maximum tries for checking entry status in HW */
#define NB_ACTION_ARRAY (1)						/* Used as the size of muti-actions array for DOCA Flow API */

static struct vxlan_fwd_app *vxlan_fwd_ins;			/* Instance holding all allocated resources needed for a proper run */

/* user context struct that will be used in entries process callback */
struct entries_status {
	bool failure;		/* will be set to true if some entry status will not be success */
	int nb_processed;	/* will hold the number of entries that was already processed */
	void *ft_entry;		/* pointer to struct vxlan_fwd_ft_entry */
};

/*
 * Destroy flow table used by the application
 *
 * @return: 0 on success and negative value otherwise
 */
static int
vxlan_fwd_destroy_ins(void)
{
	uint16_t idx;

	if (vxlan_fwd_ins == NULL)
		return 0;

	vxlan_fwd_ft_destroy(vxlan_fwd_ins->ft);

	for (idx = 0; idx < VXLAN_FWD_PORTS; idx++) {
		if (vxlan_fwd_ins->ports[idx])
			doca_flow_port_stop(vxlan_fwd_ins->ports[idx]);
	}
	free(vxlan_fwd_ins);
	vxlan_fwd_ins = NULL;
	return 0;
}

/*
 * Destroy application allocated resources
 *
 * @return: 0 on success and negative value otherwise
 */
static int
vxlan_fwd_destroy(void)
{
	vxlan_fwd_destroy_ins();
	doca_flow_destroy();
	return 0;
}

/*
 * Callback funtion for removing aged flow
 *
 * @ctx [in]: the context of the aged flow to remove
 */
static void
vxlan_fwd_aged_flow_cb(struct vxlan_fwd_ft_user_ctx *ctx)
{
	struct vxlan_fwd_pipe_entry *entry =
		(struct vxlan_fwd_pipe_entry *)&ctx->data[0];

	if (entry->is_hw) {
		doca_flow_pipe_rm_entry(entry->pipe_queue, DOCA_FLOW_NO_WAIT, entry->hw_entry);
		entry->hw_entry = NULL;
	}
}

/*
 * Initializes flow tables used by the application for a given port
 *
 * @port_cfg [in]: the port configuration to allocate the resources
 * @return: 0 on success and negative value otherwise
 */
static int
vxlan_fwd_create_ins(struct vxlan_fwd_port_cfg *port_cfg)
{
	uint16_t index;

	vxlan_fwd_ins = (struct vxlan_fwd_app *) calloc(1, sizeof(struct vxlan_fwd_app) +
	sizeof(struct doca_flow_aged_query *) * port_cfg->nb_queues);
	if (vxlan_fwd_ins == NULL) {
		DOCA_LOG_ERR("Failed to allocate SF");
		goto fail_init;
	}

	vxlan_fwd_ins->ft = vxlan_fwd_ft_create(VXLAN_FWD_MAX_FLOWS,
					sizeof(struct vxlan_fwd_pipe_entry),
					&vxlan_fwd_aged_flow_cb, NULL);
	if (vxlan_fwd_ins->ft == NULL) {
		DOCA_LOG_ERR("Failed to allocate FT");
		goto fail_init;
	}
	vxlan_fwd_ins->nb_queues = port_cfg->nb_queues;
	for (index = 0 ; index < VXLAN_FWD_PORTS; index++)
		vxlan_fwd_ins->hairpin_peer[index] = index ^ 1;
	return 0;
fail_init:
	vxlan_fwd_destroy_ins();
	return -1;
}

/*
 * Create DOCA Flow "RSS pipe" and adds entry that match every packet, and forwards to SW, for a given port
 *
 * @port_id [in]: port identifier
 * @return: 0 on success, negative value otherwise and error is set.
 */
static int
vxlan_fwd_build_rss_flow(uint16_t port_id)
{
	struct doca_flow_match match;
	struct doca_flow_actions actions, *actions_arr[NB_ACTION_ARRAY];
	struct doca_flow_fwd fwd;
	struct doca_flow_pipe_cfg pipe_cfg;
	struct doca_flow_pipe_entry *entry;
	struct vxlan_fwd_port_cfg *port_cfg = ((struct vxlan_fwd_port_cfg *)doca_flow_port_priv_data(vxlan_fwd_ins->ports[port_id]));
	struct entries_status *status;
	uint16_t rss_queues[port_cfg->nb_queues];
	int num_of_entries = 1;
	int i;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&actions, 0, sizeof(actions));
	memset(&fwd, 0, sizeof(fwd));
	memset(&pipe_cfg, 0, sizeof(pipe_cfg));

	status = (struct entries_status *)calloc(1, sizeof(struct entries_status));

	pipe_cfg.attr.name = "RSS_PIPE";
	pipe_cfg.match = &match;
	actions_arr[0] = &actions;
	pipe_cfg.actions = actions_arr;
	pipe_cfg.attr.is_root = false;
	pipe_cfg.attr.nb_actions = NB_ACTION_ARRAY;
	pipe_cfg.port = vxlan_fwd_ins->ports[port_cfg->port_id];

	for (i = 0; i < port_cfg->nb_queues; i++)
		rss_queues[i] = i;

	fwd.type = DOCA_FLOW_FWD_RSS;
	fwd.rss_outer_flags = DOCA_FLOW_RSS_IPV4 | DOCA_FLOW_RSS_UDP;
	fwd.num_of_queues = port_cfg->nb_queues;
	fwd.rss_queues = rss_queues;

	result = doca_flow_pipe_create(&pipe_cfg, &fwd, NULL, &vxlan_fwd_ins->pipe_rss[port_cfg->port_id]);
	if (result != DOCA_SUCCESS) {
		free(status);
		return -1;
	}

	result = doca_flow_pipe_add_entry(0, vxlan_fwd_ins->pipe_rss[port_cfg->port_id], &match, &actions, NULL, &fwd, 0, status, &entry);
	if (result != DOCA_SUCCESS) {
		free(status);
		return -1;
	}
	result = doca_flow_entries_process(vxlan_fwd_ins->ports[port_cfg->port_id], 0, PULL_TIME_OUT, num_of_entries);
	if (result != DOCA_SUCCESS)
		return -1;

	if (status->nb_processed != num_of_entries || status->failure)
		return -1;

	return 0;
}

/*
 * Create DOCA Flow hairpin pipe and adds entry that match every packet for a given port
 *
 * @port_id [in]: port identifier
 * @return: 0 on success, negative value otherwise and error is set.
 */
static int
vxlan_fwd_build_hairpin_flow(uint16_t port_id)
{
	struct doca_flow_match match;
	struct doca_flow_actions actions, *actions_arr[NB_ACTION_ARRAY];
	struct doca_flow_fwd fwd;
	struct doca_flow_pipe_cfg pipe_cfg;
	struct doca_flow_pipe_entry *entry;
	struct vxlan_fwd_port_cfg *port_cfg = ((struct vxlan_fwd_port_cfg *)doca_flow_port_priv_data(vxlan_fwd_ins->ports[port_id]));
	struct entries_status *status;
	int num_of_entries = 1;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&actions, 0, sizeof(actions));
	memset(&fwd, 0, sizeof(fwd));
	memset(&pipe_cfg, 0, sizeof(pipe_cfg));

	status = (struct entries_status *)calloc(1, sizeof(struct entries_status));

	pipe_cfg.attr.name = "HAIRPIN_PIPE";
	pipe_cfg.match = &match;
	actions_arr[0] = &actions;
	pipe_cfg.actions = actions_arr;
	pipe_cfg.attr.is_root = false;
	pipe_cfg.attr.nb_actions = NB_ACTION_ARRAY;
	pipe_cfg.port = vxlan_fwd_ins->ports[port_cfg->port_id];

	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = port_cfg->port_id ^ 1;

	result = doca_flow_pipe_create(&pipe_cfg, &fwd, NULL, &vxlan_fwd_ins->pipe_hairpin[port_cfg->port_id]);
	if (result != DOCA_SUCCESS) {
		free(status);
		return -1;
	}

	result = doca_flow_pipe_add_entry(0, vxlan_fwd_ins->pipe_hairpin[port_cfg->port_id], &match, &actions, NULL, &fwd, 0, status, &entry);
	if (result != DOCA_SUCCESS) {
		free(status);
		return -1;
	}

	result = doca_flow_entries_process(vxlan_fwd_ins->ports[port_cfg->port_id], 0, PULL_TIME_OUT, num_of_entries);
	if (result != DOCA_SUCCESS)
		return -1;

	if (status->nb_processed != num_of_entries || status->failure)
		return -1;

	return 0;
}

/*
 * Build DOCA Flow FWD component based on the port configuration provided by the user
 *
 * @port_cfg [in]: port configuration as provided by the user
 * @fwd [out]: DOCA Flow FWD component to fill its fields
 */
static void
vxlan_fwd_build_fwd(struct vxlan_fwd_port_cfg *port_cfg, struct doca_flow_fwd *fwd)
{
	if (port_cfg->is_hairpin > 0) {
		fwd->type = DOCA_FLOW_FWD_PORT;
		fwd->port_id = port_cfg->port_id ^ 1;
	}

	else {
		fwd->type = DOCA_FLOW_FWD_PIPE;
		fwd->next_pipe = vxlan_fwd_ins->pipe_rss[port_cfg->port_id];
	}

}

static int
vxlan_fwd_create_monitor_pipe(struct vxlan_fwd_port_cfg *port_cfg)
{
	struct doca_flow_match match;
	struct doca_flow_actions actions, *actions_arr[NB_ACTION_ARRAY];
	struct doca_flow_monitor monitor;
	struct doca_flow_fwd fwd;
	struct doca_flow_fwd fwd_miss;
	struct doca_flow_pipe_cfg pipe_cfg;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&actions, 0, sizeof(actions));
	memset(&monitor, 0, sizeof(monitor));
	memset(&fwd, 0, sizeof(fwd));
	memset(&fwd_miss, 0, sizeof(fwd_miss));
	memset(&pipe_cfg, 0, sizeof(pipe_cfg));

	pipe_cfg.attr.type = DOCA_FLOW_PIPE_BASIC;
	pipe_cfg.match = &match;
	actions_arr[0] = &actions;
	pipe_cfg.actions = actions_arr;
	pipe_cfg.attr.is_root = true;
	pipe_cfg.attr.nb_actions = NB_ACTION_ARRAY;
	pipe_cfg.port = vxlan_fwd_ins->ports[port_cfg->port_id];
	pipe_cfg.monitor = &monitor;

	SET_MAC_ADDR(match.outer.eth.dst_mac, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff);

	/* build monitor part */
	monitor.flags = DOCA_FLOW_MONITOR_COUNT;

	vxlan_fwd_build_fwd(port_cfg, &fwd);

	fwd_miss.type = DOCA_FLOW_FWD_PIPE;
	fwd_miss.next_pipe = vxlan_fwd_ins->pipe_rss[port_cfg->port_id];

	if (doca_flow_pipe_create(&pipe_cfg, &fwd, &fwd_miss, &vxlan_fwd_ins->pipe_monitor[port_cfg->port_id]) != DOCA_SUCCESS) {
		printf("Failed to create monitor pipe!\n");
		return -1;
	}

	for (int i = 0; i < 190; i++) {
		struct doca_flow_monitor monitor;
		memset(&monitor, 0, sizeof(monitor));
		// monitor.flags = DOCA_FLOW_MONITOR_COUNT;
		SET_MAC_ADDR(match.outer.eth.dst_mac, 0xb8, 0xce, 0xf6, 0xa8, 0x82, i);
		result = doca_flow_pipe_add_entry(0, vxlan_fwd_ins->pipe_monitor[port_cfg->port_id], &match, &actions, &monitor, NULL, DOCA_FLOW_NO_WAIT, NULL, NULL);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed adding entry to pipe");
			return 0;
		}
	}

	result = doca_flow_entries_process(vxlan_fwd_ins->ports[port_cfg->port_id], 0, PULL_TIME_OUT, 190);

	return 0;
}

/*
 * Stop DOCA Flow ports
 *
 * @nb_ports [in]: number of ports to stop
 * @ports [in]: array of DOCA flow ports to stop
 */
static void
vxlan_fwd_stop_doca_flow_ports(int nb_ports, struct doca_flow_port *ports[])
{
	int portid;

	for (portid = 0; portid < nb_ports; portid++) {
		if (ports[portid] != NULL)
			doca_flow_port_stop(ports[portid]);
	}
}

/*
 * Create DOCA Flow port by port id
 *
 * @port_id [in]: port ID
 * @return: port handler on success, NULL otherwise and error is set.
 */
static struct doca_flow_port *
vxlan_fwd_create_doca_flow_port(int port_id)
{
	int max_port_str_len = 128;
	struct doca_flow_port_cfg port_cfg;
	char port_id_str[max_port_str_len];
	struct doca_flow_port *port;

	memset(&port_cfg, 0, sizeof(port_cfg));

	port_cfg.port_id = port_id;
	port_cfg.type = DOCA_FLOW_PORT_DPDK_BY_ID;
	snprintf(port_id_str, max_port_str_len, "%d", port_cfg.port_id);
	port_cfg.devargs = port_id_str;
	port_cfg.priv_data_size = sizeof(struct vxlan_fwd_port_cfg);

	if (doca_flow_port_start(&port_cfg, &port) != DOCA_SUCCESS)
		return NULL;

	return port;
}

/*
 * Transfer packet l4 type to doca l4 type
 *
 * @pkt_l4_type [in]: the l4 type of eth packet
 * @return: the doca type of the packet l4 type
 */
static enum doca_flow_l4_type_ext
vxlan_fwd_l3_type_transfer(uint8_t pkt_l4_type)
{
	switch (pkt_l4_type) {
	case DOCA_PROTO_TCP:
		return DOCA_FLOW_L4_TYPE_EXT_TCP;
	case DOCA_PROTO_UDP:
		return DOCA_FLOW_L4_TYPE_EXT_UDP;
	case DOCA_PROTO_GRE:
		/* set gre type in doca_flow_tun type */
		return DOCA_FLOW_L4_TYPE_EXT_NONE;
	default:
		DOCA_LOG_WARN("The L4 type %u is not supported", pkt_l4_type);
		return DOCA_FLOW_L4_TYPE_EXT_NONE;
	}
}

/*
 * Setting tunneling type in the match component
 *
 * @pinfo [in]: the packet info as represented in the application
 * @match [out]: match component to set the tunneling type in based on the packet info provided
 */
static inline void
vxlan_fwd_match_set_tun(struct vxlan_fwd_pkt_info *pinfo,
			 struct doca_flow_match *match)
{
	if (!pinfo->tun_type)
		return;
	match->tun.type = pinfo->tun_type;
	switch (match->tun.type) {
	case DOCA_FLOW_TUN_VXLAN:
		match->tun.vxlan_tun_id = pinfo->tun.vni;
		break;
	default:
		DOCA_LOG_WARN("Unsupported tunnel type:%u", match->tun.type);
		break;
	}
}

/*
 * Initialize DOCA Flow ports
 *
 * @nb_ports [in]: number of ports to create
 * @ports [in]: array of ports to create
 * @is_hairpin [in]: port pair should run if is_hairpin = true
 * @return: 0 on success and negative value otherwise.
 */
static int
vxlan_fwd_init_doca_flow_ports(int nb_ports, struct doca_flow_port *ports[], bool is_hairpin)
{
	int portid;

	for (portid = 0; portid < nb_ports; portid++) {
		/* Create doca flow port */
		ports[portid] = vxlan_fwd_create_doca_flow_port(portid);
		if (ports[portid] == NULL) {
			vxlan_fwd_stop_doca_flow_ports(portid + 1, ports);
			return -1;
		}

		/* Pair ports should be done in the following order: port0 with port1, port2 with port3 etc */
		if (!is_hairpin || !portid || !(portid % 2))
			continue;

		/* pair odd port with previous port */
		if (doca_flow_port_pair(ports[portid], ports[portid ^ 1]) != DOCA_SUCCESS) {
			vxlan_fwd_stop_doca_flow_ports(portid + 1, ports);
			return -1;
		}
	}
	return 0;
}

/*
 * Build match component
 *
 * @pinfo [in]: the packet info as represented in the application
 * @match [out]: the match component to build
 */
static void
vxlan_fwd_build_entry_match(struct vxlan_fwd_pkt_info *pinfo,
			     struct doca_flow_match *match)
{
	memset(match, 0x0, sizeof(*match));
	/* set match all fields, pipe will select which field to match */
	memcpy(match->outer.eth.dst_mac, vxlan_fwd_pinfo_outer_mac_dst(pinfo),
		DOCA_ETHER_ADDR_LEN);
	memcpy(match->outer.eth.src_mac, vxlan_fwd_pinfo_outer_mac_src(pinfo),
		DOCA_ETHER_ADDR_LEN);
	match->outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	match->outer.ip4.dst_ip = vxlan_fwd_pinfo_outer_ipv4_dst(pinfo);
	match->outer.ip4.src_ip = vxlan_fwd_pinfo_outer_ipv4_src(pinfo);
	match->outer.l4_type_ext = vxlan_fwd_l3_type_transfer(pinfo->outer.l4_type);
	SET_L4_PORT(outer, src_port, vxlan_fwd_pinfo_outer_src_port(pinfo));
	SET_L4_PORT(outer, dst_port, vxlan_fwd_pinfo_outer_dst_port(pinfo));
	if (!pinfo->tun_type)
		return;
	vxlan_fwd_match_set_tun(pinfo, match);
	match->inner.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	match->inner.ip4.dst_ip = vxlan_fwd_pinfo_inner_ipv4_dst(pinfo);
	match->inner.ip4.src_ip = vxlan_fwd_pinfo_inner_ipv4_src(pinfo);
	match->inner.l4_type_ext = vxlan_fwd_l3_type_transfer(pinfo->inner.l4_type);
	SET_L4_PORT(inner, src_port, vxlan_fwd_pinfo_inner_src_port(pinfo));
	SET_L4_PORT(inner, dst_port, vxlan_fwd_pinfo_inner_dst_port(pinfo));
}

/*
 * Build monitor component
 *
 * @pinfo [in]: the packet info as represented in the application
 * @monitor [out]: the monitor component to build
 */
static void
vxlan_fwd_build_entry_monitor(struct vxlan_fwd_pkt_info *pinfo,
			       struct doca_flow_monitor *monitor)
{
	(void)pinfo;

	monitor->flags = DOCA_FLOW_MONITOR_COUNT;
	monitor->flags |= DOCA_FLOW_MONITOR_AGING;
	/* flows will be aged out in 5 - 60s */
	monitor->aging_sec = (uint32_t)rte_rand() % 55 + 5;
}

/*
 * Selects the pipe based on the tunneling type
 *
 * @pinfo [in]: the packet info as represented in the application
 * @return: a pointer for the selected pipe on success and NULL otherwise
 */
static struct doca_flow_pipe*
vxlan_fwd_select_pipe(struct vxlan_fwd_pkt_info *pinfo)
{
	if (pinfo->tun_type == DOCA_FLOW_TUN_VXLAN)
		return vxlan_fwd_ins->pipe_vxlan[pinfo->orig_port_id];
	return NULL;
}

/*
 * Adds new entry, with respect to the packet info, to the flow table
 *
 * @pinfo [in]: the packet info as represented in the application
 * @user_ctx [in]: user context
 * @age_sec [out]: Aging time for the created entry in seconds
 * @return: created entry pointer on success and NULL otherwise
 */
static struct doca_flow_pipe_entry*
vxlan_fwd_pipe_add_entry(struct vxlan_fwd_pkt_info *pinfo,
			  void *user_ctx, uint32_t *age_sec)
{
	struct doca_flow_match match;
	struct doca_flow_monitor monitor = {0};
	struct doca_flow_actions actions = {0};
	struct doca_flow_pipe *pipe;
	struct doca_flow_pipe_entry *entry;
	struct entries_status *status;
	int num_of_entries = 1;
	doca_error_t result;

	status = (struct entries_status *)calloc(1, sizeof(struct entries_status));

	memset(&match, 0, sizeof(match));
	memset(&actions, 0, sizeof(actions));

	pipe = vxlan_fwd_select_pipe(pinfo);
	if (pipe == NULL) {
		DOCA_LOG_WARN("Failed to select pipe on this packet");
		free(status);
		return NULL;
	}

	actions.meta.pkt_meta = 1;
	actions.action_idx = 0;

	status->ft_entry = user_ctx;

	vxlan_fwd_build_entry_match(pinfo, &match);
	vxlan_fwd_build_entry_monitor(pinfo, &monitor);
	result = doca_flow_pipe_add_entry(pinfo->pipe_queue,
		pipe, &match, &actions, &monitor, NULL, DOCA_FLOW_NO_WAIT, status, &entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed adding entry to pipe");
		free(status);
		return NULL;
	}

	result = doca_flow_entries_process(vxlan_fwd_ins->ports[pinfo->orig_port_id],
				pinfo->pipe_queue, PULL_TIME_OUT, num_of_entries);

	if (result != DOCA_SUCCESS)
		goto error;

	if (status->nb_processed != num_of_entries || status->failure)
		goto error;

	*age_sec = monitor.aging_sec;
	return entry;

error:
	doca_flow_pipe_rm_entry(pinfo->pipe_queue, DOCA_FLOW_NO_WAIT, entry);
	return NULL;
}

/*
 * Checks whether or not the received packet info is new.
 *
 * @pinfo [in]: the packet info as represented in the application
 * @return: true on success and false otherwise
 */
static bool
vxlan_fwd_need_new_ft(struct vxlan_fwd_pkt_info *pinfo)
{
	if (pinfo->outer.l3_type != IPV4) {
		DOCA_LOG_WARN("The outer L3 type %u is not supported",
			pinfo->outer.l3_type);
		return false;
	}
	if ((pinfo->outer.l4_type != DOCA_PROTO_TCP) &&
		(pinfo->outer.l4_type != DOCA_PROTO_UDP) &&
		(pinfo->outer.l4_type != DOCA_PROTO_GRE)) {
		DOCA_LOG_WARN("The outer L4 type %u is not supported",
			pinfo->outer.l4_type);
		return false;
	}
	return true;
}

/*
 * Entry processing callback
 *
 * @entry [in]: DOCA Flow entry pointer
 * @pipe_queue [in]: queue identifier
 * @status [in]: DOCA Flow entry status
 * @op [in]: DOCA Flow entry operation
 * @user_ctx [out]: user context
 */
static void
vxlan_fwd_check_for_valid_entry(struct doca_flow_pipe_entry *entry, uint16_t pipe_queue,
		      enum doca_flow_entry_status status, enum doca_flow_entry_op op, void *user_ctx)
{
	(void)entry;
	(void)op;
	(void)pipe_queue;

	struct vxlan_fwd_ft_entry *ft_entry;
	struct entries_status *entry_status = (struct entries_status *)user_ctx;

	if (entry_status == NULL)
		return;
	if (status != DOCA_FLOW_ENTRY_STATUS_SUCCESS)
		entry_status->failure = true; /* set failure to true if processing failed */
	if (op == DOCA_FLOW_ENTRY_OP_AGED) {
		ft_entry = GET_FT_ENTRY((void *)(entry_status->ft_entry));
		vxlan_fwd_ft_destroy_entry(vxlan_fwd_ins->ft, ft_entry);
	} else if (op == DOCA_FLOW_ENTRY_OP_ADD)
		entry_status->nb_processed++;
	else if (op == DOCA_FLOW_ENTRY_OP_DEL)
		free(entry_status);
}

/*
 * Initialize DOCA Flow library
 *
 * @nb_queues [in]: number of queues the sample will use
 * @mode [in]: doca flow architecture mode
 * @resource [in]: number of meters and counters to configure
 * @nr_shared_resources [in]: total shared resource per type
 * @return: 0 on success, negative errno value otherwise and error is set.
 */
static int
vxlan_fwd_init_doca_flow(int nb_queues, const char *mode, struct doca_flow_resources resource, uint32_t nr_shared_resources[])
{
	struct doca_flow_cfg flow_cfg;
	int shared_resource_idx;

	memset(&flow_cfg, 0, sizeof(flow_cfg));

	flow_cfg.queues = nb_queues;
	flow_cfg.mode_args = mode;
	flow_cfg.resource = resource;
	flow_cfg.cb = vxlan_fwd_check_for_valid_entry;
	for (shared_resource_idx = 0; shared_resource_idx < DOCA_FLOW_SHARED_RESOURCE_MAX; shared_resource_idx++)
		flow_cfg.nr_shared_resources[shared_resource_idx] = nr_shared_resources[shared_resource_idx];
	if (doca_flow_init(&flow_cfg) != DOCA_SUCCESS)
		return -1;

	return 0;
}

/*
 * Initialize simple FWD application DOCA Flow ports and pipes
 *
 * @port_cfg [in]: a pointer to the port configuration to initialize
 * @return: 0 on success and negative value otherwise
 */
static int
vxlan_fwd_init_ports_and_pipes(struct vxlan_fwd_port_cfg *port_cfg)
{
	int nb_ports = VXLAN_FWD_PORTS;
	struct vxlan_fwd_port_cfg *curr_port_cfg;
	struct doca_flow_resources resource = {0};
	uint32_t nr_shared_resources[DOCA_FLOW_SHARED_RESOURCE_MAX] = {0};
	int port_id;
	int result;

	resource.nb_counters = port_cfg->nb_counters;

	if (vxlan_fwd_init_doca_flow(port_cfg->nb_queues, "vnf,hws", resource, nr_shared_resources) < 0) {
		DOCA_LOG_ERR("Failed to init DOCA Flow");
		vxlan_fwd_destroy_ins();
		return -1;
	}

	if (vxlan_fwd_init_doca_flow_ports(nb_ports, vxlan_fwd_ins->ports, true) < 0) {
		DOCA_LOG_ERR("Failed to init DOCA ports");
		return -1;
	}

	printf("\tCreate pipes and entries...\n");

	for (port_id = 0; port_id < nb_ports; port_id++) {
		curr_port_cfg = ((struct vxlan_fwd_port_cfg *)doca_flow_port_priv_data(vxlan_fwd_ins->ports[port_id]));
		curr_port_cfg->port_id = port_id;
		curr_port_cfg->nb_queues = port_cfg->nb_queues;
		curr_port_cfg->is_hairpin = port_cfg->is_hairpin;
		curr_port_cfg->nb_meters = port_cfg->nb_meters;
		curr_port_cfg->nb_counters = port_cfg->nb_counters;

		DOCA_LOG_INFO("Build hairpin flow...");
		result = vxlan_fwd_build_hairpin_flow(curr_port_cfg->port_id);
		if (result < 0) {
			DOCA_LOG_ERR("Failed building hairpin flow");
			return -1;
		}

		DOCA_LOG_INFO("Build RSS flow...");
		result = vxlan_fwd_build_rss_flow(curr_port_cfg->port_id);
		if (result < 0) {
			DOCA_LOG_ERR("Failed building RSS flow");
			return -1;
		}

		DOCA_LOG_INFO("Create monitor pipe...");
		result = vxlan_fwd_create_monitor_pipe(curr_port_cfg);
		if (result < 0) {
			DOCA_LOG_ERR("Failed building VXLAN pipe");
			return -1;
		}
	}

	return 0;
}

/*
 * Initialize simple FWD application resources
 *
 * @p [in]: a pointer to the port configuration
 * @return: 0 on success and negative value otherwise
 */
static int
vxlan_fwd_init(void *p)
{
	struct vxlan_fwd_port_cfg *port_cfg;
	int ret = 0;

	port_cfg = (struct vxlan_fwd_port_cfg *)p;
	ret = vxlan_fwd_create_ins(port_cfg);
	if (ret)
		return ret;
	return vxlan_fwd_init_ports_and_pipes(port_cfg);
}

/*
 * Adds new flow, with respect to the packet info, to the flow table
 *
 * @pinfo [in]: the packet info as represented in the application
 * @ctx [in]: user context
 * @return: 0 on success and negative value otherwise
 */
static int
vxlan_fwd_handle_new_flow(struct vxlan_fwd_pkt_info *pinfo,
			   struct vxlan_fwd_ft_user_ctx **ctx)
{
	doca_error_t result;
	struct vxlan_fwd_pipe_entry *entry = NULL;
	struct vxlan_fwd_ft_entry *ft_entry;
	uint32_t age_sec;

	result = vxlan_fwd_ft_add_new(vxlan_fwd_ins->ft, pinfo, ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_DBG("Failed create new entry");
		return -1;
	}
	ft_entry = GET_FT_ENTRY(*ctx);
	entry = (struct vxlan_fwd_pipe_entry *)&(*ctx)->data[0];
	entry->pipe_queue = pinfo->pipe_queue;
	entry->hw_entry = vxlan_fwd_pipe_add_entry(pinfo, (void *)(*ctx), &age_sec);
	if (entry->hw_entry == NULL) {
		vxlan_fwd_ft_destroy_entry(vxlan_fwd_ins->ft, ft_entry);
		return -1;
	}
	vxlan_fwd_ft_update_age_sec(ft_entry, age_sec);
	vxlan_fwd_ft_update_expiration(ft_entry);
	entry->is_hw = true;

	return 0;
}

/*
 * Adjust the mbuf pointer, to point on the packet's raw data
 *
 * @pinfo [in]: packet info representation  in the application
 * @return: 0 on success and negative value otherwise
 */
static int
vxlan_fwd_handle_packet(struct vxlan_fwd_pkt_info *pinfo)
{
	struct vxlan_fwd_ft_user_ctx *ctx = NULL;
	struct vxlan_fwd_pipe_entry *entry = NULL;

	if (!vxlan_fwd_need_new_ft(pinfo))
		return -1;
	if (vxlan_fwd_ft_find(vxlan_fwd_ins->ft, pinfo, &ctx) != DOCA_SUCCESS) {
		if (vxlan_fwd_handle_new_flow(pinfo, &ctx))
			return -1;
	}
	entry = (struct vxlan_fwd_pipe_entry *)&ctx->data[0];
	entry->total_pkts++;

	return 0;
}

/*
 * Handles aged flows
 *
 * @port_id [in]: port identifier of the port to handle its aged flows
 * @queue [in]: queue index of the queue to handle its aged flows
 */
static void
vxlan_fwd_handle_aging(uint32_t port_id, uint16_t queue)
{
#define MAX_HANDLING_TIME_MS 10	/*ms*/

	if (queue > vxlan_fwd_ins->nb_queues)
		return;
	doca_flow_aging_handle(vxlan_fwd_ins->ports[port_id], queue, MAX_HANDLING_TIME_MS, 0);
}

/*
 * Dump stats of the given port identifier
 *
 * @port_id [in]: port identifier to dump its stats
 * @return: 0 on success and non-zero value on failure
 */
static int
vxlan_fwd_dump_stats(uint32_t port_id)
{
	return vxlan_fwd_dump_port_stats(port_id, vxlan_fwd_ins->ports[port_id]);
}

/* Stores all functions pointers used by the application */
struct app_vnf vxlan_fwd_vnf = {
	.vnf_init = &vxlan_fwd_init,			/* Simple Forward initialization resouces function pointer */
	.vnf_process_pkt = &vxlan_fwd_handle_packet,	/* Simple Forward packet processing function pointer */
	.vnf_flow_age = &vxlan_fwd_handle_aging,	/* Simple Forward aging handling function pointer */
	.vnf_dump_stats = &vxlan_fwd_dump_stats,	/* Simple Forward dumping stats function pointer */
	.vnf_destroy = &vxlan_fwd_destroy,		/* Simple Forward destroy allocated resources function pointer */
};

/*
 * Sets and stores all function pointers, in order to  call them later in the application
 */
struct app_vnf*
vxlan_fwd_get_vnf(void)
{
	return &vxlan_fwd_vnf;
}
