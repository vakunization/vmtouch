// core/crawl.cpp — see crawl.h (spec2 §5).
//
// The traversal logic is transplanted verbatim from the monolith's
// vmtouch_crawl() (through the Increment-1 Crawler): trailing-slash strip,
// lstat, symlink check, hardlink dedup, directory loop check, depth limit,
// opendir/readdir/closedir. Deviations required by the producer seam:
//   * per-file work and traversal failures go to the VISITOR (CrawlItem),
//     not to a Session;
//   * fatal cases (depth overflow, allocation failure, rlimit) throw
//     FatalError with the historical texts (spec2 §5.10);
//   * -f/-F/-h policy is FIXED (flags dropped per the ratified ruling):
//     lstat always, no device pinning check, dedup always on.
// The seen-inodes tsearch tree is freed in the destructor (replacing the
// atexit workaround from the leak fix).

#include "crawl.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#include "platform.h"

namespace vmt {
namespace {

constexpr int kMaxCrawlDepth = 1024;

// compare device and inode information (verbatim from the monolith)
int compare_func(const void* p1, const void* p2) {
    const struct dev_and_inode* kp1 = static_cast<const struct dev_and_inode*>(p1);
    const struct dev_and_inode* kp2 = static_cast<const struct dev_and_inode*>(p2);
    const int cmp1 = (kp1->ino > kp2->ino) - (kp1->ino < kp2->ino);
    if (cmp1 != 0)
        return cmp1;
    return (kp1->dev > kp2->dev) - (kp1->dev < kp2->dev);
}

void free_dev_and_inode(void* p) {
    free(p);
}

} // namespace

Crawler::Crawler() = default;

Crawler::~Crawler() {
    // Frees the seen-inodes tree (upstream leak, originally fixed via
    // atexit in the CLI; owned properly by the state holder since Increment 1).
    if (state_.seen_inodes) {
        tdestroy(state_.seen_inodes, free_dev_and_inode);
        state_.seen_inodes = nullptr;
    }
}

void Crawler::walk(std::string path, const CrawlVisitor& visitor) {
    walk_impl(path, visitor);
}

void Crawler::walk_impl(std::string& path, const CrawlVisitor& visitor) {
    struct stat sb;
    char npath[PATH_MAX];

    // prevent ugly double slashes when printing path names (verbatim;
    // path is this frame's private copy)
    const int tp_path_len = static_cast<int>(path.size());
    if (tp_path_len > 1 && path[tp_path_len - 1] == '/')
        path.erase(tp_path_len - 1);
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

    // Fixed policy: hardlink dedup always on (upstream default).
    if (sb.st_nlink > 1) {
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
        // Loop check against the directory-inode stack (spec2 §5.7).
        for (int i = 0; i < state_.curr_crawl_depth; i++) {
            if (state_.crawl_inodes[i] == sb.st_ino) {
                const Failure f{Failure::Kind::CrawlLoop, 0, path, 0};
                visitor(CrawlItem{path, &f});
                return;
            }
        }

        if (state_.curr_crawl_depth == kMaxCrawlDepth)
            throw FatalError("maximum directory crawl depth reached: " + path);

        state_.total_dirs++;   // entered (counts even if opendir fails, spec2 §5.8)

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
                const Failure f{Failure::Kind::OpendirFailed, errno, path, 0};
                visitor(CrawlItem{path, &f});
                return;
            }
        }

        while (struct dirent* de = readdir(dirp)) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;

            if (snprintf(npath, sizeof(npath), "%s/%s", cpath, de->d_name) >=
                static_cast<int>(sizeof(npath))) {
                const Failure f{Failure::Kind::PathTooLong, 0, path, 0};
                visitor(CrawlItem{path, &f});
                break;
            }

            state_.curr_crawl_depth++;
            std::string child(npath);   // recursion: child gets its own path copy
            walk_impl(child, visitor);
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
