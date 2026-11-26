// core/crawl.cpp — see crawl.h.
//
// Traversal errors go to the VISITOR (CrawlItem), not to a Session.
// Fatal cases (depth overflow, allocation failure, rlimit exhaustion)
// throw FatalError with the historical message texts.

#include "crawl.h"

#include <cerrno>
#include <memory>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "platform.h"

namespace vmt {
namespace {

constexpr int kMaxCrawlDepth = 1024;

struct CloseDir {
    void operator()(DIR* dir) const noexcept { (void)::closedir(dir); }
};

using DirHandle = std::unique_ptr<DIR, CloseDir>;

} // namespace

void Crawler::emit(const std::string& path, Failure::Kind kind, int32_t err,
                   const CrawlVisitor& visitor) {
    const Failure f{kind, err, path, 0};
    visitor(CrawlItem{path, &f});
}

void Crawler::walk(std::string path, const CrawlVisitor& visitor) {
    walk_impl(path, visitor);
}

void Crawler::walk_impl(std::string& path, const CrawlVisitor& visitor) {
    struct stat sb;

    // Strip trailing separators so lstat still inspects a symlink itself.
    while (path.size() > 1 && path.back() == '/')
        path.pop_back();
    const char* cpath = path.c_str();

    int res = lstat(cpath, &sb);   // fixed policy: never follow symlinks

    if (res) {
        emit(path, Failure::Kind::Stat, errno, visitor);
        return;
    }

    if (S_ISLNK(sb.st_mode)) {
        emit(path, Failure::Kind::Symlink, 0, visitor);
        return;
    }

    // Hardlink dedup: without this, files copied as hardlinks (cp -al)
    // are counted (and processed) once per link.
    if (sb.st_nlink > 1) {
        const dev_and_inode key{sb.st_dev, sb.st_ino};
        if (!seen_inodes_.insert(key).second)
            return;   // already seen this (dev, ino)
    }

    if (S_ISDIR(sb.st_mode)) {
        // Loop check against the directory-inode stack. A linear scan is
        // deliberate: depth is bounded by kMaxCrawlDepth (1024), so an
        // unordered_set would cost more memory and cache misses than the
        // scan costs comparisons.
        for (const ino_t ino : depth_stack_) {
            if (ino == sb.st_ino) {
                emit(path, Failure::Kind::CrawlLoop, 0, visitor);
                return;
            }
        }

        if (depth_stack_.size() >= kMaxCrawlDepth)
            throw FatalError("maximum directory crawl depth reached: " + path);

        total_dirs_++;   // entered (counts even if opendir fails)

        // RAII guard: pop the inode no matter how this directory's scope
        // ends (opendir failure, closedir failure, early return, exception).
        // Without it an early return leaks the inode onto the stack and
        // corrupts loop detection for every subsequent directory.
        depth_stack_.push_back(sb.st_ino);
        struct DepthPop {
            std::vector<ino_t>& stack;
            ~DepthPop() { stack.pop_back(); }
        } depth_pop{depth_stack_};

        DirHandle dir(opendir(cpath));

        if (!dir) {
            if (errno == ENFILE || errno == EMFILE) {
                const auto r = platform::raise_nofile_rlimit();
                if (r.outcome != platform::RaiseNofileResult::Outcome::Raised)
                    throw FatalError(platform::nofile_fatal_message(r));
                dir.reset(opendir(cpath));   // single retry
            }

            if (!dir) {
                emit(path, Failure::Kind::OpendirFailed, errno, visitor);
                return;
            }
        }

        while (const auto* de = readdir(dir.get())) {
            const std::string_view name(de->d_name);
            if (name == "." || name == "..")
                continue;

            std::string child;
            child.reserve(path.size() + 1 + name.size());
            child += path;
            if (!path.ends_with('/'))
                child += '/';
            child += name;

            walk_impl(child, visitor);   // recursion: child is this frame's own copy
        }

        // Report close errors on the normal path; the owner handles unwinding.
        if (closedir(dir.release())) {
            emit(path, Failure::Kind::Closedir, errno, visitor);
        }
    } else if (S_ISREG(sb.st_mode) || S_ISBLK(sb.st_mode)) {
        visitor(CrawlItem{std::string_view(path), nullptr});   // a file item
    } else {
        emit(path, Failure::Kind::NotRegular, 0, visitor);
    }
}

} // namespace vmt
