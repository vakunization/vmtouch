// core/crawl.cpp — see crawl.h.

#include "crawl.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <dirent.h>
#include <libgen.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#include "platform.h"

namespace {

constexpr int kMaxCrawlDepth = 1024;

// compare device and inode information (verbatim from the monolith)
int compare_func(const void* p1, const void* p2) {
    const struct dev_and_inode* kp1 = static_cast<const struct dev_and_inode*>(p1);
    const struct dev_and_inode* kp2 = static_cast<const struct dev_and_inode*>(p2);
    int cmp1;
    cmp1 = (kp1->ino > kp2->ino) - (kp1->ino < kp2->ino);
    if (cmp1 != 0)
        return cmp1;
    return (kp1->dev > kp2->dev) - (kp1->dev < kp2->dev);
}

void free_dev_and_inode(void* p) {
    free(p);
}

} // namespace

Crawler::Crawler(Session& session) : session_(session) {}

Crawler::~Crawler() {
    // Frees the seen-inodes tree (upstream leak, originally fixed via
    // atexit in the CLI; now owned properly by the state holder).
    if (state_.seen_inodes) {
        tdestroy(state_.seen_inodes, free_dev_and_inode);
        state_.seen_inodes = nullptr;
    }
}

void Crawler::walk(std::string path) {
    struct stat sb;
    char npath[PATH_MAX];

    const Options& opts = session_.options();
    const FileFilter& filter = session_.filter();

    // prevent ugly double slashes when printing path names (verbatim;
    // path is this frame's private copy)
    const int tp_path_len = static_cast<int>(path.size());
    if (tp_path_len > 1 && path[tp_path_len - 1] == '/')
        path.erase(tp_path_len - 1);
    const char* cpath = path.c_str();

    if (filter.is_ignored(path))
        return;

    int res = opts.followsymlinks ? stat(cpath, &sb) : lstat(cpath, &sb);

    if (res) {
        session_.fail(Failure::Kind::Stat, errno, path);
        return;
    }

    if (S_ISLNK(sb.st_mode)) {
        session_.fail(Failure::Kind::Symlink, 0, path);
        return;
    }

    if (opts.singlefilesystem) {
        if (!state_.orig_device_inited) {
            state_.orig_device = sb.st_dev;
            state_.orig_device_inited = 1;
        } else if (sb.st_dev != state_.orig_device) {
            session_.fail(Failure::Kind::SeparateFs, 0, path);
            return;
        }
    }

    if (!opts.ignorehardlinkeduplictes && sb.st_nlink > 1) {
        /*
         * For files with more than one link to it, ignore it if we already know
         * inode.  Without this check files copied as hardlinks (cp -al) are
         * counted twice (which may lead to a cache usage of more than 100% of
         * RAM).
         */
        struct dev_and_inode obj;
        obj.dev = sb.st_dev;
        obj.ino = sb.st_ino;
        if (tfind(&obj, &state_.seen_inodes, compare_func)) {
            // we already saw the device and inode referenced by this file
            return;
        }
        struct dev_and_inode* newp =
            static_cast<struct dev_and_inode*>(malloc(sizeof(struct dev_and_inode)));
        if (newp == NULL)
            throw FatalError("malloc: out of memory");
        newp->dev = sb.st_dev;
        newp->ino = sb.st_ino;
        if (tsearch(newp, &state_.seen_inodes, compare_func) == NULL)
            throw FatalError("tsearch: out of memory");
    }

    if (S_ISDIR(sb.st_mode)) {
        for (int i = 0; i < state_.curr_crawl_depth; i++) {
            if (state_.crawl_inodes[i] == sb.st_ino) {
                session_.fail(Failure::Kind::CrawlLoop, 0, path);
                return;
            }
        }

        if (state_.curr_crawl_depth == kMaxCrawlDepth)
            throw FatalError("maximum directory crawl depth reached: " + path);

        state_.total_dirs++;

        state_.crawl_inodes[state_.curr_crawl_depth] = sb.st_ino;

        DIR* dirp = opendir(cpath);

        if (dirp == NULL) {
            if (errno == ENFILE || errno == EMFILE) {
                const auto r = platform::raise_nofile_rlimit();
                if (r.outcome == platform::RaiseNofileResult::Outcome::Raised) {
                    dirp = opendir(cpath);   // single retry, as upstream's goto did
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
                session_.fail(Failure::Kind::OpendirFailed, errno, path);   // upstream: "unable to opendir"
                return;
            }
        }

        while (struct dirent* de = readdir(dirp)) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;

            if (snprintf(npath, sizeof(npath), "%s/%s", cpath, de->d_name) >=
                static_cast<int>(sizeof(npath))) {
                session_.fail(Failure::Kind::PathTooLong, 0, path);
                break;
            }

            state_.curr_crawl_depth++;
            walk(std::string(npath));   // recursion: child gets its own path copy
            state_.curr_crawl_depth--;
        }

        if (closedir(dirp)) {
            session_.fail(Failure::Kind::Closedir, errno, path);
            return;
        }
    } else if (S_ISREG(sb.st_mode) || S_ISBLK(sb.st_mode)) {
        if (filter.is_filename_filtered(path)) {
            state_.total_files++;
            session_.process_file(path);
        }
    } else {
        session_.fail(Failure::Kind::NotRegular, 0, path);
    }
}
