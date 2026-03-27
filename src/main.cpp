#include "Bridge.hpp"
#include "Config.hpp"
#include "Logger.hpp"

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>

static std::unique_ptr<Bridge> g_bridge;

static void signalHandler(int sig) {
    Logger::info("[Main] Signal %d received – shutting down…", sig);
    if (g_bridge) g_bridge->stop();
}

static void printUsage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s [OPTIONS]\n"
        "\n"
        "Options:\n"
        "  -c <file>   Path to JSON config file (default: config.json)\n"
        "  -l <level>  Log level: debug|info|warn|error (overrides config)\n"
        "  -h          Show this help\n"
        "\n"
        "ThingsBoard ↔ Wappsto MQTT conversion bridge\n"
        "Version: 1.0.0\n",
        prog);
}

int main(int argc, char* argv[]) {
    std::string configPath = "config.json";
    std::string logLevelOverride;

    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return EXIT_SUCCESS;
        } else if (arg == "-c" && i + 1 < argc) {
            configPath = argv[++i];
        } else if (arg == "-l" && i + 1 < argc) {
            logLevelOverride = argv[++i];
        } else {
            std::fprintf(stderr, "Unknown argument: %s\n", arg.c_str());
            printUsage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    // Load config
    Config cfg;
    try {
        cfg = Config::load(configPath);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Config error: %s\n", e.what());
        return EXIT_FAILURE;
    }

    // Apply log level
    std::string logLevel = logLevelOverride.empty() ? cfg.log_level : logLevelOverride;
    Logger::setLevel(logLevel);
    Logger::info("[Main] Config loaded from %s", configPath.c_str());
    Logger::info("[Main] Log level: %s", logLevel.c_str());
    Logger::info("[Main] Mapping mode: %s",
        cfg.mapping.mode == MappingMode::STATIC ? "static" : "dynamic");

    // Signal handlers
    std::signal(SIGINT,  signalHandler);
    std::signal(SIGTERM, signalHandler);
    std::signal(SIGPIPE, SIG_IGN); // Ignore broken pipe from TLS socket

    // Create and start bridge
    g_bridge = std::make_unique<Bridge>(cfg);
    if (!g_bridge->start()) {
        Logger::error("[Main] Bridge failed to start");
        return EXIT_FAILURE;
    }

    Logger::info("[Main] Bridge running – press Ctrl+C to stop");
    g_bridge->run(); // blocks until stop() is called

    g_bridge.reset();
    Logger::info("[Main] Done");
    return EXIT_SUCCESS;
}
