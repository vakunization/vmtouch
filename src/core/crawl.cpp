// core/crawl.cpp — see crawl.h.
//
// Traversal errors go to the VISITOR (CrawlItem), not to a Session.
// Fatal cases (depth overflow, allocation failure, rlimit exhaustion)
// throw FatalError with the historical message texts.

#include "crawl.h"

#include <cerrno>
#include <cstring>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "platform.h"

namespace vmt {

constexpr int kMaxCrawlDepth = 1024;

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

    // strip a trailing slash (path is this frame's private copy)
    if (path.size() > 1 && path.back() == '/')
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
        // Loop check against the directory-inode stack.
        for (const ino_t ino : depth_stack_) {
            if (ino == sb.st_ino) {
                emit(path, Failure::Kind::CrawlLoop, 0, visitor);
                return;
            }
        }

        if (depth_stack_.size() >= kMaxCrawlDepth)
            throw FatalError("maximum directory crawl depth reached: " + path);

        total_dirs_++;   // entered (counts even if opendir fails)
        depth_stack_.push_back(sb.st_ino);

        DIR* dirp = opendir(cpath);

        if (dirp == NULL) {
            if (errno == ENFILE || errno == EMFILE) {
                const auto r = platform::raise_nofile_rlimit();
                if (r.outcome == platform::RaiseNofileResult::Outcome::Raised) {
                    dirp = opendir(cpath);   // single retry
                } else if (r.outcome == platform::RaiseNofileResult::Outcome::PermissionDenied) {
                    if (getuid() == 0 || geteuid() == 0)
                        throw FatalError("system open file limit reached");
                    throw FatalError("open file limit reached and unable to increase limit. retry as root");
                } else {
                    throw FatalError(std::string("increment_nofile_rlimit: ") +
                                     (r.outcome == platform::RaiseNofileResult::Outcome::GetrlimitFailed
                                          ? "getrlimit ("
                                          : "setrlimit (") +
                                     strerror(r.err) + ")");
                }
            }

            if (dirp == NULL) {
                emit(path, Failure::Kind::OpendirFailed, errno, visitor);
                return;
            }
        }

        while (struct dirent* de = readdir(dirp)) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;

            std::string child;
            child.reserve(path.size() + 1 + strlen(de->d_name));
            child += cpath;
            child += '/';
            child += de->d_name;

            walk_impl(child, visitor);   // recursion: child is this frame's own copy
        }

        if (closedir(dirp)) {
            emit(path, Failure::Kind::Closedir, errno, visitor);
            return;
        }

        depth_stack_.pop_back();
    } else if (S_ISREG(sb.st_mode) || S_ISBLK(sb.st_mode)) {
        visitor(CrawlItem{std::string_view(path), nullptr});   // a file item
    } else {
        emit(path, Failure::Kind::NotRegular, 0, visitor);
    }
}

} // namespace vmt
