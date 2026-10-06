#include <gtest/gtest.h>

#include "nix/util/file-descriptor.hh"
#include "nix/util/processes.hh"

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

namespace nix {

static bool isOpen(int fd)
{
    return ::fcntl(fd, F_GETFD) != -1 || errno != EBADF;
}

/**
 * Return the highest descriptor number `dup2` accepts, by binary search
 * from @p probeFrom (which must be open).
 *
 * On macOS `setrlimit` accepts soft limits above `kern.maxfilesperproc`, so
 * `sysconf(_SC_OPEN_MAX)` (the bound of the old close() loop) can be far
 * above the highest usable descriptor.
 */
static int highestUsableFD(int probeFrom)
{
    long limit = ::sysconf(_SC_OPEN_MAX);
    int good = probeFrom;
    long bad = limit > 0 && limit < (1L << 30) ? limit : (1L << 30);
    while (bad - good > 1) {
        int mid = good + int((bad - good) / 2);
        if (::dup2(probeFrom, mid) == -1) {
            bad = mid;
        } else {
            ::close(mid);
            good = mid;
        }
    }
    return good;
}

static constexpr int skipExitCode = 77;

/**
 * In a child with the open-files soft limit set to @p softLimit, open
 * descriptors at the highest usable numbers, call closeExtraFDs, and check
 * that exactly stdio survives.
 */
static void checkHighDescriptors(rlim_t softLimit)
{
    using namespace nix::unix;

    Pipe pipe;
    pipe.create();
    Pid pid = startProcess([&]() {
        struct rlimit lim;
        if (::getrlimit(RLIMIT_NOFILE, &lim) == -1)
            _exit(2);
        /* Where the hard limit is lower (typical on Linux), use it. */
        lim.rlim_cur = softLimit == RLIM_INFINITY ? softLimit : std::min(softLimit, lim.rlim_max);
        if (lim.rlim_cur > lim.rlim_max || ::setrlimit(RLIMIT_NOFILE, &lim) == -1)
            _exit(skipExitCode);

        int highest = highestUsableFD(pipe.writeSide.get());
        if (highest < 4096)
            _exit(skipExitCode);

        /* A few far apart, plus a run longer than one listing buffer. */
        std::vector<int> doomed = {highest, highest / 2, 40};
        for (int fd = 1000; fd < 2000; ++fd)
            doomed.push_back(fd);
        for (int fd : doomed)
            if (::dup2(pipe.writeSide.get(), fd) == -1)
                _exit(4);

        auto start = std::chrono::steady_clock::now();
        closeExtraFDs();
        auto elapsed = std::chrono::steady_clock::now() - start;

        for (int fd : doomed)
            if (isOpen(fd))
                _exit(5);
        for (int fd : {pipe.readSide.get(), pipe.writeSide.get()})
            if (isOpen(fd))
                _exit(6);
        for (int fd : {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO})
            if (!isOpen(fd))
                _exit(7);

        std::fprintf(
            stderr,
            "closeExtraFDs: sysconf(_SC_OPEN_MAX) %ld, highest fd %d: %.3f ms\n",
            ::sysconf(_SC_OPEN_MAX),
            highest,
            std::chrono::duration<double, std::milli>(elapsed).count());
        _exit(0);
    });

    ASSERT_NE(pid_t(pid), -1);
    int status = pid.wait();
    if (WIFEXITED(status) && WEXITSTATUS(status) == skipExitCode)
        GTEST_SKIP() << "cannot raise RLIMIT_NOFILE enough (wanted soft limit " << softLimit << ")";
    ASSERT_TRUE(statusOk(status)) << "child exit status " << status;
}

/* The soft limit launchd gives the nix-daemon on macOS, or the hard limit if lower. */
TEST(closeExtraFDs, highDescriptorsAtDaemonLimit)
{
    checkHighDescriptors(rlim_t{1} << 20);
}

/* sysconf(_SC_OPEN_MAX) then returns a value that does not fit in an int. */
TEST(closeExtraFDs, highDescriptorsAtUnlimitedLimit)
{
    checkHighDescriptors(RLIM_INFINITY);
}

} // namespace nix
