#include "vfs.h"

#include <fuse3/fuse.h>
#include <iostream>
#include <string>
#include <map>
#include <vector>
#include <sstream>
#include <fstream>
#include <unistd.h>
#include <sys/wait.h>
#include <pwd.h>
#include <sys/stat.h>
#include <cstring>
#include <cerrno>
#include <cstdlib>
#include <pthread.h>
#include <ctime>
#include <algorithm>

class VirtualFileSystem {
public:
    static VirtualFileSystem& instance() {
        static VirtualFileSystem vfs;
        return vfs;
    }

    void initialize() {
        ::mkdir(mount_path().c_str(), 0755);

        pthread_t fuse_thread_id{};
        if (pthread_create(&fuse_thread_id, nullptr, &VirtualFileSystem::run_fuse_thread, nullptr) != 0) {
            std::cerr << "Failed to create FUSE thread" << std::endl;
            return;
        }

        std::cout << "VFS initialized at: " << mount_path() << std::endl;
    }

    void cleanup() {
        const std::string command =
            "fusermount -u " + mount_path() +
            " 2>/dev/null || fusermount3 -u " + mount_path() +
            " 2>/dev/null || true";
        std::system(command.c_str());
    }

    int getattr(const char* path, struct stat* st, struct fuse_file_info* fi) {
        (void)fi;
        std::memset(st, 0, sizeof(struct stat));

        const std::time_t now = std::time(nullptr);
        st->st_atime = st->st_mtime = st->st_ctime = now;

        if (std::strcmp(path, "/") == 0) {
            st->st_mode = S_IFDIR | 0755;
            st->st_uid = ::getuid();
            st->st_gid = ::getgid();
            return 0;
        }

        char username[256];
        char filename[256];

        if (std::sscanf(path, "/%255[^/]/%255[^/]", username, filename) == 2) {
            if (vfs_data_.count(username) &&
                (std::strcmp(filename, "id") == 0 ||
                 std::strcmp(filename, "home") == 0 ||
                 std::strcmp(filename, "shell") == 0)) {

                st->st_mode = S_IFREG | 0644;
                st->st_uid = ::getuid();
                st->st_gid = ::getgid();
                st->st_size = vfs_data_[username][filename].size();
                return 0;
            }
            return -ENOENT;
        }

        if (std::sscanf(path, "/%255[^/]", username) == 1) {
            if (vfs_data_.count(username)) {
                st->st_mode = S_IFDIR | 0755;
                st->st_uid = ::getuid();
                st->st_gid = ::getgid();
                return 0;
            }
            return -ENOENT;
        }

        return -ENOENT;
    }

    int readdir(const char* path,
                void* buf,
                fuse_fill_dir_t filler,
                off_t offset,
                struct fuse_file_info* fi,
                enum fuse_readdir_flags flags) {
        (void)offset;
        (void)fi;
        (void)flags;

        filler(buf, ".", nullptr, 0, FUSE_FILL_DIR_PLUS);
        filler(buf, "..", nullptr, 0, FUSE_FILL_DIR_PLUS);

        if (std::strcmp(path, "/") == 0) {
            for (const auto& entry : vfs_data_) {
                filler(buf, entry.first.c_str(), nullptr, 0, FUSE_FILL_DIR_PLUS);
            }
            return 0;
        }

        std::string username(path + 1);
        if (vfs_data_.count(username)) {
            filler(buf, "id", nullptr, 0, FUSE_FILL_DIR_PLUS);
            filler(buf, "home", nullptr, 0, FUSE_FILL_DIR_PLUS);
            filler(buf, "shell", nullptr, 0, FUSE_FILL_DIR_PLUS);
            return 0;
        }

        return -ENOENT;
    }

    int read(const char* path,
             char* buf,
             std::size_t size,
             off_t offset,
             struct fuse_file_info* fi) {
        (void)fi;

        char username[256];
        char filename[256];

        if (std::sscanf(path, "/%255[^/]/%255[^/]", username, filename) != 2) {
            return -ENOENT;
        }

        if (!vfs_data_.count(username) ||
            !vfs_data_[username].count(filename)) {
            return -ENOENT;
        }

        const std::string& content = vfs_data_[username][filename];
        if (static_cast<std::size_t>(offset) >= content.size()) {
            return 0;
        }

        const std::size_t length =
            std::min<std::size_t>(size, content.size() - static_cast<std::size_t>(offset));

        std::memcpy(buf, content.c_str() + offset, length);
        return static_cast<int>(length);
    }

