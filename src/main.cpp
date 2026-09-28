#include "island.hpp"
#include <csignal>
#include <iostream>

static island::IslandApp* g_app = nullptr;

static void signal_handler(int sig) {
    (void)sig;
    std::cout << "\nTerminating niri-island...\n";
    exit(0);
}

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    std::cout << "Starting niri-island (C++20 Dynamic Island for Wayland)...\n";

    island::IslandApp app;
    g_app = &app;

    if (!app.init()) {
        std::cerr << "Initialization failed!\n";
        return 1;
    }

    std::cout << "niri-island running on Wayland layer-shell.\n";
    app.run();

    return 0;
}
