#include "control/Server.h"

#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#include <memory>
#include <optional>
#include <string>

using namespace overdub::control;
using nlohmann::json;

namespace
{
// A fresh folder under /tmp: a short path, since sun_path holds 104 bytes.
struct TempSocketFolder
{
    TempSocketFolder()
        : folder ("/tmp/ovd-" + juce::Uuid().toString().substring (0, 8).toStdString())
    {
    }
    ~TempSocketFolder() { juce::File (folder).deleteRecursively(); }

    std::string socketPath() const { return folder + "/c.sock"; }

    std::string folder;
};

ServerConfig configFor (const std::string& path)
{
    return ServerConfig { .enabled = true, .socketPath = path, .privateFolder = true };
}

int modeOf (const std::string& path)
{
    struct stat info {};
    REQUIRE (::lstat (path.c_str(), &info) == 0);
    return info.st_mode & 0777;
}

bool exists (const std::string& path)
{
    struct stat info {};
    return ::lstat (path.c_str(), &info) == 0;
}

sockaddr_un addressOf (const std::string& path)
{
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    std::strncpy (address.sun_path, path.c_str(), sizeof (address.sun_path) - 1);
    return address;
}

struct Client
{
    explicit Client (const std::string& path)
        : fd (::socket (AF_UNIX, SOCK_STREAM, 0))
    {
        const int on = 1;
        ::setsockopt (fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof (on));
        const auto address = addressOf (path);
        connected = ::connect (fd, reinterpret_cast<const sockaddr*> (&address), sizeof (address)) == 0;
    }
    ~Client()
    {
        if (fd >= 0)
            ::close (fd);
    }

    void send (const std::string& text) { REQUIRE (::write (fd, text.data(), text.size()) == (ssize_t) text.size()); }

    std::optional<json> readLine (Server& server)
    {
        for (int attempt = 0; attempt < 200; ++attempt)
        {
            if (const auto end = buffer.find ('\n'); end != std::string::npos)
            {
                auto line = json::parse (buffer.substr (0, end));
                buffer.erase (0, end + 1);
                return line;
            }
            server.pump (Clock::now());
            pollfd polled { .fd = fd, .events = POLLIN, .revents = 0 };
            if (::poll (&polled, 1, 5) > 0)
            {
                char chunk[65536];
                const auto count = ::read (fd, chunk, sizeof (chunk));
                if (count <= 0)
                    return std::nullopt;
                buffer.append (chunk, (std::size_t) count);
            }
        }
        return std::nullopt;
    }

    int fd = -1;
    bool connected = false;
    std::string buffer;
};

bool logged (const overdub::core::ScopedLogCapture& capture, std::string_view event)
{
    for (const auto& line : capture.events())
        if (line["event"] == event)
            return true;
    return false;
}

json lastEvent (const overdub::core::ScopedLogCapture& capture, std::string_view event)
{
    json found;
    for (const auto& line : capture.events())
        if (line["event"] == event)
            found = line;
    return found;
}

Controller plainController()
{
    return Controller { std::make_shared<EventTap>() };
}
} // namespace

TEST_CASE ("the server listens on a 0600 socket in a 0700 folder and answers a request", "[control][server]")
{
    overdub::core::ScopedLogCapture capture;
    TempSocketFolder temp;
    REQUIRE (::mkdir (temp.folder.c_str(), 0755) == 0);
    REQUIRE (::chmod (temp.folder.c_str(), 0755) == 0);

    auto server = Server::start (configFor (temp.socketPath()), plainController());
    REQUIRE (server != nullptr);
    CHECK (modeOf (temp.folder) == 0700);
    CHECK (modeOf (temp.socketPath()) == 0600);
    CHECK (lastEvent (capture, "control.server_listening")["path"] == temp.socketPath());

    Client client { temp.socketPath() };
    REQUIRE (client.connected);
    client.send ("{\"id\":1,\"op\":\"hello\"}\n");
    const auto reply = client.readLine (*server);
    REQUIRE (reply.has_value());
    CHECK ((*reply)["id"] == 1);
    CHECK ((*reply)["ok"] == true);
    CHECK ((*reply)["pid"] == (int) ::getpid());

    const auto connected = lastEvent (capture, "control.client_connected");
    CHECK (connected["peer_pid"] == (int) ::getpid());

    server.reset();
    CHECK (! exists (temp.socketPath()));
}

TEST_CASE ("a disabled server opens nothing", "[control][server]")
{
    overdub::core::ScopedLogCapture capture;
    TempSocketFolder temp;

    const auto config = serverConfigFrom ("0", temp.socketPath().c_str());
    CHECK (! config.enabled);
    CHECK (Server::start (config, plainController()) == nullptr);
    CHECK (lastEvent (capture, "control.server_disabled")["reason"] == "env");
    CHECK (! exists (temp.folder));
}

