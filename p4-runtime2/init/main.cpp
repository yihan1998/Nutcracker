#include <stdio.h>
#include <stdlib.h>
#include <thread>
#include <chrono>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <csignal>

#include "utils/netStat.h"
#include "utils/commandParser.h"
#include "utils/jit.h"
#include "syscall.h"
#include "net/dpdk.h"
#include "net/net.h"
#include "fs/fs.h"
#include "fs/file.h"
#include "ipc/ipc.h"
#include "kernel/sched.h"
#include "kernel/threads.h"
#include "utils/printk.h"
#ifdef CONFIG_DOCA
#include "drivers/doca/common.h"
#include "drivers/doca/context.h"
#endif  /* CONFIG_DOCA */
// #include "jit.h"
// #include "shell.h"

#include "backend/flowPipe.h"
#include "backend/flowPipeInternal.h"

#undef CR1
#undef CR2

#define BE_IPV4_ADDR(a, b, c, d) \
	(RTE_BE32(((uint32_t)a<<24) + (b<<16) + (c<<8) + d))

bool kernel_early_boot = true;
bool kernel_shutdown = false;

struct lcore_arg {
	// enum Role role;
    int shell_pipe_fd;
};

void signal_handler(int sig) { 
    switch (sig) {
        case SIGINT:
            pr_info("Caught SIGINT! Terminating...\n");
            pr_info("Goodbye!\n");
            // stop_doca_flow_ports();
            exit(1);
            break;
        default:
            break;
    }
}

void dpdkThread(int readFd) {
    // char buffer[256];
    while (true) {
        // int nbytes = read(readFd, buffer, sizeof(buffer));
        // if (nbytes > 0) {
        //     buffer[nbytes] = '\0';
        //     std::string command(buffer);
        //     parser.parseCommand(command);
        // }

        // dpdk_recv((int *)&netStats.secRecv, (int *)&netStats.secSend);
        net_loop();
    }
}

int run_control_plane(int readFd) {
    CommandParser parser;
#if 0
    {
        std::string cmd = "load_json";
        std::string C_input = "/home/ubuntu/Nutcracker/utils/p4-nutcracker/out/ingress.json";
        std::vector<std::string> input;
        input.push_back(cmd);
        input.push_back(C_input);
        parser.loadJson(input);   
    }
#endif
#if 0
    {
        std::string cmd = "load_json";
        std::string C_input = "/home/ubuntu/Nutcracker/utils/p4-nutcracker/out/egress.json";
        std::vector<std::string> input;
        input.push_back(cmd);
        input.push_back(C_input);
        parser.loadJson(input);   
    }
#endif
#if 0
    {
        std::string cmd = "load_json";
        std::string C_input = "/home/ubuntu/Nutcracker/utils/p4-nutcracker/C_CODE/egress.json";
        std::vector<std::string> input;
        input.push_back(cmd);
        input.push_back(C_input);
        parser.loadJson(input);   
    }
#endif
#if 0
    {
        std::string cmd = "run_test";
        std::string test_input = "vxlan";
        std::vector<std::string> input;
        input.push_back(cmd);
        input.push_back(test_input);
        parser.runTest(input);
    }
#endif
#if 1
    {
        std::string cmd = "load_json";
        std::string C_input = "/home/ubuntu/Nutcracker/utils/p4-nutcracker/FLOW_MONITOR/ingress.json";
        std::vector<std::string> input;
        input.push_back(cmd);
        input.push_back(C_input);
        parser.loadJson(input);   
    }
#endif
#if 0
    {
        std::string cmd = "load_json";
        std::string C_input = "/home/ubuntu/Nutcracker/utils/p4-nutcracker/FLOW_MONITOR_C/ingress.json";
        std::vector<std::string> input;
        input.push_back(cmd);
        input.push_back(C_input);
        parser.loadJson(input);   
    }
#endif
#if 1
    {
        std::string cmd = "run_test";
        std::string test_input = "flow_monitor";
        std::vector<std::string> input;
        input.push_back(cmd);
        input.push_back(test_input);
        parser.runTest(input);
    }
#endif
    while (1) {
        ipc_poll();
        net_loop();
    }
}

int lcore_main(void * arg) {
    struct lcore_arg * args = (struct lcore_arg *)arg;
    int lid = rte_lcore_id();
    bool is_main = false;

    pistachio_lcore_init(lid);

#if RTE_VERSION >= RTE_VERSION_NUM(20, 11, 0, 0)
    if (lid == rte_get_main_lcore()) is_main = true;
#else
    if (lid == rte_get_master_lcore()) is_main = true;
#endif

    if (is_main) {
        run_control_plane(args->shell_pipe_fd);
    } else {
        while (1) {
            net_loop();
        };        
    }

    return 0;
}

int main(int argc, char **argv) {
    int pipeFds[2];
	struct lcore_arg * args;
    int i, n, lcore_id;

    pr_info("init: starting DPDK...\n");
    dpdk_init(argc, argv);

#ifdef CONFIG_DOCA
    pr_info("init: starting DOCA...\n");
    doca_init();
    docadv_init();
#endif  /* CONFIG_DOCA */

    /* Register termination handling callback */
    signal(SIGINT, signal_handler);

    pr_info("init: starting JIT...\n");
    jit_init();

    pipe(pipeFds);

    int flags = fcntl(pipeFds[0], F_GETFL, 0);
    fcntl(pipeFds[0], F_SETFL, flags | O_NONBLOCK);

    // NetStats netStats;
    // Shell shell(netStats);
    // std::thread shellThread(&Shell::run, &shell, pipeFds[1]);
    // shellThread.detach();

    // DPDK thread
    // std::thread dpdk(dpdkThread, std::ref(netStats), pipeFds[0]);
    // dpdk.join();

    kernel_early_boot = false;

    pr_info("init: initializing fs...\n");
	fs_init();

    /* Reserve 0, 1, and 2 for stdin, stdout, and stderr */
    set_open_fd(0);
    set_open_fd(1);
    set_open_fd(2);

	pr_info("init: initializing worker threads...\n");
    worker_init();

    pr_info("init: initializing ipc...\n");
	ipc_init();

    /* Init scheduler */
    pr_info("Init: Flax module...\n");
    flax_init();

    /* Init network IO */
    pr_info("Init: pistachIO module...\n");
    pistachio_init();

    /* Init runtime */
    pr_info("Init: Cacao module...\n");
    cacao_init();

    /* Init syscall */
    pr_info("init: initializing SYSCALL...\n");
    syscall_hook_init();

    i = 0;
    n = rte_lcore_count();

    args = (struct lcore_arg *)calloc(n, sizeof(struct lcore_arg));
    RTE_LCORE_FOREACH(lcore_id) {
        struct lcore_context * ctx = &lcore_ctxs[lcore_id];
#ifdef CONFIG_DOCA
        docadv_ctx_init(&ctx->docadv_ctx);
#endif  /* CONFIG_DOCA */
        args[i].shell_pipe_fd = pipeFds[1];
        i++;
    }

    /* Launch per-lcore init on every lcore */
	rte_eal_mp_remote_launch(lcore_main, args, CALL_MAIN);
	rte_eal_mp_wait_lcore();

	/* clean up the EAL */
	rte_eal_cleanup();

    return 0;
}