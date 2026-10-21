#include "control/Server.h"

#include <juce_core/juce_core.h>

#include <fcntl.h>
#include <libproc.h>
#include <poll.h>
#include <sys/event.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <optional>
#include <utility>

namespace overdub::control
{
namespace
{
struct BoundSocket
{
    int fd = -1;
    dev_t device = 0;
    ino_t inode = 0;
};

core::Error socketError (std::string code, std::string message, const std::string& path, int errorNumber = 0)
{
    core::Fields fields { { "path", path } };
    if (errorNumber != 0)
        fields["errno"] = errorNumber;
    return core::Error { std::move (code), std::move (message), std::move (fields) };
}

void makeNonBlocking (int fd)
{
    ::fcntl (fd, F_SETFL, ::fcntl (fd, F_GETFL) | O_NONBLOCK);
    ::fcntl (fd, F_SETFD, FD_CLOEXEC);
    const int on = 1;
    ::setsockopt (fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof (on));
}

sockaddr_un addressOf (const std::string& path)
{
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    std::memcpy (address.sun_path, path.c_str(), path.size() + 1);
    return address;
}

core::Expected<void> prepareFolder (const std::string& socketPath, bool privateFolder)
{
    const auto folder = juce::File (socketPath).getParentDirectory();
    const auto existed = folder.isDirectory();
    if (! existed && ! folder.createDirectory().wasOk())
        return std::unexpected (socketError ("control.socket_folder_failed", "Could not create the socket's folder", socketPath, errno));
    if ((privateFolder || ! existed) && ::chmod (folder.getFullPathName().toRawUTF8(), 0700) != 0)
        return std::unexpected (socketError ("control.socket_folder_failed", "Could not make the socket's folder private", socketPath, errno));
    return {};
}

// A live socket belongs to another app instance and is never taken over.
core::Expected<void> removeStaleSocket (const std::string& path)
{
    struct stat info {};
    if (::lstat (path.c_str(), &info) != 0)
        return {};
    if (! S_ISSOCK (info.st_mode))
        return std::unexpected (socketError ("control.socket_path_occupied", "Something other than a socket is at the socket path", path));

    const auto probe = ::socket (AF_UNIX, SOCK_STREAM, 0);
    if (probe < 0)
        return std::unexpected (socketError ("control.socket_bind_failed", "Could not create a socket", path, errno));
    const int on = 1;
    ::setsockopt (probe, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof (on));
    const auto address = addressOf (path);
    const auto connected = ::connect (probe, reinterpret_cast<const sockaddr*> (&address), sizeof (address));
    const auto connectErrno = errno;
    ::close (probe);

    if (connected == 0)
        return std::unexpected (socketError ("control.socket_in_use", "Another app is listening on the socket", path));
    if (connectErrno != ECONNREFUSED)
        return std::unexpected (socketError ("control.socket_in_use", "Could not tell whether the socket is in use", path, connectErrno));
    if (::unlink (path.c_str()) != 0)
        return std::unexpected (socketError ("control.socket_bind_failed", "Could not remove the stale socket", path, errno));
    return {};
}

core::Expected<BoundSocket> bindSocket (const ServerConfig& config)
{
    const auto& path = config.socketPath;
    if (! path.starts_with ('/'))
        return std::unexpected (socketError ("control.socket_path_not_absolute", "The socket path must be absolute", path));
    if (path.size() >= sizeof (sockaddr_un::sun_path))
        return std::unexpected (core::Error { "control.socket_path_too_long",
                                              "The socket path does not fit a Unix socket address",
                                              { { "path", path }, { "length", path.size() }, { "max", sizeof (sockaddr_un::sun_path) - 1 } } });
    if (auto folder = prepareFolder (path, config.privateFolder); ! folder.has_value())
        return std::unexpected (folder.error());
    if (auto stale = removeStaleSocket (path); ! stale.has_value())
        return std::unexpected (stale.error());

    const auto fd = ::socket (AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return std::unexpected (socketError ("control.socket_bind_failed", "Could not create a socket", path, errno));
    makeNonBlocking (fd);

    const auto address = addressOf (path);
    struct stat info {};
    if (::bind (fd, reinterpret_cast<const sockaddr*> (&address), sizeof (address)) != 0
        || ::chmod (path.c_str(), 0600) != 0
        || ::listen (fd, 8) != 0
        || ::stat (path.c_str(), &info) != 0)
    {
        const auto failure = errno;
        ::close (fd);
        return std::unexpected (socketError ("control.socket_bind_failed", "Could not listen on the socket", path, failure));
    }
    return BoundSocket { .fd = fd, .device = info.st_dev, .inode = info.st_ino };
}

enum class ReadResult
{
    open,
    finished, // the peer will send nothing more but may still read
    failed
};

ReadResult readAvailable (int fd, std::string& into, std::size_t limit)
{
    char chunk[65536];
    while (into.size() < limit)
    {
        const auto count = ::read (fd, chunk, std::min (sizeof (chunk), limit - into.size()));
        if (count > 0)
        {
            into.append (chunk, (std::size_t) count);
            continue;
        }
        if (count == 0)
            return ReadResult::finished;
        if (errno == EINTR)
            continue;
        return errno == EAGAIN || errno == EWOULDBLOCK ? ReadResult::open : ReadResult::failed;
    }
    return ReadResult::open;
}

// Whether a peer that finished sending has also stopped reading. poll reports POLLHUP for both, so
// this asks kqueue: EV_EOF on the write filter means nothing written can reach the peer.
bool peerHungUp (int fd)
{
    const auto queue = ::kqueue();
    if (queue < 0)
        return false;
    struct kevent change {};
    EV_SET (&change, fd, EVFILT_WRITE, EV_ADD, 0, 0, nullptr);
    struct kevent result {};
    const timespec noWait {};
    const auto count = ::kevent (queue, &change, 1, &result, 1, &noWait);
    ::close (queue);
    return count == 1 && (result.flags & (EV_EOF | EV_ERROR)) != 0;
}

// pid 0 is a peer that hung up before accept, whose pid the kernel no longer gives.
std::string processName (pid_t pid)
{
    if (pid <= 0)
        return "unknown process";
    char name[2 * MAXCOMLEN + 1] {};
    if (::proc_name (pid, name, sizeof (name)) > 0)
        return name;
    return "pid " + std::to_string (pid);
}
} // namespace

std::string defaultSocketPath()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("Application Support")
        .getChildFile ("Overdub")
        .getChildFile ("control")
        .getChildFile ("control.sock")
        .getFullPathName()
        .toStdString();
}

ServerConfig serverConfigFrom (const char* controlVariable, const char* socketVariable)
{
    ServerConfig config;
    config.enabled = controlVariable == nullptr || std::string_view (controlVariable) != "0";
    if (socketVariable != nullptr && *socketVariable != '\0')
    {
        config.socketPath = socketVariable;
        config.privateFolder = false;
    }
    else
    {
        config.socketPath = defaultSocketPath();
        config.privateFolder = true;
    }
    return config;
}

std::unique_ptr<Server> Server::start (const ServerConfig& config, Controller controller)
{
    if (! config.enabled)
    {
        core::logEvent (core::Level::info, "control.server_disabled", { { "reason", "env" } });
        return nullptr;
    }
    auto bound = bindSocket (config);
    if (! bound.has_value())
    {
        core::logError (bound.error(), core::Level::warn);
        return nullptr;
    }
    core::logEvent (core::Level::info, "control.server_listening", { { "path", config.socketPath } });
    return std::unique_ptr<Server> (new Server (bound->fd, config.socketPath, bound->device, bound->inode, std::move (controller)));
}

Server::Server (int listeningFd, std::string socketPath, dev_t socketDevice, ino_t socketInode, Controller ownController)
    : listenFd (listeningFd),
      path (std::move (socketPath)),
      boundDevice (socketDevice),
      boundInode (socketInode),
      controller (std::move (ownController))
{
}

Server::~Server()
{
    for (auto& client : clients)
        close (client, "server_stopped");
    ::close (listenFd);

    struct stat info {};
    if (::lstat (path.c_str(), &info) == 0 && info.st_dev == boundDevice && info.st_ino == boundInode)
        ::unlink (path.c_str());
}

void Server::acceptClients()
{
    while (true)
    {
        const auto fd = ::accept (listenFd, nullptr, nullptr);
        if (fd < 0)
        {
            if (errno == EINTR)
                continue;
            return;
        }
        makeNonBlocking (fd);

        uid_t uid = 0;
        gid_t gid = 0;
        if (::getpeereid (fd, &uid, &gid) != 0 || uid != ::geteuid())
        {
            core::logEvent (core::Level::warn, "control.client_rejected", { { "reason", "other_user" }, { "uid", uid } });
            ::close (fd);
            continue;
        }
        if (clients.size() >= maxClients)
        {
            core::logEvent (core::Level::warn, "control.client_rejected", { { "reason", "too_many_clients" }, { "max_clients", maxClients } });
            ::close (fd);
            continue;
        }

        pid_t peerPid = 0;
        socklen_t length = sizeof (peerPid);
        ::getsockopt (fd, SOL_LOCAL, LOCAL_PEERPID, &peerPid, &length);

        const auto connection = controller.connect();
        auto process = processName (peerPid);
        core::logEvent (core::Level::info, "control.client_connected", { { "client", connection }, { "peer_pid", peerPid }, { "process", process } });
        clients.push_back (Client { .fd = fd, .connection = connection, .peerPid = peerPid, .process = std::move (process) });
    }
}

void Server::pump (Clock::time_point now)
{
    if (pumping)
        return;
    pumping = true;

    acceptClients();

    std::vector<pollfd> polled;
    for (const auto& client : clients)
    {
        const auto reading = ! client.finishedSending && ! controller.closeReason (client.connection).has_value();
        polled.push_back (pollfd { .fd = reading ? client.fd : -1, .events = POLLIN, .revents = 0 });
    }
    if (! polled.empty())
        ::poll (polled.data(), (nfds_t) polled.size(), 0);

    std::vector<std::optional<std::string>> closing (clients.size());
    for (std::size_t i = 0; i < clients.size(); ++i)
    {
        if ((polled[i].revents & (POLLIN | POLLHUP | POLLERR)) == 0)
            continue;
        std::string bytes;
        const auto result = readAvailable (clients[i].fd, bytes, maxReadBytesPerPump);
        controller.receive (clients[i].connection, bytes, now);
        if (result == ReadResult::finished)
            clients[i].finishedSending = true;
        else if (result == ReadResult::failed)
            closing[i] = "read_failed";
    }

    for (std::size_t i = 0; i < clients.size(); ++i)
        if (! closing[i].has_value() && clients[i].finishedSending && peerHungUp (clients[i].fd))
            closing[i] = "peer_closed";

    controller.pump (now);

    for (std::size_t i = 0; i < clients.size(); ++i)
    {
        auto& client = clients[i];
        for (auto output = controller.pendingOutput (client.connection); ! closing[i].has_value() && ! output.empty();
             output = controller.pendingOutput (client.connection))
        {
            const auto written = ::write (client.fd, output.data(), output.size());
            if (written > 0)
                controller.consumeOutput (client.connection, (std::size_t) written);
            else if (written < 0 && errno == EINTR)
                continue;
            else if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                break;
            else
                closing[i] = "peer_closed";
        }

        if (closing[i].has_value() || ! controller.pendingOutput (client.connection).empty())
            continue;
        if (const auto reason = controller.closeReason (client.connection))
            closing[i] = std::string (closeReasonName (*reason));
        else if (client.finishedSending && ! controller.hasPendingWork (client.connection))
            closing[i] = "eof";
    }

    std::erase_if (departedClients, [now] (const DepartedClient& departed) { return now - departed.leftAt >= departedClientLinger; });
    for (std::size_t i = 0; i < clients.size(); ++i)
        if (closing[i].has_value())
        {
            close (clients[i], *closing[i]);
            departedClients.push_back (DepartedClient { .process = clients[i].process, .leftAt = now });
        }
    for (std::size_t i = clients.size(); i-- > 0;)
        if (closing[i].has_value())
            clients.erase (clients.begin() + (std::ptrdiff_t) i);

    pumping = false;
}

std::vector<std::string> Server::clientProcesses (Clock::time_point now) const
{
    std::vector<std::string> processes;
    for (const auto& client : clients)
        processes.push_back (client.process);
    for (const auto& departed : departedClients)
        if (now - departed.leftAt < departedClientLinger)
            processes.push_back (departed.process);
    return processes;
}

void Server::close (Client& client, std::string_view reason)
{
    controller.disconnect (client.connection);
    ::close (client.fd);
    core::logEvent (core::Level::info,
                    "control.client_disconnected",
                    { { "client", client.connection }, { "peer_pid", client.peerPid }, { "reason", reason } });
}
} // namespace overdub::control
