#include <instruments/instruments.h>
#include <cstdio>
#include <cstring>
#include <csignal>
#include <atomic>
#include <string>
#include <vector>

using namespace instruments;

static std::atomic<bool> g_running{true};

static void SignalHandler(int) {
    g_running.store(false);
}

static void PrintUsage(const char* prog) {
    fprintf(stderr,
        "Usage: %s <command> [options]\n"
        "\n"
        "Commands:\n"
        "  process list       List running processes\n"
        "  process launch     Launch an app by bundle ID\n"
        "  process kill       Kill a process by PID\n"
        "  fps                Monitor FPS\n"
        "  perf               Monitor system/process performance\n"
        "  xctest             Run XCTest bundle\n"
        "  wda                Run WebDriverAgent\n"
        "  tunnel list        List active tunnels\n"
        "  tunnel start       Start a tunnel for a device\n"
        "  forward            Forward a port\n"
        "\n"
        "Global options:\n"
        "  --udid <UDID>      Target device UDID\n"
        "  --tunnel <addr:port>  Use tunnel address (iOS 17+)\n"
        "  --verbose          Enable debug logging\n"
        "  --quiet            Suppress info logging\n"
        "\n", prog);
}

// Parse command line into a simple key-value map
struct CLIArgs {
    std::string command;
    std::string subcommand;
    std::string udid;
    std::string tunnelAddr;
    uint16_t tunnelPort = 0;
    std::string bundleId;
    std::string testRunnerBundleId;
    std::string xctestConfig;
    int64_t pid = 0;
    uint32_t interval = 1000;
    uint16_t hostPort = 0;
    uint16_t devicePort = 0;
    uint16_t wdaPort = 8100;
    uint16_t mjpegPort = 9100;
    bool verbose = false;
    bool quiet = false;
};

