#pragma once

#include "control/Controller.h"

#include <sys/types.h>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

// The local control socket: AF_UNIX, SOCK_STREAM, newline-delimited JSON (control/Controller.h).
// A thin pump between file descriptors and the Controller, with no thread of its own: the app
// calls pump from a timer on the message thread, tests call it by hand. Every fd is non-blocking
// and SO_NOSIGPIPE, and every client must run as this user.
namespace overdub::control
{
struct ServerConfig
{
    bool enabled = true;
    std::string socketPath; // absolute: start refuses a relative one
    // The folder holding the socket belongs to it alone: created if missing and set to 0700.
    // False for a path the user chose, whose folder is only created if missing.
    bool privateFolder = true;
};

// ~/Library/Application Support/Overdub/control/control.sock, in a private folder.
std::string defaultSocketPath();

// The values of OVERDUB_CONTROL ("0" disables the server) and OVERDUB_CONTROL_SOCKET (a socket
// path that replaces the default), either may be null.
ServerConfig serverConfigFrom (const char* controlVariable, const char* socketVariable);

class Server
{
public:
    static constexpr std::size_t maxClients = 4;
    // Enough for a longest-allowed line and a chunk more, so an over-long line is seen in one pump.
    static constexpr std::size_t maxReadBytesPerPump = Controller::maxLineBytes + (64u << 10);

    // Binds and listens, logging control.server_listening (path). Returns null, having logged why,
    // when the config disables it (control.server_disabled, info) or it cannot start (warn:
    // control.socket_path_not_absolute, control.socket_path_too_long, control.socket_in_use,
    // control.socket_path_occupied, control.socket_folder_failed, control.socket_bind_failed).
    // The app runs on without it.
    static std::unique_ptr<Server> start (const ServerConfig& config, Controller controller);

    // Closes every client and the listening socket, and removes the socket file if it is still
    // the one this server bound.
    ~Server();

    Server (const Server&) = delete;
    Server& operator= (const Server&) = delete;

    // Accepts, reads, runs the Controller, writes and closes, without blocking. Ignored when
    // re-entered from a nested message loop inside a request's handler.
    void pump (Clock::time_point now);

private:
    struct Client
    {
        int fd = -1;
        ConnectionId connection = 0;
        pid_t peerPid = 0;
        bool finishedSending = false;
    };

    Server (int listeningFd, std::string socketPath, dev_t socketDevice, ino_t socketInode, Controller controller);
    void acceptClients();
    void close (Client& client, std::string_view reason);

    int listenFd = -1;
    std::string path;
    dev_t boundDevice = 0;
    ino_t boundInode = 0;
    Controller controller;
    std::vector<Client> clients;
    bool pumping = false;
};
} // namespace overdub::control
