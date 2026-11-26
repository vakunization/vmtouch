// tests/unit/test_file_handle.cpp — behavior tests for vmt::FileHandle
// and the vmt::Result<T> carrier (B1).
//
// scenario: S1/S2 open by path (failure -> Failure record, no throw);
// S3 never uses FileHandle (caller-owned fd). The open logic pinned here
// was transplanted verbatim from Session::process_file — run.sh pins the
// end-to-end behavior (missing path warns "unable to open" + exit 0).

#include "test.h"
#include "helpers.h"

#include "core/crawl.h"
#include "core/file_handle.h"
#include "core/result.h"

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>   // chmod
#include <sys/wait.h>
#include <unistd.h>

// ---------------------------------------------------------------------------

TEST(file_handle_open_success_owns_fd) {
    unit::TempFile f(1);
    auto r = vmt::FileHandle::open(f.path);

    EXPECT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().fd() >= 0);
    EXPECT_TRUE(r.value().fd() != f.fd);   // a fresh descriptor, not the test's

    // The fd is valid: fcntl succeeds on it.
    EXPECT_TRUE(fcntl(r.value().fd(), F_GETFD) != -1);
}

TEST(file_handle_close_on_destruction) {
    unit::TempFile f(1);
    int fd = -1;
    {
        auto r = vmt::FileHandle::open(f.path);
        EXPECT_TRUE(r.is_ok());
        fd = r.value().fd();
    }
    // After the handle is gone the descriptor must be closed.
    EXPECT_TRUE(fcntl(fd, F_GETFD) == -1);
    EXPECT_EQ(errno, EBADF);
}

TEST(file_handle_move_transfers_ownership) {
    unit::TempFile f(1);
    auto r = vmt::FileHandle::open(f.path);
    EXPECT_TRUE(r.is_ok());
    const int fd = r.value().fd();

    vmt::FileHandle moved = std::move(r.value());
    EXPECT_EQ(moved.fd(), fd);
    EXPECT_TRUE(fcntl(moved.fd(), F_GETFD) != -1);

    // r.value() still exists but its fd was zeroed by the move; using the
    // moved-from handle must not double-close. (The moved-from handle is
    // documented as fd_ == -1.)
}

TEST(file_handle_open_missing_path_is_error_not_throw) {
    vmt::Result<vmt::FileHandle> r = vmt::FileHandle::open("/tmp/vmtouch-no-such-file-evar");

    EXPECT_TRUE(r.is_error());
    EXPECT_EQ((int)r.error().kind, (int)Failure::Kind::Open);
    EXPECT_EQ(r.error().err, ENOENT);
    EXPECT_EQ(r.error().path, "/tmp/vmtouch-no-such-file-evar");
    EXPECT_EQ(r.error().offset, 0);
}

TEST(file_handle_open_unreadable_file_error_enoent_style) {
    if (getuid() == 0) {
        SKIP("running as root");
    }
    unit::TempFile f(1);
    EXPECT_EQ(chmod(f.path.c_str(), 0000), 0);
    auto r = vmt::FileHandle::open(f.path);
    chmod(f.path.c_str(), 0644);

    EXPECT_TRUE(r.is_error());
    EXPECT_EQ((int)r.error().kind, (int)Failure::Kind::Open);
    EXPECT_EQ(r.error().err, EACCES);
}

TEST(file_and_directory_open_stop_at_hard_descriptor_limit) {
#ifdef __SANITIZE_ADDRESS__
    // The sanitizer runtime can try to create its own pipe while the
    // process is out of descriptors. Exercise real exhaustion in plain CI.
    SKIP("ASan runtime itself needs spare descriptors");
#endif
    unit::TempFile file(1);
    const pid_t child = ::fork();
    EXPECT_TRUE(child >= 0);
    if (child < 0)
        return;
    if (child == 0) {
        ::alarm(5);   // A regression must fail the test, not hang the suite.
        const struct rlimit limit{32, 32};
        if (::setrlimit(RLIMIT_NOFILE, &limit) != 0)
            _exit(2);
        std::array<int, 32> occupied{};
        size_t count = 0;
        const int result = [&] {
            while (count < occupied.size()) {
                const int fd = ::open("/dev/null", O_RDONLY);
                if (fd < 0)
                    break;
                occupied[count++] = fd;
            }
            if (count == occupied.size() || errno != EMFILE)
                return 3;
            try {
                auto opened = vmt::FileHandle::open(file.path);
                (void)opened;
                return 4;
            } catch (const FatalError&) {
            }
            try {
                vmt::Crawler crawler;
                crawler.walk(".", [](const vmt::CrawlItem&) {});
                return 5;
            } catch (const FatalError&) {
                return 0;
            }
        }();
        // Sanitizer exit hooks may need descriptors of their own.
        while (count > 0) {
            ::close(occupied[--count]);
        }
        ::alarm(0);
        _exit(result);
    }
    int status = 0;
    EXPECT_EQ(::waitpid(child, &status, 0), child);
    EXPECT_TRUE(WIFEXITED(status));
    if (WIFEXITED(status))
        EXPECT_EQ(WEXITSTATUS(status), 0);
}

// ---------------------------------------------------------------------------

TEST(result_ok_carries_value) {
    auto r = vmt::Result<int>::Ok(42);
    EXPECT_TRUE(r.is_ok());
    EXPECT_FALSE(r.is_error());
    EXPECT_EQ(r.value(), 42);
}

TEST(result_error_carries_failure) {
    auto r = vmt::Result<int>::Error(Failure{Failure::Kind::Map, EINVAL, "/p", 7});
    EXPECT_TRUE(r.is_error());
    EXPECT_EQ((int)r.error().kind, (int)Failure::Kind::Map);
    EXPECT_EQ(r.error().err, EINVAL);
    EXPECT_EQ(r.error().path, "/p");
    EXPECT_EQ(r.error().offset, 7);
}

TEST(result_void_ok_and_error) {
    auto ok = vmt::Result<void>::Ok();
    EXPECT_TRUE(ok.is_ok());

    auto err = vmt::Result<void>::Error(Failure{Failure::Kind::Open, ENOENT, "/x", 0});
    EXPECT_TRUE(err.is_error());
    EXPECT_EQ(err.error().err, ENOENT);
}