static CLIArgs ParseArgs(int argc, char* argv[]) {
    CLIArgs args;

    int i = 1;
    // Parse command
    if (i < argc && argv[i][0] != '-') {
        args.command = argv[i++];
    }
    // Parse subcommand
    if (i < argc && argv[i][0] != '-') {
        args.subcommand = argv[i++];
    }

    // Parse options
    for (; i < argc; i++) {
        std::string opt = argv[i];
        if (opt == "--udid" && i + 1 < argc) {
            args.udid = argv[++i];
        } else if (opt == "--tunnel" && i + 1 < argc) {
            std::string val = argv[++i];
            auto colon = val.rfind(':');
            if (colon != std::string::npos) {
                args.tunnelAddr = val.substr(0, colon);
                args.tunnelPort = static_cast<uint16_t>(std::atoi(val.substr(colon + 1).c_str()));
            }
        } else if (opt == "--bundle" && i + 1 < argc) {
            args.bundleId = argv[++i];
        } else if (opt == "--runner" && i + 1 < argc) {
            args.testRunnerBundleId = argv[++i];
        } else if (opt == "--xctest" && i + 1 < argc) {
            args.xctestConfig = argv[++i];
        } else if (opt == "--pid" && i + 1 < argc) {
            args.pid = std::atoll(argv[++i]);
        } else if (opt == "--interval" && i + 1 < argc) {
            args.interval = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (opt == "--host-port" && i + 1 < argc) {
            args.hostPort = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (opt == "--device-port" && i + 1 < argc) {
            args.devicePort = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (opt == "--wda-port" && i + 1 < argc) {
            args.wdaPort = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (opt == "--mjpeg-port" && i + 1 < argc) {
            args.mjpegPort = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (opt == "--verbose") {
            args.verbose = true;
        } else if (opt == "--quiet") {
            args.quiet = true;
        }
    }

    return args;
}

static std::shared_ptr<Instruments> ConnectDevice(const CLIArgs& args) {
    if (!args.tunnelAddr.empty()) {
        return Instruments::CreateWithTunnel(args.tunnelAddr, args.tunnelPort);
    }
    if (!args.udid.empty()) {
        return Instruments::Create(args.udid);
    }
    fprintf(stderr, "Error: --udid or --tunnel required\n");
    return nullptr;
}

// --- Command implementations ---

static int CmdProcessList(const CLIArgs& args) {
    auto inst = ConnectDevice(args);
    if (!inst) return 1;

    std::vector<ProcessInfo> procs;
    Error err = inst->Process().GetProcessList(procs);
    if (err != Error::Success) {
        fprintf(stderr, "Error: %s\n", ErrorToString(err));
        return 1;
    }

    printf("%-8s %-6s %-40s %s\n", "PID", "Type", "Bundle ID", "Name");
    printf("%-8s %-6s %-40s %s\n", "---", "----", "---------", "----");
    for (const auto& p : procs) {
        printf("%-8lld %-6s %-40s %s\n",
               (long long)p.pid,
               p.isApplication ? "App" : "Proc",
               p.bundleId.c_str(),
               p.name.c_str());
    }
    printf("\nTotal: %zu processes\n", procs.size());
    return 0;
}

static int CmdProcessLaunch(const CLIArgs& args) {
    if (args.bundleId.empty()) {
        fprintf(stderr, "Error: --bundle required\n");
        return 1;
    }

    auto inst = ConnectDevice(args);
    if (!inst) return 1;

    int64_t pid = 0;
    Error err = inst->Process().LaunchApp(args.bundleId, pid);
    if (err != Error::Success) {
        fprintf(stderr, "Error: %s\n", ErrorToString(err));
        return 1;
    }

    printf("Launched %s with PID %lld\n", args.bundleId.c_str(), (long long)pid);
    return 0;
}

static int CmdProcessKill(const CLIArgs& args) {
    if (args.pid == 0) {
        fprintf(stderr, "Error: --pid required\n");
        return 1;
    }

    auto inst = ConnectDevice(args);
    if (!inst) return 1;

    Error err = inst->Process().KillProcess(args.pid);
    if (err != Error::Success) {
        fprintf(stderr, "Error: %s\n", ErrorToString(err));
        return 1;
    }

    printf("Killed PID %lld\n", (long long)args.pid);
    return 0;
}

static int CmdFPS(const CLIArgs& args) {
    auto inst = ConnectDevice(args);
    if (!inst) return 1;

    printf("Monitoring FPS (interval=%ums, Ctrl+C to stop)...\n", args.interval);

    Error err = inst->FPS().Start(args.interval,
        [](const FPSData& data) {
            printf("FPS: %.0f  GPU: %.1f%%\n", data.fps, data.gpuUtilization);
        },
        [](Error e, const std::string& msg) {
            fprintf(stderr, "Error: %s - %s\n", ErrorToString(e), msg.c_str());
        }
    );

    if (err != Error::Success) {
        fprintf(stderr, "Error: %s\n", ErrorToString(err));
        return 1;
    }

    while (g_running.load() && inst->FPS().IsRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    inst->FPS().Stop();
    return 0;
}

static int CmdPerf(const CLIArgs& args) {
    auto inst = ConnectDevice(args);
    if (!inst) return 1;

    printf("Monitoring performance (interval=%ums, Ctrl+C to stop)...\n", args.interval);

    PerfConfig config;
    config.sampleIntervalMs = args.interval;

    Error err = inst->Performance().Start(config,
        [](const SystemMetrics& m) {
            printf("CPU: %.1f%% (user: %.1f%%, sys: %.1f%%)  "
                   "Net I/O: %llu/%llu bytes\n",
                   m.cpuTotalLoad, m.cpuUserLoad, m.cpuSystemLoad,
                   (unsigned long long)m.netBytesIn,
                   (unsigned long long)m.netBytesOut);
        },
        [](const std::vector<ProcessMetrics>& procs) {
            for (const auto& p : procs) {
                if (p.cpuUsage > 0.1) {
                    printf("  PID %-6lld CPU: %5.1f%%  MEM: %lluKB  %s\n",
                           (long long)p.pid, p.cpuUsage,
                           (unsigned long long)(p.memResident / 1024),
                           p.name.c_str());
                }
            }
        },
        [](Error e, const std::string& msg) {
            fprintf(stderr, "Error: %s - %s\n", ErrorToString(e), msg.c_str());
        }
    );

    if (err != Error::Success) {
        fprintf(stderr, "Error: %s\n", ErrorToString(err));
        return 1;
    }

    while (g_running.load() && inst->Performance().IsRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    inst->Performance().Stop();
    return 0;
}

static int CmdXCTest(const CLIArgs& args) {
    if (args.bundleId.empty() || args.testRunnerBundleId.empty()) {
        fprintf(stderr, "Error: --bundle and --runner required\n");
        return 1;
    }

    auto inst = ConnectDevice(args);
    if (!inst) return 1;

    XCTestConfig config;
    config.bundleId = args.bundleId;
    config.testRunnerBundleId = args.testRunnerBundleId;
    config.xctestConfigName = args.xctestConfig.empty()
        ? "UITests.xctest" : args.xctestConfig;

    printf("Running XCTest (Ctrl+C to stop)...\n");

    int passed = 0, failed = 0;

    Error err = inst->XCTest().Run(config,
        [&passed, &failed](const TestResult& r) {
            const char* status = r.status == TestResult::Status::Passed ? "PASS" : "FAIL";
            printf("[%s] %s/%s (%.3fs)\n", status,
                   r.className.c_str(), r.methodName.c_str(), r.duration);
            if (r.status == TestResult::Status::Passed) passed++;
            else failed++;
            if (!r.errorMessage.empty()) {
                printf("       %s (%s:%d)\n",
                       r.errorMessage.c_str(), r.errorFile.c_str(), r.errorLine);
            }
        },
        [](const std::string& log) {
            printf("[LOG] %s\n", log.c_str());
        },
        [](Error e, const std::string& msg) {
            fprintf(stderr, "[ERR] %s - %s\n", ErrorToString(e), msg.c_str());
        }
    );

    printf("\nResults: %d passed, %d failed\n", passed, failed);
    return (err == Error::Success && failed == 0) ? 0 : 1;
}

static int CmdWDA(const CLIArgs& args) {
    if (args.bundleId.empty()) {
        fprintf(stderr, "Error: --bundle required (WDA bundle ID)\n");
        return 1;
    }

    auto inst = ConnectDevice(args);
    if (!inst) return 1;

    WDAConfig config;
    config.bundleId = args.bundleId;
    config.testRunnerBundleId = args.testRunnerBundleId.empty()
        ? args.bundleId : args.testRunnerBundleId;
    config.wdaPort = args.wdaPort;
    config.mjpegPort = args.mjpegPort;

    printf("Starting WebDriverAgent (Ctrl+C to stop)...\n");

    Error err = inst->WDA().Start(config,
        [](const std::string& log) {
            printf("[WDA] %s\n", log.c_str());
        },
        [](Error e, const std::string& msg) {
            fprintf(stderr, "[ERR] %s - %s\n", ErrorToString(e), msg.c_str());
        }
    );

    if (err != Error::Success) {
        fprintf(stderr, "Error: %s\n", ErrorToString(err));
        return 1;
    }

    printf("WDA running - HTTP: http://localhost:%u  MJPEG: http://localhost:%u\n",
           inst->WDA().GetWDAPort(), inst->WDA().GetMJPEGPort());

    while (g_running.load() && inst->WDA().IsRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    inst->WDA().Stop();
    return 0;
}

static int CmdTunnel(const CLIArgs& args) {
    TunnelManager mgr;

    if (args.subcommand == "list") {
        auto tunnels = mgr.GetActiveTunnels();
        if (tunnels.empty()) {
            printf("No active tunnels\n");
        } else {
            printf("%-40s %-40s %s\n", "UDID", "Address", "RSD Port");
            for (const auto& t : tunnels) {
                printf("%-40s %-40s %u\n", t.udid.c_str(), t.address.c_str(), t.rsdPort);
            }
        }
        return 0;
    }

    if (args.subcommand == "start") {
        if (args.udid.empty()) {
            fprintf(stderr, "Error: --udid required\n");
            return 1;
        }
        TunnelInfo info;
        Error err = mgr.StartTunnel(args.udid, info);
        if (err != Error::Success) {
            fprintf(stderr, "Error: %s\n", ErrorToString(err));
            return 1;
        }
        printf("Tunnel started: %s -> %s:%u\n",
               info.udid.c_str(), info.address.c_str(), info.rsdPort);
        return 0;
    }

    fprintf(stderr, "Unknown tunnel subcommand: %s\n", args.subcommand.c_str());
    return 1;
}

static int CmdForward(const CLIArgs& args) {
    if (args.hostPort == 0 || args.devicePort == 0) {
        fprintf(stderr, "Error: --host-port and --device-port required\n");
        return 1;
    }

    auto inst = ConnectDevice(args);
    if (!inst) return 1;

    uint16_t actualPort = 0;
    Error err = inst->Ports().Forward(args.hostPort, args.devicePort, &actualPort);
    if (err != Error::Success) {
        fprintf(stderr, "Error: %s\n", ErrorToString(err));
        return 1;
    }

    printf("Forwarding localhost:%u -> device:%u (Ctrl+C to stop)\n",
           actualPort, args.devicePort);

    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    inst->Ports().StopAll();
    return 0;
}

int main(int argc, char* argv[]) {
    signal(SIGINT, SignalHandler);
#ifdef SIGTERM
    signal(SIGTERM, SignalHandler);
#endif

    if (argc < 2) {
        PrintUsage(argv[0]);
        return 1;
    }

    CLIArgs args = ParseArgs(argc, argv);

    // Configure logging
    if (args.verbose) {
        Instruments::SetLogLevel(LogLevel::Debug);
    } else if (args.quiet) {
        Instruments::SetLogLevel(LogLevel::Error);
    }

    // Dispatch command
    if (args.command == "process") {
        if (args.subcommand == "list") return CmdProcessList(args);
        if (args.subcommand == "launch") return CmdProcessLaunch(args);
        if (args.subcommand == "kill") return CmdProcessKill(args);
        fprintf(stderr, "Unknown process subcommand: %s\n", args.subcommand.c_str());
        return 1;
    }
    if (args.command == "fps") return CmdFPS(args);
    if (args.command == "perf") return CmdPerf(args);
    if (args.command == "xctest") return CmdXCTest(args);
    if (args.command == "wda") return CmdWDA(args);
    if (args.command == "tunnel") return CmdTunnel(args);
    if (args.command == "forward") return CmdForward(args);

    fprintf(stderr, "Unknown command: %s\n", args.command.c_str());
    PrintUsage(argv[0]);
    return 1;
}
