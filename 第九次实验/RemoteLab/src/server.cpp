#include "common.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using remote_lab::MessageType;
using remote_lab::Packet;

namespace {

struct ServerConfig {
    std::string bind_host = "0.0.0.0";
    unsigned short port = 5050;
    std::filesystem::path root = "C:\\TrojanLab\\server_files";
};

void print_usage() {
    std::cout
        << "remote_lab_server [--host 0.0.0.0] [--port 5050] [--root C:\\TrojanLab\\server_files]\n"
        << "\n"
        << "File operations are limited to --root. Shutdown commands are real and use a 60-second delay.\n";
}

bool parse_args(int argc, char** argv, ServerConfig& config) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage();
            return false;
        }
        if (arg == "--host" && i + 1 < argc) {
            config.bind_host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            const int port_value = std::stoi(argv[++i]);
            if (port_value <= 0 || port_value > 65535) {
                throw std::runtime_error("port must be in 1..65535");
            }
            config.port = static_cast<unsigned short>(port_value);
        } else if (arg == "--root" && i + 1 < argc) {
            config.root = argv[++i];
        } else {
            throw std::runtime_error("unknown or incomplete argument: " + arg);
        }
    }
    return true;
}

std::string current_time_text() {
    const auto now = std::chrono::system_clock::now();
    const auto time_value = std::chrono::system_clock::to_time_t(now);
    std::tm local_time{};
    localtime_s(&local_time, &time_value);

    std::ostringstream oss;
    oss << std::put_time(&local_time, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

void log_event(const std::string& message) {
    std::cout << "[" << current_time_text() << "] " << message << "\n";
}

bool starts_with_case_insensitive(std::string text, std::string prefix) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(prefix.begin(), prefix.end(), prefix.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text.rfind(prefix, 0) == 0;
}

bool resolve_safe_path(const std::filesystem::path& root,
                       const std::string& relative_text,
                       std::filesystem::path& resolved,
                       std::string& error) {
    std::filesystem::path relative(relative_text);
    if (relative.empty()) {
        error = "empty path";
        return false;
    }
    if (relative.is_absolute()) {
        error = "absolute paths are not accepted; choose a file from the server list";
        return false;
    }
    for (const auto& part : relative) {
        if (part == "..") {
            error = "parent directory references are not accepted";
            return false;
        }
    }

    std::error_code ec;
    const auto root_abs = std::filesystem::weakly_canonical(root, ec);
    if (ec) {
        error = "failed to resolve root: " + ec.message();
        return false;
    }

    resolved = std::filesystem::weakly_canonical(root_abs / relative, ec);
    if (ec) {
        resolved = std::filesystem::absolute(root_abs / relative, ec);
        if (ec) {
            error = "failed to resolve target path: " + ec.message();
            return false;
        }
    }

    std::string root_text = remote_lab::narrow_path(root_abs);
    std::string resolved_text = remote_lab::narrow_path(resolved);
    if (!root_text.empty() && root_text.back() != '\\' && root_text.back() != '/') {
        root_text.push_back('\\');
    }
    if (!starts_with_case_insensitive(resolved_text, root_text)) {
        error = "target path is outside the experiment root";
        return false;
    }
    return true;
}

std::string list_files(const std::filesystem::path& root) {
    std::ostringstream oss;
    oss << "Experiment C drive root: " << remote_lab::narrow_path(root) << "\n";
    oss << "Use the RelativePath column for delete/download commands.\n\n";
    oss << std::left << std::setw(8) << "Type" << std::setw(14) << "Size" << "RelativePath\n";
    oss << "------------------------------------------------------------\n";

    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) {
        oss << "(root does not exist)\n";
        return oss.str();
    }

    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        if (ec) {
            oss << "list error: " << ec.message() << "\n";
            return oss.str();
        }

        const bool is_dir = entry.is_directory(ec);
        const bool is_file = entry.is_regular_file(ec);
        std::uintmax_t size = 0;
        if (is_file) {
            size = entry.file_size(ec);
            if (ec) {
                size = 0;
                ec.clear();
            }
        }

        oss << std::left << std::setw(8) << (is_dir ? "DIR" : "FILE")
            << std::setw(14) << (is_dir ? "-" : std::to_string(size))
            << remote_lab::narrow_path(entry.path().filename()) << "\n";
        ++count;
    }

    if (count == 0) {
        oss << "(empty)\n";
    }
    return oss.str();
}

#pragma pack(push, 1)
struct BmpFileHeader {
    std::uint16_t type = 0x4D42;
    std::uint32_t size = 0;
    std::uint16_t reserved1 = 0;
    std::uint16_t reserved2 = 0;
    std::uint32_t offset = 0;
};
#pragma pack(pop)

