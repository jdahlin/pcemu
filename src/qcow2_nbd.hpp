#pragma once
// QCOW2 mutations are delegated to QEMU, not a second disk-format writer.
// This client uses standard fixed-newstyle NBD over a private Unix socket.
#include <array>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <fcntl.h>
extern char **environ;
namespace pc {
inline pid_t startStorageTool(std::vector<std::string> arguments, const std::string &log) {
    std::vector<char *> argv;
    for (auto &s : arguments) argv.push_back(s.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log.c_str(), O_WRONLY|O_CREAT|O_APPEND, 0600);
    posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    pid_t pid = -1;
    int result = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (result) throw std::runtime_error("Cannot start " + arguments[0] + ": " + std::strerror(result));
    return pid;
}
inline std::string storageLog(const std::string &log) {
    std::ifstream file(log);
    std::string text((std::istreambuf_iterator<char>(file)), {});
    if (text.size() > 2048) text.resize(2048);
    return text;
}
inline void runStorageTool(std::vector<std::string> arguments) {
    char name[] = "/tmp/pcemu-storage-XXXXXX";
    int fd = mkstemp(name);
    if (fd < 0) throw std::runtime_error("Cannot create storage tool log");
    close(fd);
    try {
        auto pid = startStorageTool(std::move(arguments), name);
        int status = 0;
        while (waitpid(pid, &status, 0) < 0) if (errno != EINTR) throw std::runtime_error("Storage tool wait failed");
        if (!WIFEXITED(status) || WEXITSTATUS(status)) throw std::runtime_error(storageLog(name));
        unlink(name);
    } catch (...) { unlink(name); throw; }
}
class Qcow2Nbd {
    int socket = -1;
    pid_t child = -1;
    std::string directory;
    uint64_t bytes = 0, sequence = 0;
    uint16_t flags = 0;
    std::array<uint8_t,65536> readCache{};
    uint64_t cacheAt = ~uint64_t(0);
    unsigned cacheLength = 0;
    static void put(uint8_t *out, uint64_t n, unsigned width) {
        for (unsigned i = 0; i < width; ++i) out[width-1-i] = n >> (8*i);
    }
    static uint64_t get(const uint8_t *in, unsigned width) {
        uint64_t n = 0;
        while (width--) n = (n<<8) | *in++;
        return n;
    }
    void send(const uint8_t *data, size_t length) {
        while (length) {
#ifdef MSG_NOSIGNAL
            auto n = ::send(socket, data, length, MSG_NOSIGNAL);
#else
            auto n = ::send(socket, data, length, 0);
#endif
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) throw std::runtime_error("QCOW2 backend write failed");
            data += n; length -= n;
        }
    }
    void receive(uint8_t *data, size_t length) {
        while (length) {
            auto n = recv(socket, data, length, 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) throw std::runtime_error("QCOW2 backend read failed");
            data += n; length -= n;
        }
    }
    void request(uint16_t command, uint64_t at, uint32_t length, uint8_t *data) {
        std::array<uint8_t,28> header{};
        put(header.data(), 0x25609513, 4); put(header.data()+6, command, 2);
        put(header.data()+8, ++sequence, 8); put(header.data()+16, at, 8); put(header.data()+24, length, 4);
        send(header.data(), header.size());
        if (command == 1) send(data, length);
        std::array<uint8_t,16> reply{};
        receive(reply.data(), reply.size());
        if (get(reply.data(),4) != 0x67446698 || get(reply.data()+8,8) != sequence || get(reply.data()+4,4))
            throw std::runtime_error("QCOW2 backend rejected request");
        if (!command) receive(data, length);
    }
    void cleanup() noexcept {
        if (socket >= 0) { close(socket); socket = -1; }
        if (child > 0) {
            // Closing the sole NBD connection terminates non-persistent qemu-nbd.
            int status;
            for (unsigned i = 0; i < 200; ++i) {
                auto result = waitpid(child, &status, WNOHANG);
                if (result == child || (result < 0 && errno == ECHILD)) { child = -1; break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (child > 0) { kill(child, SIGTERM); while (waitpid(child,&status,0)<0 && errno==EINTR) {} child=-1; }
        }
        if (!directory.empty()) { std::error_code ec; std::filesystem::remove_all(directory,ec); }
    }
  public:
    Qcow2Nbd(const std::string &path, bool readonly) {
        char name[] = "/tmp/pcemu-nbd-XXXXXX";
        if (!mkdtemp(name)) throw std::runtime_error("Cannot create QCOW2 backend directory");
        directory = name;
        try {
            std::vector<std::string> args{"qemu-nbd","--format=qcow2","--cache=writeback",
                                          "--socket="+directory+"/disk.sock"};
            if (readonly) args.push_back("--read-only");
            args.push_back(path);
            child = startStorageTool(std::move(args), directory+"/backend.log");
            socket = ::socket(AF_UNIX, SOCK_STREAM, 0);
            if (socket < 0) throw std::runtime_error("Cannot create QCOW2 backend socket");
            fcntl(socket,F_SETFD,FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
            int one = 1; setsockopt(socket,SOL_SOCKET,SO_NOSIGPIPE,&one,sizeof(one));
#endif
            timeval timeout{30,0};
            setsockopt(socket,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
            setsockopt(socket,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
            sockaddr_un address{}; address.sun_family=AF_UNIX;
            std::strncpy(address.sun_path,(directory+"/disk.sock").c_str(),sizeof(address.sun_path)-1);
            bool connected = false;
            for (unsigned i=0;i<1000;++i) {
                if (!connect(socket,reinterpret_cast<sockaddr *>(&address),sizeof(address))) { connected=true; break; }
                int status;
                if (waitpid(child,&status,WNOHANG)==child) {
                    child=-1; throw std::runtime_error("QCOW2 backend: "+storageLog(directory+"/backend.log"));
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (!connected) throw std::runtime_error("QCOW2 backend startup timed out");
            std::array<uint8_t,18> greeting{}; receive(greeting.data(),greeting.size());
            if (get(greeting.data(),8)!=0x4e42444d41474943ull || get(greeting.data()+8,8)!=0x49484156454f5054ull ||
                !(get(greeting.data()+16,2)&1)) throw std::runtime_error("Invalid fixed-newstyle NBD greeting");
            const bool noZeroes = get(greeting.data()+16,2)&2;
            std::array<uint8_t,20> option{};
            put(option.data(),1|(noZeroes?2:0),4); send(option.data(),4);
            put(option.data(),0x49484156454f5054ull,8); put(option.data()+8,1,4);
            send(option.data(),16); // NBD_OPT_EXPORT_NAME, empty export name.
            std::array<uint8_t,10> exportInfo{}; receive(exportInfo.data(),exportInfo.size());
            bytes=get(exportInfo.data(),8); flags=get(exportInfo.data()+8,2);
            if (!noZeroes) { std::array<uint8_t,124> padding{}; receive(padding.data(),padding.size()); }
            if (!bytes || bytes%512 || !(flags&1) || (!readonly && (flags&2)))
                throw std::runtime_error("Invalid QCOW2 backend export");
        } catch (...) { cleanup(); throw; }
    }
    ~Qcow2Nbd() { cleanup(); }
    Qcow2Nbd(const Qcow2Nbd &)=delete;
    uint64_t size() const { return bytes; }
    void read(uint64_t at, uint32_t length, uint8_t *data) {
        if (at>bytes || length>bytes-at) throw std::runtime_error("QCOW2 read outside export");
        if (length>=readCache.size()) { request(0,at,length,data); return; }
        while (length) {
            const uint64_t base=at & ~uint64_t(readCache.size()-1);
            if (cacheAt!=base) {
                cacheLength=std::min<uint64_t>(readCache.size(),bytes-base);
                request(0,base,cacheLength,readCache.data()); cacheAt=base;
            }
            const unsigned within=at-base, n=std::min(length,cacheLength-within);
            std::memcpy(data,readCache.data()+within,n); at+=n; data+=n; length-=n;
        }
    }
    void write(uint64_t at, uint32_t length, const uint8_t *data) {
        if (at>bytes || length>bytes-at) throw std::runtime_error("QCOW2 write outside export");
        cacheAt=~uint64_t(0);
        request(1,at,length,const_cast<uint8_t *>(data));
    }
    void flush() {
        if (flags&4) request(3,0,0,nullptr);
        else throw std::runtime_error("QCOW2 backend does not support durable flush");
    }
};
} // namespace pc
