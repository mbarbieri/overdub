#include "song/SeparationHelper.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace overdub::song
{
namespace
{
// Reads whatever a non-blocking pipe holds now, appending it to `buffer`. Returns false once the
// pipe is at its end (the writer closed it).
bool drainPipe (int fd, std::string& buffer)
{
    char chunk[4096];
    while (true)
    {
        const auto count = ::read (fd, chunk, sizeof (chunk));
        if (count > 0)
        {
            buffer.append (chunk, (std::size_t) count);
            continue;
        }
        if (count == 0)
            return false;
        if (errno == EINTR)
            continue;
        return errno == EAGAIN || errno == EWOULDBLOCK;
    }
}

// Moves the complete lines out of `buffer` (without their '\n'); a partial last line stays.
std::vector<std::string> takeCompleteLines (std::string& buffer)
{
    std::vector<std::string> lines;
    std::size_t start = 0;
    for (auto end = buffer.find ('\n'); end != std::string::npos; end = buffer.find ('\n', start))
    {
        lines.push_back (buffer.substr (start, end - start));
        start = end + 1;
    }
    buffer.erase (0, start);
    return lines;
}

class HelperProcess final : public SeparationHelperRun
{
public:
    HelperProcess (pid_t childPid, int stdoutPipe, int stderrPipe)
        : pid (childPid),
          stdoutFd (stdoutPipe),
          stderrFd (stderrPipe)
    {
    }

    ~HelperProcess() override
    {
        kill();
        ::close (stdoutFd);
        ::close (stderrFd);
    }

    HelperOutput poll() override
    {
        HelperOutput output;
        if (exitReported)
            return output;
        drainPipe (stdoutFd, stdoutText);
        drainPipe (stderrFd, stderrText);
        output.lines = takeCompleteLines (stdoutText);

        int status = 0;
        if (! reaped)
        {
            const auto waited = ::waitpid (pid, &status, WNOHANG);
            if (waited != pid)
                return output;
            reaped = true;
            exit = exitFrom (status);
        }
        // The child is gone: everything it wrote is in the pipes now.
        drainPipe (stdoutFd, stdoutText);
        drainPipe (stderrFd, stderrText);
        for (auto& line : takeCompleteLines (stdoutText))
            output.lines.push_back (std::move (line));
        if (! stdoutText.empty())
            output.lines.push_back (std::exchange (stdoutText, {}));
        exit.errorLine = juce::String (stderrText).trim().toStdString();
        output.exit = exit;
        exitReported = true;
        return output;
    }

    void kill() override
    {
        if (reaped)
            return;
        ::kill (pid, SIGKILL);
        int status = 0;
        while (::waitpid (pid, &status, 0) == -1 && errno == EINTR)
        {
        }
        reaped = true;
        exit = exitFrom (status);
    }

private:
    static HelperExit exitFrom (int status)
    {
        if (WIFSIGNALED (status))
            return HelperExit { .exitCode = -1, .signal = WTERMSIG (status) };
        return HelperExit { .exitCode = WEXITSTATUS (status) };
    }

    pid_t pid;
    int stdoutFd;
    int stderrFd;
    std::string stdoutText;
    std::string stderrText;
    bool reaped = false;
    bool exitReported = false;
    HelperExit exit;
};

core::Error startFailed (const juce::File& executable, int error)
{
    return core::Error { "song.separation_start_failed",
                         "the Separation helper could not be started",
                         { { "path", executable.getFullPathName().toStdString() }, { "errno", error } } };
}

core::Expected<std::unique_ptr<SeparationHelperRun>> startHelperProcess (const juce::File& executable, const juce::File& model, const juce::File& input, const juce::File& outputFolder)
{
    int stdoutPipe[2] = { -1, -1 };
    int stderrPipe[2] = { -1, -1 };
    if (::pipe (stdoutPipe) != 0)
        return std::unexpected (startFailed (executable, errno));
    if (::pipe (stderrPipe) != 0)
    {
        const auto error = errno;
        ::close (stdoutPipe[0]);
        ::close (stdoutPipe[1]);
        return std::unexpected (startFailed (executable, error));
    }
    for (const auto fd : { stdoutPipe[0], stderrPipe[0] })
    {
        ::fcntl (fd, F_SETFL, ::fcntl (fd, F_GETFL) | O_NONBLOCK);
        ::fcntl (fd, F_SETFD, FD_CLOEXEC);
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init (&actions);
    posix_spawn_file_actions_adddup2 (&actions, stdoutPipe[1], 1);
    posix_spawn_file_actions_adddup2 (&actions, stderrPipe[1], 2);
    posix_spawn_file_actions_addclose (&actions, stdoutPipe[0]);
    posix_spawn_file_actions_addclose (&actions, stderrPipe[0]);

    std::vector<std::string> arguments {
        executable.getFullPathName().toStdString(),
        model.getFullPathName().toStdString(),
        input.getFullPathName().toStdString(),
        outputFolder.getFullPathName().toStdString(),
    };
    std::vector<char*> argv;
    for (auto& argument : arguments)
        argv.push_back (argument.data());
    argv.push_back (nullptr);

    pid_t pid = 0;
    const auto spawned = ::posix_spawn (&pid, arguments[0].c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy (&actions);
    ::close (stdoutPipe[1]);
    ::close (stderrPipe[1]);
    if (spawned != 0)
    {
        ::close (stdoutPipe[0]);
        ::close (stderrPipe[0]);
        return std::unexpected (startFailed (executable, spawned));
    }
    return std::make_unique<HelperProcess> (pid, stdoutPipe[0], stderrPipe[0]);
}
} // namespace

StartSeparationHelper separationHelperProcess (juce::File executable)
{
    return [executable] (const juce::File& model, const juce::File& input, const juce::File& outputFolder) {
        return startHelperProcess (executable, model, input, outputFolder);
    };
}

juce::File defaultSeparationHelperFile()
{
    return juce::File::getSpecialLocation (juce::File::currentExecutableFile).getSiblingFile ("overdub-separate");
}

juce::File defaultSeparationModelFile()
{
    const auto* overridden = std::getenv ("OVERDUB_MODELS_DIR");
    const auto folder = overridden != nullptr && *overridden != '\0'
                          ? juce::File::getCurrentWorkingDirectory().getChildFile (juce::String::fromUTF8 (overridden))
                          : juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                                .getChildFile ("Library")
                                .getChildFile ("Application Support")
                                .getChildFile ("Overdub")
                                .getChildFile ("models");
    return folder.getChildFile ("bs_roformer_sw.fp32.onnx");
}

std::string separationModelName (const juce::File& model)
{
    return model.getFileNameWithoutExtension().toStdString();
}
} // namespace overdub::song
