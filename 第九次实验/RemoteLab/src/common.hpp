#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace remote_lab {

enum class MessageType : std::uint32_t {
    Echo = 1,
    Shutdown = 2,
    CancelShutdown = 3,
    ListFiles = 4,
    Screenshot = 5,
    DeleteFile = 6,
    UploadFile = 7,
    DownloadFile = 8,
    Quit = 9,

    Ok = 100,
    Error = 101,
    Text = 102,
    File = 103
};

struct Packet {
    MessageType type{};
    std::vector<std::uint8_t> payload;
};

class WsaSession {
public:
    WsaSession();
    ~WsaSession();

    WsaSession(const WsaSession&) = delete;
    WsaSession& operator=(const WsaSession&) = delete;

    bool ok() const noexcept { return ok_; }
    const std::string& error() const noexcept { return error_; }

private:
    bool ok_{false};
    std::string error_;
};

std::string last_socket_error(const char* action);
void close_socket(SOCKET socket_handle);

bool send_packet(SOCKET socket_handle,
                 MessageType type,
                 const std::vector<std::uint8_t>& payload,
                 std::string& error);

bool receive_packet(SOCKET socket_handle, Packet& packet, std::string& error);

std::vector<std::uint8_t> string_payload(const std::string& text);
std::string payload_to_string(const std::vector<std::uint8_t>& payload);

std::vector<std::uint8_t> make_named_payload(const std::string& name,
                                             const std::vector<std::uint8_t>& data);

bool parse_named_payload(const std::vector<std::uint8_t>& payload,
                         std::string& name,
                         std::vector<std::uint8_t>& data,
                         std::string& error);

bool read_binary_file(const std::filesystem::path& path,
                      std::vector<std::uint8_t>& data,
                      std::string& error);

bool write_binary_file(const std::filesystem::path& path,
                       const std::vector<std::uint8_t>& data,
                       std::string& error);

std::string narrow_path(const std::filesystem::path& path);

} // namespace remote_lab
