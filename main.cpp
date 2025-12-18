#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <sstream>
#include <csignal>
#include <cstring>
#include <cstdint>
#include <cerrno>

#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "vfs.h"

class ShellSignalManager {
public:
    static void install_sighup_handler() {
        struct sigaction action;
        std::memset(&action, 0, sizeof(action));
        action.sa_handler = &ShellSignalManager::sighup_handler;
        action.sa_flags = SA_RESTART;
        sigaction(SIGHUP, &action, nullptr);
    }

    static bool is_sighup_received() {
        return sighup_flag_ != 0;
    }

    static void clear_sighup() {
        sighup_flag_ = 0;
    }

private:
    static void sighup_handler(int) {
        sighup_flag_ = 1;
    }

    static volatile sig_atomic_t sighup_flag_;
};

volatile sig_atomic_t ShellSignalManager::sighup_flag_ = 0;

class PartitionTableAnalyzer {
public:
    static void list_partitions_mbr(const std::string& disk_path) {
        int fd = ::open(disk_path.c_str(), O_RDONLY);
        if (fd == -1) {
            std::cout << "Cannot open device: " << disk_path
                      << " (errno: " << errno << ")" << std::endl;
            std::perror("open");
            return;
        }

        unsigned char buffer[512];
        const ssize_t bytes_read = ::read(fd, buffer, sizeof(buffer));
        ::close(fd);

        if (bytes_read != static_cast<ssize_t>(sizeof(buffer))) {
            std::cerr << "Error reading MBR from: " << disk_path
                      << " (read " << bytes_read << " bytes)" << std::endl;
            return;
        }

        if (buffer[510] != 0x55 || buffer[511] != 0xAA) {
            std::cerr << "Invalid MBR signature on: " << disk_path << std::endl;
            std::cerr << "Got signature: 0x"
                      << std::hex << static_cast<int>(buffer[511])
                      << static_cast<int>(buffer[510])
                      << std::dec << std::endl;
            return;
        }

        std::cout << "Disk analysis for: " << disk_path << std::endl;
        std::cout << "Partition table:" << std::endl;

        bool bootable_found = false;
        bool is_gpt_protective = false;

        const int partition_table_offset = 0x1BE;

        for (int index = 0; index < 4; ++index) {
            const int offset = partition_table_offset + index * 16;

            const std::uint8_t status = buffer[offset];
            const std::uint8_t type = buffer[offset + 4];

            const std::uint32_t lba_start =
                (static_cast<std::uint32_t>(buffer[offset + 11]) << 24) |
                (static_cast<std::uint32_t>(buffer[offset + 10]) << 16) |
                (static_cast<std::uint32_t>(buffer[offset + 9]) << 8) |
                static_cast<std::uint32_t>(buffer[offset + 8]);

            const std::uint32_t sector_count =
                (static_cast<std::uint32_t>(buffer[offset + 15]) << 24) |
                (static_cast<std::uint32_t>(buffer[offset + 14]) << 16) |
                (static_cast<std::uint32_t>(buffer[offset + 13]) << 8) |
                static_cast<std::uint32_t>(buffer[offset + 12]);

            std::cout << "Partition " << (index + 1) << ": ";

            if (status == 0x80) {
                std::cout << "Bootable, ";
                bootable_found = true;
            } else if (status == 0x00) {
                std::cout << "Non-bootable, ";
            } else {
                std::cout << "Unknown status (0x"
                          << std::hex << static_cast<int>(status)
                          << std::dec << "), ";
            }

            std::cout << "Type: 0x"
                      << std::hex << static_cast<int>(type)
                      << std::dec
                      << " (" << partition_type_description(type) << ")";

            if (type == 0xEE) {
                is_gpt_protective = true;
            }

            if (type != 0x00 && sector_count > 0) {
                const std::uint64_t size_bytes =
                    static_cast<std::uint64_t>(sector_count) * 512;

                if (size_bytes >= 1024ull * 1024ull * 1024ull) {
                    std::cout << ", Size: "
                              << (size_bytes / (1024.0 * 1024.0 * 1024.0))
                              << " GB";
                } else {
                    std::cout << ", Size: "
                              << (size_bytes / (1024.0 * 1024.0))
                              << " MB";
                }

                std::cout << ", Start LBA: " << lba_start;
            }

            std::cout << std::endl;
        }

        if (is_gpt_protective) {
            std::cout << "This disk uses GPT partitioning (protective MBR detected)" << std::endl;
        } else {
            std::cout << "This disk uses MBR partitioning" << std::endl;
        }

        if (!bootable_found) {
            std::cout << "No bootable partitions found" << std::endl;
        }
    }

private:
    static std::string partition_type_description(std::uint8_t type) {
        switch (type) {
            case 0x00: return "Empty";
            case 0xEE: return "GPT Protective";
            case 0xEF: return "EFI System";
            case 0x07: return "NTFS/HPFS";
            case 0x0B: return "FAT32 (CHS)";
            case 0x0C: return "FAT32 (LBA)";
            case 0x05: return "Extended (CHS)";
            case 0x0F: return "Extended (LBA)";
            case 0x82: return "Linux Swap";
            case 0x83: return "Linux";
            case 0x8E: return "Linux LVM";
            default:   return "Unknown";
        }
    }
};

