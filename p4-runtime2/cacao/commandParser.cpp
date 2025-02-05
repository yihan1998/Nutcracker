#include "utils/commandParser.h"
// #include "flowPipe.h"
#include "backend/doca.h"
#include "drivers/doca/common.h"

#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <cstdarg>
#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <arpa/inet.h>

using namespace llvm;
using namespace llvm::orc;

#define CALL_ADD_PIPE_ENTRY(pipe, pipeName, args...) \
    pipe->hwPipe.ops.add_pipe_entry(pipe, pipeName, ##args)

std::vector<Pipeline*> pipelines;

void CommandParser::instantializeHwPipe(Pipeline& pipeline, struct flow_pipe * pipe, const json& hwOps) {
    if(hwOps.contains("create_pipe")) {
        std::string name = hwOps["create_pipe"];
        auto symbol = cantFail(pipeline.JitEnv->lookup(name));
        auto func = (int (*)(struct flow_pipe *))symbol.getAddress();
        pipe->hwPipe.ops.create_pipe = func;
    }
    if(hwOps.contains("init_pipe")) {
        std::string name = hwOps["init_pipe"];
        auto symbol = cantFail(pipeline.JitEnv->lookup(name));
        auto func = (int (*)(struct flow_pipe *))symbol.getAddress();
        pipe->hwPipe.ops.init_pipe = func;
    }
    if(hwOps.contains("get_actions_list")) {
        std::string name = hwOps["get_actions_list"];
        auto symbol = cantFail(pipeline.JitEnv->lookup(name));
        auto func = (int (*)(struct flow_pipe *))symbol.getAddress();
        pipe->hwPipe.ops.get_actions_list = func;
    }
    if(hwOps.contains("add_pipe_entry")) {
        std::string name = hwOps["add_pipe_entry"];
        auto symbol = cantFail(pipeline.JitEnv->lookup(name));
        auto func = (int (*)(struct flow_pipe *, ...))symbol.getAddress();
        pipe->hwPipe.ops.add_pipe_entry = (int (*)(struct flow_pipe *, const char *, ...))func;
    }
    memset(pipe->hwPipe.nb_entries, 0, sizeof(int) * DOCA_MAX_PORTS);
    memset(pipe->hwPipe.entries, 0, sizeof(struct doca_flow_pipe_entry *) * 16 * DOCA_MAX_PORTS);
}

void CommandParser::instantializeFsmPipe(Pipeline& pipeline, struct flow_pipe * pipe, const json& fsmOps) {
    if(fsmOps.contains("create_pipe")) {
        std::string name = fsmOps["create_pipe"];
        auto symbol = cantFail(pipeline.JitEnv->lookup(name));
        auto func = (int (*)(struct flow_pipe *))symbol.getAddress();
        pipe->swPipe.ops.create_pipe = func;
    }
    // if(fsmOps.contains("init_pipe")) {
    //     std::string name = hwOps["init_pipe"];
    //     auto symbol = cantFail(pipeline.JitEnv->lookup(name));
    //     auto func = (int (*)(struct flow_pipe *))symbol.getAddress();
    //     pipe->hwPipe.ops.init_pipe = func;
    // }
    // if(fsmOps.contains("get_actions_list")) {
    //     std::string name = hwOps["get_actions_list"];
    //     auto symbol = cantFail(pipeline.JitEnv->lookup(name));
    //     auto func = (int (*)(struct flow_pipe *))symbol.getAddress();
    //     pipe->hwPipe.ops.get_actions_list = func;
    // }
    if(fsmOps.contains("add_pipe_entry")) {
        std::string name = fsmOps["add_pipe_entry"];
        auto symbol = cantFail(pipeline.JitEnv->lookup(name));
        auto func = (int (*)(struct flow_pipe *, ...))symbol.getAddress();
        pipe->swPipe.ops.add_pipe_entry = (int (*)(struct flow_pipe *, const char *, ...))func;
    }
    if(fsmOps.contains("run")) {
        std::string name = fsmOps["run"];
        auto symbol = cantFail(pipeline.JitEnv->lookup(name));
        auto func = (struct flow_pipe * (*)(struct flow_pipe * pipe, struct sk_buff *skb, ...))symbol.getAddress();
        pipe->swPipe.ops.run = func;
    }

    init_list_head(&pipe->swPipe.list);
}

void CommandParser::displayTableInfo(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        std::cout << "Usage: table_info <table_name>" << std::endl;
        return;
    }

    const std::string& tableName = args[1];
    // struct flow_pipe * pipe = flow_get_pipe(tableName.c_str());
    struct flow_pipe * pipe = NULL;
    for (auto p : pipelines) {
        pipe = p->lookupFlowPipe(tableName.c_str());
        if (pipe) break;
    }
    if (!pipe) {
        std::cout << "Table " << tableName << "not found!" << std::endl;
        return;
    }
    std::cout << "Table info for: " << tableName << std::endl;
    std::cout << "\tActions: ";
    for (int i = 0; i < pipe->nb_actions; i++) {
        std::cout << pipe->actions[i]->name;
        if (i < pipe->nb_actions - 1) std::cout << ", ";
    }
    std::cout << std::endl;
}

