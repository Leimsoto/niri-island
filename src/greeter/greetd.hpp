#pragma once

#include <string>
#include <vector>

namespace island {

// A client of greetd's IPC (JSON over $GREETD_SOCK, each message prefixed by
// its length as a native-endian u32). greetd stays the small root process
// that opens the session with PAM; the greeter only asks it to.
//
// Runs the whole exchange for one login and blocks (call it off the main
// thread): create the session, answer every prompt with the password,
// then start `cmd`. Returns "" when the session was started, otherwise
// what went wrong, ready to show.
std::string greetd_login(const std::string& socket_path, const std::string& user, const std::string& password,
                         const std::vector<std::string>& cmd, const std::vector<std::string>& env);

}  // namespace island
