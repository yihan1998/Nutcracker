#include <stdint.h>
#include <signal.h>

#include <rte_cycles.h>
#include <rte_launch.h>
#include <rte_ethdev.h>

#include <doca_argp.h>
#include <doca_log.h>

#include <dpdk_utils.h>
#include <utils.h>

#include "vxlan_fwd.h"
#include "vxlan_fwd_port.h"
#include "vxlan_fwd_vnf_core.h"

DOCA_LOG_REGISTER(VXLAN_FWD_VNF);

#define DEFAULT_NB_METERS (1 << 13) /* Maximmum number of meters used */

/*
 * Signal handler
 *
 * @signum [in]: The signal received to handle
 */
static void
signal_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM) {
		DOCA_LOG_INFO("Signal %d received, preparing to exit", signum);
		vxlan_fwd_process_pkts_stop();
	}
}

int
main(int argc, char **argv)
{
    doca_error_t result;
	int exit_status = EXIT_SUCCESS;
	struct doca_logger_backend *logger;
	struct vxlan_fwd_port_cfg port_cfg = {0};
	struct application_dpdk_config dpdk_config = {
		.port_config.nb_ports = 2,
		.port_config.nb_queues = 4,
		.port_config.nb_hairpin_q = 4,
		.sft_config = {0},
		.reserve_main_thread = true,
	};
	struct vxlan_fwd_config app_cfg = {
		.dpdk_cfg = &dpdk_config,
		.rx_only = 0,
		.hw_offload = 0,
		.stats_timer = 100000,
		.is_hairpin = false,
	};
	struct app_vnf *vnf;
	struct vxlan_fwd_process_pkts_params process_pkts_params = {.cfg = &app_cfg};


	/* Register a logger backend */
	result = doca_log_create_standard_backend();
	if (result != DOCA_SUCCESS)
		return EXIT_FAILURE;

	/* Parse cmdline/json arguments */
	result = doca_argp_init("doca_vxlan_fwd", &app_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_get_error_string(result));
		return EXIT_FAILURE;
	}
	doca_argp_set_dpdk_program(dpdk_init);
	result = register_vxlan_fwd_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register application params: %s", doca_get_error_string(result));
		doca_argp_destroy();
		return EXIT_FAILURE;
	}
	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse application input: %s", doca_get_error_string(result));
		doca_argp_destroy();
		return EXIT_FAILURE;
	}

	result = doca_log_create_syslog_backend("doca_core", &logger);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate the logger");
		doca_argp_destroy();
		return EXIT_FAILURE;
	}

	/* update queues and ports */
	result = dpdk_queues_and_ports_init(&dpdk_config);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to update application ports and queues: %s", doca_get_error_string(result));
		exit_status = EXIT_FAILURE;
		goto dpdk_destroy;
	}

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	/* convert to number of cycles */
	app_cfg.stats_timer *= rte_get_timer_hz();

	vnf = vxlan_fwd_get_vnf();
	port_cfg.nb_queues = dpdk_config.port_config.nb_queues;
	port_cfg.is_hairpin = app_cfg.is_hairpin;
	port_cfg.nb_meters = DEFAULT_NB_METERS;
	port_cfg.nb_counters = (1 << 13);
	if (vnf->vnf_init(&port_cfg) != 0) {
		DOCA_LOG_ERR("VNF application init error");
		exit_status = EXIT_FAILURE;
		goto exit_app;
	}

	vxlan_fwd_map_queue(dpdk_config.port_config.nb_queues);
	process_pkts_params.vnf = vnf;
	rte_eal_mp_remote_launch(vxlan_fwd_process_pkts, &process_pkts_params, CALL_MAIN);
	rte_eal_mp_wait_lcore();

exit_app:
	/* cleanup app resources */
	vxlan_fwd_destroy(vnf);

	/* DPDK cleanup resources */
	dpdk_queues_and_ports_fini(&dpdk_config);
dpdk_destroy:
	dpdk_fini();

	/* ARGP cleanup */
	doca_argp_destroy();

	return exit_status;
}