bool capture_screen_bmp(std::vector<std::uint8_t>& output, std::string& error) {
    HDC screen_dc = GetDC(nullptr);
    if (screen_dc == nullptr) {
        error = "GetDC failed";
        return false;
    }

    const int width = GetSystemMetrics(SM_CXSCREEN);
    const int height = GetSystemMetrics(SM_CYSCREEN);
    HDC memory_dc = CreateCompatibleDC(screen_dc);
    if (memory_dc == nullptr) {
        ReleaseDC(nullptr, screen_dc);
        error = "CreateCompatibleDC failed";
        return false;
    }

    HBITMAP bitmap = CreateCompatibleBitmap(screen_dc, width, height);
    if (bitmap == nullptr) {
        DeleteDC(memory_dc);
        ReleaseDC(nullptr, screen_dc);
        error = "CreateCompatibleBitmap failed";
        return false;
    }

    HGDIOBJ old_object = SelectObject(memory_dc, bitmap);
    if (old_object == nullptr) {
        DeleteObject(bitmap);
        DeleteDC(memory_dc);
        ReleaseDC(nullptr, screen_dc);
        error = "SelectObject failed";
        return false;
    }

    if (!BitBlt(memory_dc, 0, 0, width, height, screen_dc, 0, 0, SRCCOPY)) {
        SelectObject(memory_dc, old_object);
        DeleteObject(bitmap);
        DeleteDC(memory_dc);
        ReleaseDC(nullptr, screen_dc);
        error = "BitBlt failed";
        return false;
    }

    BITMAPINFOHEADER info{};
    info.biSize = sizeof(BITMAPINFOHEADER);
    info.biWidth = width;
    info.biHeight = height;
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    info.biSizeImage = static_cast<DWORD>(width * height * 4);

    std::vector<std::uint8_t> pixels(info.biSizeImage);
    const int scan_lines = GetDIBits(memory_dc,
                                     bitmap,
                                     0,
                                     static_cast<UINT>(height),
                                     pixels.data(),
                                     reinterpret_cast<BITMAPINFO*>(&info),
                                     DIB_RGB_COLORS);

    SelectObject(memory_dc, old_object);
    DeleteObject(bitmap);
    DeleteDC(memory_dc);
    ReleaseDC(nullptr, screen_dc);

    if (scan_lines == 0) {
        error = "GetDIBits failed";
        return false;
    }

    BmpFileHeader file_header{};
    file_header.offset = sizeof(BmpFileHeader) + sizeof(BITMAPINFOHEADER);
    file_header.size = file_header.offset + static_cast<std::uint32_t>(pixels.size());

    output.resize(file_header.size);
    std::memcpy(output.data(), &file_header, sizeof(file_header));
    std::memcpy(output.data() + sizeof(file_header), &info, sizeof(info));
    std::memcpy(output.data() + file_header.offset, pixels.data(), pixels.size());
    return true;
}

bool reply_text(SOCKET client, MessageType type, const std::string& text) {
    std::string error;
    if (!remote_lab::send_packet(client, type, remote_lab::string_payload(text), error)) {
        log_event("reply failed: " + error);
        return false;
    }
    return true;
}

bool handle_packet(SOCKET client, const Packet& packet, const ServerConfig& config) {
    std::string error;
    switch (packet.type) {
    case MessageType::Echo: {
        const std::string text = remote_lab::payload_to_string(packet.payload);
        log_event("client says: " + text);
        return reply_text(client, MessageType::Ok, "server printed string: " + text);
    }
    case MessageType::Shutdown: {
        log_event("shutdown command received");
        const int code = std::system("shutdown /s /t 60");
        return reply_text(client, code == 0 ? MessageType::Ok : MessageType::Error,
                          code == 0 ? "server host will shut down in 60 seconds" : "shutdown command failed");
    }
    case MessageType::CancelShutdown: {
        log_event("cancel shutdown command received");
        const int code = std::system("shutdown /a");
        return reply_text(client, code == 0 ? MessageType::Ok : MessageType::Error,
                          code == 0 ? "pending shutdown was cancelled" : "cancel shutdown command failed");
    }
    case MessageType::ListFiles: {
        log_event("file list requested");
        return reply_text(client, MessageType::Text, list_files(config.root));
    }
    case MessageType::Screenshot: {
        log_event("screenshot requested");
        std::vector<std::uint8_t> bmp;
        if (!capture_screen_bmp(bmp, error)) {
            return reply_text(client, MessageType::Error, error);
        }
        return remote_lab::send_packet(client, MessageType::File, remote_lab::make_named_payload("server_screen.bmp", bmp), error)
            || (log_event("screenshot reply failed: " + error), false);
    }
    case MessageType::DeleteFile: {
        const std::string relative = remote_lab::payload_to_string(packet.payload);
        log_event("delete requested: " + relative);
        std::filesystem::path target;
        if (!resolve_safe_path(config.root, relative, target, error)) {
            return reply_text(client, MessageType::Error, error);
        }
        std::error_code ec;
        if (!std::filesystem::is_regular_file(target, ec)) {
            return reply_text(client, MessageType::Error, "target is not a regular file: " + relative);
        }
        if (!std::filesystem::remove(target, ec) || ec) {
            return reply_text(client, MessageType::Error, "delete failed: " + ec.message());
        }
        return reply_text(client, MessageType::Ok, "deleted: " + relative);
    }
    case MessageType::UploadFile: {
        std::string original_name;
        std::vector<std::uint8_t> data;
        if (!remote_lab::parse_named_payload(packet.payload, original_name, data, error)) {
            return reply_text(client, MessageType::Error, error);
        }
        const auto output_path = config.root / "myFile.txt";
        log_event("upload received from " + original_name + ", saving as " + remote_lab::narrow_path(output_path));
        if (!remote_lab::write_binary_file(output_path, data, error)) {
            return reply_text(client, MessageType::Error, error);
        }
        return reply_text(client, MessageType::Ok, "uploaded content saved to server file myFile.txt");
    }
    case MessageType::DownloadFile: {
        const std::string relative = remote_lab::payload_to_string(packet.payload);
        log_event("download requested: " + relative);
        std::filesystem::path target;
        if (!resolve_safe_path(config.root, relative, target, error)) {
            return reply_text(client, MessageType::Error, error);
        }
        if (!std::filesystem::is_regular_file(target)) {
            return reply_text(client, MessageType::Error, "target is not a regular file: " + relative);
        }
        std::vector<std::uint8_t> data;
        if (!remote_lab::read_binary_file(target, data, error)) {
            return reply_text(client, MessageType::Error, error);
        }
        return remote_lab::send_packet(client, MessageType::File, remote_lab::make_named_payload("myFile.txt", data), error)
            || (log_event("download reply failed: " + error), false);
    }
    case MessageType::Quit:
        log_event("client requested quit");
        return false;
    default:
        return reply_text(client, MessageType::Error, "unknown command type");
    }
}

