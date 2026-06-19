#include "common.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace remote_lab {
namespace {

constexpr std::array<char, 4> kMagic{'R', 'L', 'B', '1'};
constexpr std::uint32_t kHeaderSize = 12;
constexpr std::uint32_t kMaxPayloadSize = 64U * 1024U * 1024U;

std::uint32_t read_u32_network(const std::uint8_t* data) {
    std::uint32_t value = 0;
    value |= static_cast<std::uint32_t>(data[0]) << 24U;
    value |= static_cast<std::uint32_t>(data[1]) << 16U;
    value |= static_cast<std::uint32_t>(data[2]) << 8U;
    value |= static_cast<std::uint32_t>(data[3]);
    return value;
}

void write_u32_network(std::uint8_t* data, std::uint32_t value) {
    data[0] = static_cast<std::uint8_t>((value >> 24U) & 0xffU);
    data[1] = static_cast<std::uint8_t>((value >> 16U) & 0xffU);
    data[2] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
    data[3] = static_cast<std::uint8_t>(value & 0xffU);
}

bool send_all(SOCKET socket_handle, const std::uint8_t* data, std::size_t size, std::string& error) {
    std::size_t sent = 0;
    while (sent < size) {
        const int chunk = static_cast<int>(std::min<std::size_t>(size - sent, 64 * 1024));
        const int result = send(socket_handle, reinterpret_cast<const char*>(data + sent), chunk, 0);
        if (result == SOCKET_ERROR) {
            error = last_socket_error("send");
            return false;
        }
        if (result == 0) {
            error = "socket closed while sending";
            return false;
        }
        sent += static_cast<std::size_t>(result);
    }
    return true;
}

bool recv_all(SOCKET socket_handle, std::uint8_t* data, std::size_t size, std::string& error) {
    std::size_t received = 0;
    while (received < size) {
        const int chunk = static_cast<int>(std::min<std::size_t>(size - received, 64 * 1024));
        const int result = recv(socket_handle, reinterpret_cast<char*>(data + received), chunk, 0);
        if (result == SOCKET_ERROR) {
            error = last_socket_error("recv");
            return false;
        }
        if (result == 0) {
            error = "peer closed the connection";
            return false;
        }
        received += static_cast<std::size_t>(result);
    }
    return true;
}

} // namespace

WsaSession::WsaSession() {
    WSADATA data{};
    const int result = WSAStartup(MAKEWORD(2, 2), &data);
    if (result != 0) {
        std::ostringstream oss;
        oss << "WSAStartup failed: " << result;
        error_ = oss.str();
        return;
    }
    ok_ = true;
}

WsaSession::~WsaSession() {
    if (ok_) {
        WSACleanup();
    }
}

std::string last_socket_error(const char* action) {
    std::ostringstream oss;
    oss << action << " failed, WSAGetLastError=" << WSAGetLastError();
    return oss.str();
}

void close_socket(SOCKET socket_handle) {
    if (socket_handle != INVALID_SOCKET) {
        closesocket(socket_handle);
    }
}

bool send_packet(SOCKET socket_handle,
                 MessageType type,
                 const std::vector<std::uint8_t>& payload,
                 std::string& error) {
    if (payload.size() > kMaxPayloadSize) {
        error = "payload is larger than 64 MB";
        return false;
    }

    std::array<std::uint8_t, kHeaderSize> header{};
    std::copy(kMagic.begin(), kMagic.end(), header.begin());
    write_u32_network(header.data() + 4, static_cast<std::uint32_t>(type));
    write_u32_network(header.data() + 8, static_cast<std::uint32_t>(payload.size()));

    if (!send_all(socket_handle, header.data(), header.size(), error)) {
        return false;
    }
    if (!payload.empty() && !send_all(socket_handle, payload.data(), payload.size(), error)) {
        return false;
    }
    return true;
}

bool receive_packet(SOCKET socket_handle, Packet& packet, std::string& error) {
    std::array<std::uint8_t, kHeaderSize> header{};
    if (!recv_all(socket_handle, header.data(), header.size(), error)) {
        return false;
    }

    if (!std::equal(kMagic.begin(), kMagic.end(), header.begin())) {
        error = "invalid packet magic";
        return false;
    }

    const auto type_value = read_u32_network(header.data() + 4);
    const auto payload_size = read_u32_network(header.data() + 8);
    if (payload_size > kMaxPayloadSize) {
        error = "peer sent a payload larger than 64 MB";
        return false;
    }

    packet.type = static_cast<MessageType>(type_value);
    packet.payload.assign(payload_size, 0);
    if (payload_size > 0 && !recv_all(socket_handle, packet.payload.data(), packet.payload.size(), error)) {
        return false;
    }
    return true;
}

std::vector<std::uint8_t> string_payload(const std::string& text) {
    return {text.begin(), text.end()};
}

std::string payload_to_string(const std::vector<std::uint8_t>& payload) {
    return {payload.begin(), payload.end()};
}

std::vector<std::uint8_t> make_named_payload(const std::string& name,
                                             const std::vector<std::uint8_t>& data) {
    if (name.size() > kMaxPayloadSize) {
        throw std::runtime_error("name too long");
    }

    std::vector<std::uint8_t> payload(4 + name.size() + data.size());
    write_u32_network(payload.data(), static_cast<std::uint32_t>(name.size()));
    std::copy(name.begin(), name.end(), payload.begin() + 4);
    std::copy(data.begin(), data.end(), payload.begin() + 4 + static_cast<std::ptrdiff_t>(name.size()));
    return payload;
}

bool parse_named_payload(const std::vector<std::uint8_t>& payload,
                         std::string& name,
                         std::vector<std::uint8_t>& data,
                         std::string& error) {
    if (payload.size() < 4) {
        error = "named payload is shorter than 4 bytes";
        return false;
    }

    const auto name_size = read_u32_network(payload.data());
    if (payload.size() < 4ULL + name_size) {
        error = "named payload has an invalid name length";
        return false;
    }

    name.assign(reinterpret_cast<const char*>(payload.data() + 4), name_size);
    data.assign(payload.begin() + 4 + static_cast<std::ptrdiff_t>(name_size), payload.end());
    return true;
}

bool read_binary_file(const std::filesystem::path& path,
                      std::vector<std::uint8_t>& data,
                      std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "failed to open file for reading: " + narrow_path(path);
        return false;
    }

    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size < 0) {
        error = "failed to determine file size: " + narrow_path(path);
        return false;
    }
    input.seekg(0, std::ios::beg);

    data.assign(static_cast<std::size_t>(size), 0);
    if (!data.empty()) {
        input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    if (!input && !input.eof()) {
        error = "failed to read file: " + narrow_path(path);
        return false;
    }
    return true;
}

bool write_binary_file(const std::filesystem::path& path,
                       const std::vector<std::uint8_t>& data,
                       std::string& error) {
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            error = "failed to create directory " + narrow_path(parent) + ": " + ec.message();
            return false;
        }
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "failed to open file for writing: " + narrow_path(path);
        return false;
    }

    if (!data.empty()) {
        output.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    if (!output) {
        error = "failed to write file: " + narrow_path(path);
        return false;
    }
    return true;
}

std::string narrow_path(const std::filesystem::path& path) {
    return path.u8string();
}

} // namespace remote_lab
