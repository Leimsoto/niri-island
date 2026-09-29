#include "island.hpp"
#include "ipc.hpp"
#include <csignal>
#include <iostream>
#include <string>

static island::IslandApp* g_app = nullptr;

static void signal_handler(int sig) {
    (void)sig;
    std::cout << "\nTerminating niri-island...\n";
    exit(0);
}

int main(int argc, char* argv[]) {
    // If command arguments are provided, act as IPC client to running daemon
    if (argc > 1) {
        std::string cmd = argv[1];
        if (cmd == "--help" || cmd == "-h" || cmd == "help") {
            std::cout << "Usage: niri-island [command]\n\n"
                      << "Commands:\n"
                      << "  toggle-control-center, cc   Toggle the Control Center panel\n"
                      << "  toggle-media, expand        Toggle expanded music view\n"
                      << "  play-pause                  Toggle media play/pause\n"
                      << "  next                        Play next track\n"
                      << "  prev                        Play previous track\n"
                      << "  toggle-mute                 Toggle audio mute\n"
                      << "  toggle-wifi                 Toggle Wi-Fi radio\n"
                      << "  toggle-bluetooth            Toggle Bluetooth power\n"
                      << "  toggle-dnd                  Toggle Do Not Disturb\n"
                      << "  close                       Collapse island to idle\n"
                      << "  status                      Print current daemon status\n\n"
                      << "Without arguments: starts the niri-island Wayland daemon.\n";
            return 0;
        }

        std::string reply;
        if (island::IpcServer::send_client_command(cmd, reply)) {
            std::cout << reply;
            return 0;
        } else {
            std::cerr << "niri-island daemon is not running.\n";
            return 1;
        }
    }

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