TEST_CASE ("the environment picks the socket path, defaulting to a private folder in Application Support", "[control][server]")
{
    const auto byDefault = serverConfigFrom (nullptr, nullptr);
    CHECK (byDefault.enabled);
    CHECK (byDefault.privateFolder);
    CHECK (byDefault.socketPath.ends_with ("/Library/Application Support/Overdub/control/control.sock"));

    const auto moved = serverConfigFrom ("1", "/tmp/x/c.sock");
    CHECK (moved.enabled);
    CHECK (moved.socketPath == "/tmp/x/c.sock");
    CHECK (! moved.privateFolder);
}

TEST_CASE ("a relative OVERDUB_CONTROL_SOCKET is refused and the app goes on", "[control][server]")
{
    overdub::core::ScopedLogCapture capture;

    CHECK (Server::start (serverConfigFrom ("1", "ovd-relative/c.sock"), plainController()) == nullptr);
    const auto error = lastEvent (capture, "control.socket_path_not_absolute");
    CHECK (error["lvl"] == "warn");
    CHECK (error["path"] == "ovd-relative/c.sock");
    CHECK (! juce::File::getCurrentWorkingDirectory().getChildFile ("ovd-relative").exists());
}

TEST_CASE ("a socket path longer than sun_path is refused and the app goes on", "[control][server]")
{
    overdub::core::ScopedLogCapture capture;
    const auto path = "/tmp/" + std::string (120, 'x') + ".sock";

    CHECK (Server::start (configFor (path), plainController()) == nullptr);
    const auto error = lastEvent (capture, "control.socket_path_too_long");
    CHECK (error["lvl"] == "warn");
    CHECK (error["length"] == path.size());
}

TEST_CASE ("a stale socket file left by a crash is replaced", "[control][server]")
{
    overdub::core::ScopedLogCapture capture;
    TempSocketFolder temp;
    REQUIRE (juce::File (temp.folder).createDirectory().wasOk());
    {
        const auto stale = ::socket (AF_UNIX, SOCK_STREAM, 0);
        const auto address = addressOf (temp.socketPath());
        REQUIRE (::bind (stale, reinterpret_cast<const sockaddr*> (&address), sizeof (address)) == 0);
        ::close (stale);
    }
    REQUIRE (exists (temp.socketPath()));

    auto server = Server::start (configFor (temp.socketPath()), plainController());
    REQUIRE (server != nullptr);
    Client client { temp.socketPath() };
    REQUIRE (client.connected);
    client.send ("{\"id\":1,\"op\":\"hello\"}\n");
    CHECK (client.readLine (*server).has_value());
}

TEST_CASE ("a live socket is never taken over", "[control][server]")
{
    overdub::core::ScopedLogCapture capture;
    TempSocketFolder temp;
    auto first = Server::start (configFor (temp.socketPath()), plainController());
    REQUIRE (first != nullptr);

    CHECK (Server::start (configFor (temp.socketPath()), plainController()) == nullptr);
    CHECK (lastEvent (capture, "control.socket_in_use")["path"] == temp.socketPath());

    Client client { temp.socketPath() };
    REQUIRE (client.connected);
    client.send ("{\"id\":1,\"op\":\"hello\"}\n");
    CHECK (client.readLine (*first).has_value());
}

TEST_CASE ("a file that is not a socket at the socket path is left alone", "[control][server]")
{
    overdub::core::ScopedLogCapture capture;
    TempSocketFolder temp;
    REQUIRE (juce::File (temp.socketPath()).create().wasOk());

    CHECK (Server::start (configFor (temp.socketPath()), plainController()) == nullptr);
    CHECK (logged (capture, "control.socket_path_occupied"));
    CHECK (juce::File (temp.socketPath()).existsAsFile());
}

TEST_CASE ("a client that disconnects in the middle of a reply is dropped without a SIGPIPE", "[control][server]")
{
    overdub::core::ScopedLogCapture capture;
    TempSocketFolder temp;
    const auto big = std::string (4u << 20, 'b');
    auto server = Server::start (configFor (temp.socketPath()), Controller { std::make_shared<EventTap>(), [&big] (const Request&) -> Reply {
        return json { { "blob", big } };
    } });
    REQUIRE (server != nullptr);

    {
        Client client { temp.socketPath() };
        REQUIRE (client.connected);
        client.send ("{\"id\":1,\"op\":\"big\"}\n");
        server->pump (Clock::now());
    }
    for (int i = 0; i < 5; ++i)
        server->pump (Clock::now());

    // Which syscall notices first depends on the kernel: the read (reset) or the write (EPIPE).
    const auto reason = lastEvent (capture, "control.client_disconnected")["reason"];
    CHECK ((reason == "peer_closed" || reason == "read_failed"));
}

