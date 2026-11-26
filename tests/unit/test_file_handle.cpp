// tests/unit/test_file_handle.cpp — behavior tests for vmt::FileHandle
// and the vmt::Result<T> carrier (B1).
//
// scenario: S1/S2 open by path (failure -> Failure record, no throw);
// S3 never uses FileHandle (caller-owned fd). The open logic pinned here
// was transplanted verbatim from Session::process_file — run.sh pins the
// end-to-end behavior (missing path warns "unable to open" + exit 0).

#include "test.h"
#include "helpers.h"

#include "core/file_handle.h"
#include "core/result.h"

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>   // chmod
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