class ShellCommandExecutor {
public:
    static void execute_debug(const std::string& input) {
        std::string payload = input.substr(5);

        while (!payload.empty() && payload.front() == ' ') {
            payload.erase(payload.begin());
        }

        if (!payload.empty()) {
            const char first = payload.front();
            const char last  = payload.back();

            if ((first == '"' && last == '"') ||
                (first == '\'' && last == '\'')) {
                if (payload.size() >= 2) {
                    payload = payload.substr(1, payload.size() - 2);
                } else {
                    payload.clear();
                }
            }
        }

        std::cout << payload << '\n';
    }

    static void print_environment_variable(const std::string& input) {
        const std::size_t command_pos = input.find("\\e");
        const std::size_t var_pos = command_pos + 3;

        if (var_pos < input.length()) {
            std::string variable_name = input.substr(var_pos);

            if (!variable_name.empty() && variable_name.front() == '$') {
                variable_name.erase(variable_name.begin());
            }

            const char* env_value = std::getenv(variable_name.c_str());
            if (env_value != nullptr) {
                const std::string value(env_value);
                std::size_t start = 0;
                std::size_t end = value.find(':');

                while (end != std::string::npos) {
                    std::cout << value.substr(start, end - start) << '\n';
                    start = end + 1;
                    end = value.find(':', start);
                }

                std::cout << value.substr(start) << '\n';
            } else {
                std::cout << "Environment variable '" << variable_name
                          << "' not found" << '\n';
            }
        } else {
            std::cout << "Usage: \\e $VARIABLE" << '\n';
        }
    }

    static void analyze_disk_mbr(const std::string& input) {
        if (input.length() > 3) {
            std::string device_path = input.substr(3);

            while (!device_path.empty() && device_path.front() == ' ') {
                device_path.erase(device_path.begin());
            }

            if (!device_path.empty()) {
                PartitionTableAnalyzer::list_partitions_mbr(device_path);
            } else {
                std::cout << "Usage: \\l /dev/device" << '\n';
            }
        } else {
            std::cout << "Usage: \\l /dev/device" << '\n';
        }
    }

    static void execute_external(const std::string& input) {
        const pid_t pid = ::fork();

        if (pid == 0) {
            std::vector<std::string> args;
            std::stringstream stream(input);
            std::string token;

            while (stream >> token) {
                args.push_back(token);
            }

            std::vector<char*> argv;
            argv.reserve(args.size() + 1);

            for (auto& argument : args) {
                argv.push_back(const_cast<char*>(argument.c_str()));
            }
            argv.push_back(nullptr);

            ::execvp(argv[0], argv.data());

            std::cout << input << ": command not found\n";
            std::_Exit(1);
        }

        if (pid > 0) {
            int status = 0;
            ::waitpid(pid, &status, 0);
        } else {
            std::cerr << "Failed to create process" << '\n';
        }
    }
};

class InteractiveShell {
public:
    InteractiveShell()
        : history_file_path_("kubsh_history.txt"),
          history_stream_(history_file_path_, std::ios::app) {}

    void run() {
        std::cerr << "$ ";

        std::string input;
        while (std::getline(std::cin, input)) {
            if (ShellSignalManager::is_sighup_received()) {
                std::cout << "Configuration reloaded" << std::endl;
                ShellSignalManager::clear_sighup();
                std::cerr << "$ ";
                continue;
            }

            trim_leading_spaces(input);
            append_to_history(input);

            if (input == "\\q") {
                break;
            }

            if (input.empty()) {
                std::cerr << "$ ";
                continue;
            }

            if (input.find("debug") == 0) {
                ShellCommandExecutor::execute_debug(input);
            } else if (input.find("\\e") == 0) {
                ShellCommandExecutor::print_environment_variable(input);
            } else if (input.find("\\l") == 0) {
                ShellCommandExecutor::analyze_disk_mbr(input);
            } else {
                ShellCommandExecutor::execute_external(input);
            }

            std::cerr << "$ ";
        }
    }

private:
    static void trim_leading_spaces(std::string& input) {
        while (!input.empty() && input.front() == ' ') {
            input.erase(input.begin());
        }
    }

    void append_to_history(const std::string& input) {
        if (history_stream_.is_open()) {
            history_stream_ << '$' << input << '\n';
            history_stream_.flush();
        }
    }

    std::string   history_file_path_;
    std::ofstream history_stream_;
};

int main() {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    initialize_vfs();
    ShellSignalManager::install_sighup_handler();

    InteractiveShell shell;
    shell.run();

    cleanup_vfs();
    return 0;
}
