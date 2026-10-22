#include <clocale>
#include <malloc.h>
#include <csignal>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include "core/ipc.hpp"
#include "core/loop.hpp"
#include "island/app.hpp"

int main(int argc, char* argv[]) {
    // Pictures are big, short-lived blocks decoded on worker threads. Left to
    // its defaults glibc keeps freed ones in per-thread arenas (tens of MB
    // that never go back). Big blocks get their own mapping, returned when
    // freed, and fewer arenas.
    mallopt(M_MMAP_THRESHOLD, 256 * 1024);
    mallopt(M_ARENA_MAX, 2);
    // Dates and month names in the user's language; numbers stay "C" so
    // parsing and font strings never meet a decimal comma.
    setlocale(LC_ALL, "");
    setlocale(LC_NUMERIC, "C");
    // With arguments we are a client of the running daemon.
    if (argc > 1) {
        std::string cmd;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (i > 1) cmd += ' ';
            if (arg.find(' ') != std::string::npos) cmd += "\"" + arg + "\"";
            else cmd += arg;
        }
        if (cmd == "--version" || cmd == "-v") {
            std::cout << "niri-island 2.0\n";
            return 0;
        }
        std::string reply;
        if (!island::IpcServer::send(cmd, reply)) {
            if (cmd == "help" || cmd == "--help" || cmd == "-h") {
                std::cout << "niri-island: run without arguments to start the island; with a command to control it.\n";
                return 0;
            }
            std::cerr << "niri-island is not running.\n";
            return 1;
        }
        // "lock" returns once the screen really is locked (swayidle's
        // before-sleep waits for us, so the desktop never shows on resume).
        if (cmd.rfind("lock", 0) == 0 && reply == "locking") {
            for (int i = 0; i < 60; ++i) {
                std::string st;
                if (!island::IpcServer::send("lock-state", st) || st.rfind("locked", 0) == 0 || st.rfind("unlocked", 0) == 0)
                    break;
                usleep(50000);
            }
        }
        std::cout << reply;
        if (!reply.empty() && reply.back() != '\n') std::cout << '\n';
        return 0;
    }

    if (island::IpcServer::instance_running()) {
        std::cerr << "niri-island is already running.\n";
        return 1;
    }

    // Signals arrive as a readable fd: no work inside a signal handler, and
    // the destructors run (socket removed, notes saved).
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGHUP);
    sigprocmask(SIG_BLOCK, &mask, nullptr);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_DFL);
    int sfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);

    {
        island::App app;
        if (!app.init()) {
            std::cerr << "niri-island: initialisation failed\n";
            return 1;
        }
        island::Loop::get().watch(sfd, POLLIN, [sfd](short) {
            signalfd_siginfo si;
            while (read(sfd, &si, sizeof(si)) == sizeof(si)) {
                if (si.ssi_signo == SIGHUP) continue;  // reserved for reload via IPC
                island::Loop::get().quit();
            }
        });
        std::cout << "niri-island running\n";
        app.run();
        island::Loop::get().unwatch(sfd);
        close(sfd);
    }  // the app is gone: notes, state and the IPC socket are saved/removed
    // Out without the global destructors: a detached download still winding
    // down must not meet libraries already torn down under it.
    std::cout.flush();
    std::cerr.flush();
    _exit(0);
}