    int mkdir(const char* path, mode_t mode) {
        (void)mode;

        char username[256];

        if (std::sscanf(path, "/%255[^/]", username) == 1) {
            if (vfs_data_.count(username)) {
                return -EEXIST;
            }

            std::cout << "VFS: Adding user: " << username << std::endl;

            std::string command =
                "useradd -m -s /bin/bash " + std::string(username) + " 2>/dev/null";
            int result = std::system(command.c_str());

            if (result != 0) {
                command =
                    "adduser --disabled-password --gecos '' " +
                    std::string(username) + " 2>/dev/null";
                result = std::system(command.c_str());
            }

            if (result == 0) {
                sync_with_passwd();
                std::cout << "User " << username << " added successfully" << std::endl;
                return 0;
            }

            std::cerr << "Failed to create user: " << username << std::endl;
            return -EIO;
        }

        return 0;
    }

    int rmdir(const char* path) {
        char username[256];

        if (std::sscanf(path, "/%255[^/]", username) == 1) {
            if (std::strchr(path + 1, '/') == nullptr) {
                if (!vfs_data_.count(username)) {
                    return -ENOENT;
                }

                std::cout << "VFS: Deleting user: " << username << std::endl;

                std::string command =
                    "userdel -r " + std::string(username) + " 2>/dev/null";
                const int result = std::system(command.c_str());

                if (result == 0) {
                    vfs_data_.erase(username);
                    std::cout << "User " << username << " deleted successfully" << std::endl;
                    return 0;
                }

                std::cerr << "Failed to delete user: " << username << std::endl;
                return -EIO;
            }
            return -EPERM;
        }

        return -EPERM;
    }

private:
    VirtualFileSystem() = default;

    static void* run_fuse_thread(void* arg) {
        (void)arg;

        VirtualFileSystem& vfs = VirtualFileSystem::instance();
        vfs.sync_with_passwd();

        ::mkdir(mount_path().c_str(), 0755);

        const char* argv_local[] = {
            "kubsh_vfs",
            mount_path().c_str(),
            "-f",
            "-s",
            nullptr
        };
        const int argc_local = 3;

        static struct fuse_operations operations {};
        operations.getattr = &VirtualFileSystem::getattr_wrapper;
        operations.mkdir = &VirtualFileSystem::mkdir_wrapper;
        operations.rmdir = &VirtualFileSystem::rmdir_wrapper;
        operations.read = &VirtualFileSystem::read_wrapper;
        operations.readdir = &VirtualFileSystem::readdir_wrapper;

        std::cout << "Mounting VFS at: " << mount_path() << std::endl;
        const int ret =
            ::fuse_main(argc_local,
                        const_cast<char**>(argv_local),
                        &operations,
                        nullptr);
        std::cout << "FUSE exited with code: " << ret << std::endl;

        return nullptr;
    }

    static const std::string& mount_path() {
        static const std::string path = "/opt/users";
        return path;
    }

    void sync_with_passwd() {
        vfs_data_.clear();

        std::ifstream passwd_file("/etc/passwd");
        if (!passwd_file.is_open()) {
            std::cerr << "Cannot open /etc/passwd" << std::endl;
            return;
        }

        std::string line;
        while (std::getline(passwd_file, line)) {
            std::vector<std::string> fields;
            std::string field;
            std::stringstream ss(line);

            while (std::getline(ss, field, ':')) {
                fields.push_back(field);
            }

            if (fields.size() >= 7) {
                const std::string& username = fields[0];
                const std::string& uid      = fields[2];
                const std::string& home     = fields[5];
                const std::string& shell    = fields[6];

                const int uid_num = std::stoi(uid);
                if (uid_num == 0 || uid_num >= 1000) {
                    if (shell != "/bin/false" && shell != "/usr/sbin/nologin") {
                        vfs_data_[username]["id"] = uid;
                        vfs_data_[username]["home"] = home;
                        vfs_data_[username]["shell"] = shell;
                    }
                }
            }
        }
    }

    static int getattr_wrapper(const char* path,
                               struct stat* st,
                               struct fuse_file_info* fi) {
        return VirtualFileSystem::instance().getattr(path, st, fi);
    }

    static int readdir_wrapper(const char* path,
                               void* buf,
                               fuse_fill_dir_t filler,
                               off_t offset,
                               struct fuse_file_info* fi,
                               enum fuse_readdir_flags flags) {
        return VirtualFileSystem::instance().readdir(path, buf, filler, offset, fi, flags);
    }

    static int read_wrapper(const char* path,
                            char* buf,
                            std::size_t size,
                            off_t offset,
                            struct fuse_file_info* fi) {
        return VirtualFileSystem::instance().read(path, buf, size, offset, fi);
    }

    static int mkdir_wrapper(const char* path, mode_t mode) {
        return VirtualFileSystem::instance().mkdir(path, mode);
    }

    static int rmdir_wrapper(const char* path) {
        return VirtualFileSystem::instance().rmdir(path);
    }

    std::map<std::string, std::map<std::string, std::string>> vfs_data_;
};

void initialize_vfs() {
    VirtualFileSystem::instance().initialize();
}

void cleanup_vfs() {
    VirtualFileSystem::instance().cleanup();
}
