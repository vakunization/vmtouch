// core/crawl.cpp — see crawl.h.
//
// Traversal errors go to the VISITOR (CrawlItem), not to a Session.
// Fatal cases (depth overflow, allocation failure, rlimit exhaustion)
// throw FatalError with the historical message texts.

#include "crawl.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "platform.h"

namespace vmt {
namespace {

constexpr int kMaxCrawlDepth = 1024;

} // namespace

Crawler::Crawler() = default;

Crawler::~Crawler() = default;

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
        const Failure f{Failure::Kind::Stat, errno, path, 0};
        visitor(CrawlItem{path, &f});
        return;
    }

    if (S_ISLNK(sb.st_mode)) {
        const Failure f{Failure::Kind::Symlink, 0, path, 0};
        visitor(CrawlItem{path, &f});
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
        for (int i = 0; i < state_.curr_crawl_depth; i++) {
            if (state_.crawl_inodes[i] == sb.st_ino) {
                const Failure f{Failure::Kind::CrawlLoop, 0, path, 0};
                visitor(CrawlItem{path, &f});
                return;
            }
        }

        if (state_.curr_crawl_depth == kMaxCrawlDepth)
            throw FatalError("maximum directory crawl depth reached: " + path);

        state_.total_dirs++;   // entered (counts even if opendir fails)

        state_.crawl_inodes[state_.curr_crawl_depth] = sb.st_ino;

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
                const Failure f{Failure::Kind::OpendirFailed, errno, path, 0};
                visitor(CrawlItem{path, &f});
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

            state_.curr_crawl_depth++;
            walk_impl(child, visitor);   // recursion: child is this frame's own copy
            state_.curr_crawl_depth--;
        }

        if (closedir(dirp)) {
            const Failure f{Failure::Kind::Closedir, errno, path, 0};
            visitor(CrawlItem{path, &f});
            return;
        }
    } else if (S_ISREG(sb.st_mode) || S_ISBLK(sb.st_mode)) {
        visitor(CrawlItem{std::string_view(path), nullptr});   // a file item
    } else {
        const Failure f{Failure::Kind::NotRegular, 0, path, 0};
        visitor(CrawlItem{path, &f});
    }
}

} // namespace vmt