bool parseMacAddress(const std::string& macStr, uint8_t mac[6]) {
    unsigned int macVals[6];
    if (sscanf(macStr.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x", 
                &macVals[0], &macVals[1], &macVals[2], 
                &macVals[3], &macVals[4], &macVals[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; ++i) {
        mac[i] = static_cast<uint8_t>(macVals[i]);
    }
    return true;
}

void CommandParser::addTableEntry(const std::vector<std::string>& args) {
    if (args.size() < 4) {
        std::cout << "Usage: table_add <table_name> <action_name> <match_fields> => <action_parameters>" << std::endl;
        return;
    }

    std::string tableName = args[1];
    std::string actionName = args[2];

    int i;
    // struct flow_pipe * pipe = p->lookupFlowPipe(tableName.c_str());
    struct flow_pipe * pipe = NULL;
    for (auto p : pipelines) {
        pipe = p->lookupFlowPipe(tableName.c_str());
        if (pipe) break;
    }
    if (!pipe) {
        std::cout << "Table " << tableName << " not found!" << std::endl;
        return;
    }

    for (i = 0; i < pipe->nb_actions; i++) {
        if (actionName == std::string(pipe->actions[i]->name)) {
            break;
        }
    }

    if (i == pipe->nb_actions) {
        std::cout << "Failed to find action " << actionName << " in table " << tableName << std::endl;
        return;
    }

    std::string pipeName = std::string(pipe->actions[i]->name) + "_" + std::to_string(pipe->actions[i]->id);
    std::replace(pipeName.begin(), pipeName.end(), '.', '_');

    printf("Action pipe: %s\n", pipeName.c_str());

    std::vector<std::string> params(args.begin() + 3, args.end());

    // Prepare a vector to hold the arguments to be passed
    std::vector<uint32_t> argList;
    std::vector<uint8_t> macList;  // This will hold MAC addresses

    // Iterate through params and process accordingly
    for (const auto& param : params) {
        if (param.find(':') != std::string::npos) {
            // If the parameter has a ':', we treat it as a MAC address
            uint8_t mac[6];
            if (!parseMacAddress(param, mac)) {
                std::cout << "Invalid MAC address: " << param << std::endl;
                return;
            }
            // Add each byte of the MAC address to the macList
            macList.insert(macList.end(), std::begin(mac), std::end(mac));
        } else if (param.find('.') != std::string::npos) {
            // If it has a '.', treat it as an IP address
            uint32_t ip_addr = inet_addr(param.c_str());
            if (ip_addr == INADDR_NONE) {
                std::cout << "Invalid IP address: " << param << std::endl;
                return;
            }
            argList.push_back(ip_addr);  // Add to argument list
        } else {
            // Otherwise, treat it as a port, VNI, etc.
            uint64_t value = std::stoull(param);  // Convert to uint64_t
            argList.push_back(static_cast<uint32_t>(value));  // Assuming add_pipe_entry can take uint32_t
        }
    }

    // Call the action pipe function with MAC addresses and other arguments
    switch (argList.size()) {
        case 0:
            CALL_ADD_PIPE_ENTRY(pipe, pipeName.c_str(), macList.data());
            break;
        case 1:
            CALL_ADD_PIPE_ENTRY(pipe, pipeName.c_str(), macList.data(), argList[0]);
            break;
        case 2:
            CALL_ADD_PIPE_ENTRY(pipe, pipeName.c_str(), macList.data(), argList[0], argList[1]);
            break;
        case 3:
            CALL_ADD_PIPE_ENTRY(pipe, pipeName.c_str(), macList.data(), argList[0], argList[1], argList[2]);
            break;
        case 4:
            CALL_ADD_PIPE_ENTRY(pipe, pipeName.c_str(), macList.data(), argList[0], argList[1], argList[2], argList[3]);
            break;
        case 5:
            CALL_ADD_PIPE_ENTRY(pipe, pipeName.c_str(), macList.data(), argList[0], argList[1], argList[2], argList[3], argList[4]);
            break;
        default:
            std::cout << "Too many arguments (max is 5 after MAC addresses)!" << std::endl;
            return;
    }
}

void CommandParser::loadJson(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        std::cerr << "Usage: load <file_path>" << std::endl;
        return;
    }

    std::ifstream file(args[1]);
    if (!file) {
        std::cerr << "Error: Could not open file " << args[1] << std::endl;
        return;
    }

    json j;
    file >> j;

    if (!j.contains("source_code")) {
        std::cerr << "Source code file not specified!" << std::endl;
        return;
    }

    std::string configFile = j["source_code"];
    std::string pipelineName = j["name"];
    std::cout << "Loading from " << configFile << std::endl;

    Pipeline *pipeline = new Pipeline(pipelineName, configFile);
    // p = pipeline;

    std::cout << "\033[42m\033[37m" << "Creating all pipes... " << "\033[0m" << std::endl;

    // // Process the sw_fsa_pipe first
    // json sw_fsa_pipe = j["sw_fsa_pipe"];
    // std::string swFsaPipeName = sw_fsa_pipe["name"];
    // std::cout << "  Create pipe " << swFsaPipeName << std::endl;

    // struct flow_pipe *newFlowPipe = instantialize_flow_pipe(swFsaPipeName.c_str());

    // if (sw_fsa_pipe.contains("hw_enable") && sw_fsa_pipe["hw_enable"]) {
    //     std::cout << "    Create HW Pipe for " << swFsaPipeName << " => " << "\033[32m" << "[DONE]" << "\033[0m" << std::endl;
    //     instantializeHwPipe(*pipeline, newFlowPipe, sw_fsa_pipe["hw_ops"]);
    //     newFlowPipe->hwPipe.ops.create_pipe(newFlowPipe);
    // }

    // pipeline->Pipes.push_back(newFlowPipe);
    // std::cout << "  Create pipe " << swFsaPipeName << " => " << "\033[32m" << "[DONE]" << "\033[0m" << std::endl;

    json pipes = j["pipes"];
    for (const auto& pipe : pipes) {
        std::string pipeName = pipe["name"];

        struct flow_pipe *newFlowPipe = instantialize_flow_pipe(pipeName.c_str());
        
        // Initialize action array
        if (pipe.contains("actions")) {
            const auto& actions = pipe["actions"];
            newFlowPipe->nb_actions = actions.size();
            newFlowPipe->actions = new struct actionEntry*[newFlowPipe->nb_actions]; // Allocate memory for actions array

            for (size_t i = 0; i < newFlowPipe->nb_actions; i++) {
                newFlowPipe->actions[i] = new struct actionEntry;
                std::strncpy(newFlowPipe->actions[i]->name, actions[i]["name"].get<std::string>().c_str(), sizeof(newFlowPipe->actions[i]->name) - 1);
                newFlowPipe->actions[i]->name[sizeof(newFlowPipe->actions[i]->name) - 1] = '\0'; // Ensure null-termination
                newFlowPipe->actions[i]->id = actions[i]["id"];
            }
        } else {
            newFlowPipe->nb_actions = 0;
        }

        pipeline->Pipes.push_back(newFlowPipe);

        if (pipe.contains("hw_enable") && pipe["hw_enable"]) {
            std::cout << "    Instantialize HW Pipe for " << pipeName << " => " << "\033[32m" << "[DONE]" << "\033[0m" << std::endl;
            instantializeHwPipe(*pipeline, newFlowPipe, pipe["hw_ops"]);
            // newFlowPipe->hwPipe.ops.create_pipe(newFlowPipe);
        }

        if (pipe.contains("fsm_enable") && pipe["fsm_enable"]) {
            std::cout << "    Instantialize FSM Pipe for " << pipeName << " => " << "\033[32m" << "[DONE]" << "\033[0m" << std::endl;
            instantializeFsmPipe(*pipeline, newFlowPipe, pipe["fsm_ops"]);
            // newFlowPipe->hwPipe.ops.create_pipe(newFlowPipe);
        }

        std::cout << "  Instantialize pipe " << pipeName << " => " << "\033[32m" << "[DONE]" << "\033[0m" << std::endl;
    }

    pipelines.push_back(pipeline);

    for (const auto& pipe : pipeline->Pipes) {
        if (pipe->hwPipe.ops.create_pipe) {
            std::cout << "Creating HW pipe for " << pipe->name << " => ";
            pipe->hwPipe.ops.create_pipe(pipe);
            std::cout << "\033[32m" << "[DONE]" << "\033[0m" << std::endl;
        }
        if (pipe->swPipe.ops.create_pipe) {
            std::cout << "Creating FSM pipe for " << pipe->name << " => ";
            pipe->swPipe.ops.create_pipe(pipe);
            std::cout << "\033[32m" << "[DONE]" << "\033[0m" << std::endl;
        }
    }

    // Initialize all pipes
    std::cout << "\033[42m\033[37m" << "Initializing all pipes... " << "\033[0m" << std::endl;
    for (auto& pipe : pipeline->Pipes) {
        if (pipe->hwPipe.ops.init_pipe) {
            std::cout << "  Initialize the HW pipe for " << pipe->name << " ... " << std::endl;
            pipe->hwPipe.ops.init_pipe(pipe);
        }
    }
}

void CommandParser::runTest(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        std::cerr << "Usage: load <file_path>" << std::endl;
        return;
    }

    if (args[1] == "vxlan") {
        std::cout << "Run VXLAN offloading test..." << std::endl;
        // vxlan_encap_hw_offloading();
        vxlan_encap_sw();
    } else if (args[1] == "flow_monitor") {
        std::cout << "Run FLOW MONITOR offloading test..." << std::endl;
        flow_monitor_hw_offloading();
        // flow_monitor_sw();
    }

    return;
}