void seed_lab_files(const std::filesystem::path& root) {
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (ec) {
        log_event("failed to create root: " + ec.message());
        return;
    }

    const auto sample = root / "sample_server.txt";
    if (!std::filesystem::exists(sample, ec)) {
        const std::string content =
            "This file was created for the remote lab experiment.\r\n"
            "It is safe to download or delete during the demo.\r\n";
        std::string error;
        remote_lab::write_binary_file(sample, remote_lab::string_payload(content), error);
        if (!error.empty()) {
            log_event(error);
        }
    }
}

SOCKET create_listen_socket(const ServerConfig& config) {
    SOCKET listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_socket == INVALID_SOCKET) {
        throw std::runtime_error(remote_lab::last_socket_error("socket"));
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(config.port);
    if (inet_pton(AF_INET, config.bind_host.c_str(), &address.sin_addr) != 1) {
        remote_lab::close_socket(listen_socket);
        throw std::runtime_error("invalid bind host: " + config.bind_host);
    }

    int opt = 1;
    setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    if (bind(listen_socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        const auto error = remote_lab::last_socket_error("bind");
        remote_lab::close_socket(listen_socket);
        throw std::runtime_error(error);
    }
    if (listen(listen_socket, SOMAXCONN) == SOCKET_ERROR) {
        const auto error = remote_lab::last_socket_error("listen");
        remote_lab::close_socket(listen_socket);
        throw std::runtime_error(error);
    }
    return listen_socket;
}

} // namespace

int main(int argc, char** argv) {
    try {
        ServerConfig config;
        if (!parse_args(argc, argv, config)) {
            return 0;
        }

        remote_lab::WsaSession wsa;
        if (!wsa.ok()) {
            std::cerr << wsa.error() << "\n";
            return 1;
        }

        seed_lab_files(config.root);
        SOCKET listen_socket = create_listen_socket(config);

        std::cout << "Remote Lab Server\n";
        std::cout << "Listening on " << config.bind_host << ":" << config.port << "\n";
        std::cout << "Experiment C drive root: " << remote_lab::narrow_path(config.root) << "\n";
        std::cout << "Shutdown mode: REAL 60-second shutdown\n";
        std::cout << "Press Ctrl+C to stop the server.\n\n";

        while (true) {
            sockaddr_in client_address{};
            int client_len = sizeof(client_address);
            SOCKET client = accept(listen_socket, reinterpret_cast<sockaddr*>(&client_address), &client_len);
            if (client == INVALID_SOCKET) {
                log_event(remote_lab::last_socket_error("accept"));
                continue;
            }

            char ip_text[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &client_address.sin_addr, ip_text, sizeof(ip_text));
            log_event(std::string("client connected: ") + ip_text);

            std::string error;
            Packet packet;
            while (remote_lab::receive_packet(client, packet, error)) {
                if (!handle_packet(client, packet, config)) {
                    break;
                }
            }
            if (!error.empty()) {
                log_event("client disconnected: " + error);
            }
            remote_lab::close_socket(client);
        }
    } catch (const std::exception& ex) {
        std::cerr << "server error: " << ex.what() << "\n";
        return 1;
    }
}