TEST_CASE ("a client that stops sending still gets its replies before the server closes it", "[control][server]")
{
    overdub::core::ScopedLogCapture capture;
    TempSocketFolder temp;
    auto server = Server::start (configFor (temp.socketPath()), plainController());
    REQUIRE (server != nullptr);

    Client client { temp.socketPath() };
    REQUIRE (client.connected);
    client.send ("{\"id\":1,\"op\":\"hello\"}\n");
    ::shutdown (client.fd, SHUT_WR);

    const auto reply = client.readLine (*server);
    REQUIRE (reply.has_value());
    CHECK ((*reply)["id"] == 1);
    server->pump (Clock::now());
    CHECK (lastEvent (capture, "control.client_disconnected")["reason"] == "eof");
}

TEST_CASE ("the server refuses clients past its limit", "[control][server]")
{
    overdub::core::ScopedLogCapture capture;
    TempSocketFolder temp;
    auto server = Server::start (configFor (temp.socketPath()), plainController());
    REQUIRE (server != nullptr);

    std::vector<std::unique_ptr<Client>> clients;
    for (std::size_t i = 0; i <= Server::maxClients; ++i)
        clients.push_back (std::make_unique<Client> (temp.socketPath()));
    server->pump (Clock::now());

    CHECK (lastEvent (capture, "control.client_rejected")["reason"] == "too_many_clients");
}

TEST_CASE ("one pump reads a bounded number of bytes from a client", "[control][server]")
{
    overdub::core::ScopedLogCapture capture;
    TempSocketFolder temp;
    auto server = Server::start (configFor (temp.socketPath()), plainController());
    REQUIRE (server != nullptr);

    Client client { temp.socketPath() };
    REQUIRE (client.connected);
    const int sendBuffer = 4 << 20;
    REQUIRE (::setsockopt (client.fd, SOL_SOCKET, SO_SNDBUF, &sendBuffer, sizeof (sendBuffer)) == 0);
    client.send (std::string (2u << 20, '\n') + "{\"id\":1,\"op\":\"hello\"}\n");

    server->pump (Clock::now());
    pollfd polled { .fd = client.fd, .events = POLLIN, .revents = 0 };
    CHECK (::poll (&polled, 1, 50) == 0);

    const auto reply = client.readLine (*server);
    REQUIRE (reply.has_value());
    CHECK ((*reply)["id"] == 1);
}

TEST_CASE ("a client that hangs up with a wait pending frees its slot at once", "[control][server]")
{
    overdub::core::ScopedLogCapture capture;
    TempSocketFolder temp;
    auto server = Server::start (configFor (temp.socketPath()), plainController());
    REQUIRE (server != nullptr);

    {
        Client leaver { temp.socketPath() };
        REQUIRE (leaver.connected);
        leaver.send ("{\"id\":1,\"op\":\"wait\",\"event\":\"export.finished\",\"timeout_ms\":600000}\n");
        server->pump (Clock::now());
    }
    for (int i = 0; i < 3; ++i)
        server->pump (Clock::now());
    CHECK (lastEvent (capture, "control.client_disconnected")["reason"] == "peer_closed");

    std::vector<std::unique_ptr<Client>> clients;
    for (std::size_t i = 0; i < Server::maxClients; ++i)
        clients.push_back (std::make_unique<Client> (temp.socketPath()));
    for (auto& client : clients)
    {
        client->send ("{\"id\":2,\"op\":\"hello\"}\n");
        CHECK (client->readLine (*server).has_value());
    }
    CHECK (! logged (capture, "control.client_rejected"));
}

TEST_CASE ("a client that stops sending keeps its pending wait until the event arrives", "[control][server]")
{
    overdub::core::ScopedLogCapture capture;
    TempSocketFolder temp;
    auto tap = std::make_shared<EventTap>();
    auto server = Server::start (configFor (temp.socketPath()), Controller { tap });
    REQUIRE (server != nullptr);

    Client client { temp.socketPath() };
    REQUIRE (client.connected);
    client.send ("{\"id\":1,\"op\":\"wait\",\"event\":\"export.finished\",\"timeout_ms\":600000}\n");
    ::shutdown (client.fd, SHUT_WR);
    for (int i = 0; i < 3; ++i)
        server->pump (Clock::now());
    CHECK (! logged (capture, "control.client_disconnected"));

    tap->writeLine (overdub::core::formatLogLine ({ .level = overdub::core::Level::info, .event = "export.finished" }));
    const auto reply = client.readLine (*server);
    REQUIRE (reply.has_value());
    CHECK ((*reply)["id"] == 1);
    CHECK ((*reply)["ok"] == true);
}
