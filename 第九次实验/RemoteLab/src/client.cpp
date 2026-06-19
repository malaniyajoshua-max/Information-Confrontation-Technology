#include "common.hpp"

#include <filesystem>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

using remote_lab::MessageType;
using remote_lab::Packet;

namespace {

struct ClientConfig {
    std::string host = "127.0.0.1";
    unsigned short port = 5050;
};

void print_usage() {
    std::cout << "remote_lab_client [server_ip] [port]\n"
              << "Example: remote_lab_client 127.0.0.1 5050\n";
}

bool parse_args(int argc, char** argv, ClientConfig& config) {
    if (argc >= 2) {
        const std::string first = argv[1];
        if (first == "--help" || first == "-h") {
            print_usage();
            return false;
        }
        config.host = first;
    }
    if (argc >= 3) {
        const int port_value = std::stoi(argv[2]);
        if (port_value <= 0 || port_value > 65535) {
            throw std::runtime_error("port must be in 1..65535");
        }
        config.port = static_cast<unsigned short>(port_value);
    }
    return true;
}

SOCKET connect_to_server(const ClientConfig& config) {
    SOCKET socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_handle == INVALID_SOCKET) {
        throw std::runtime_error(remote_lab::last_socket_error("socket"));
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(config.port);
    if (inet_pton(AF_INET, config.host.c_str(), &address.sin_addr) != 1) {
        remote_lab::close_socket(socket_handle);
        throw std::runtime_error("invalid server IP: " + config.host);
    }

    if (connect(socket_handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        const auto error = remote_lab::last_socket_error("connect");
        remote_lab::close_socket(socket_handle);
        throw std::runtime_error(error);
    }
    return socket_handle;
}

std::string prompt_line(const std::string& prompt) {
    std::cout << prompt;
    std::string line;
    std::getline(std::cin, line);
    return line;
}

bool request_text(SOCKET socket_handle, MessageType type, const std::string& payload_text = {}) {
    std::string error;
    if (!remote_lab::send_packet(socket_handle, type, remote_lab::string_payload(payload_text), error)) {
        std::cout << "Send failed: " << error << "\n";
        return false;
    }

    Packet reply;
    if (!remote_lab::receive_packet(socket_handle, reply, error)) {
        std::cout << "Receive failed: " << error << "\n";
        return false;
    }

    const std::string text = remote_lab::payload_to_string(reply.payload);
    if (reply.type == MessageType::Error) {
        std::cout << "Server error: " << text << "\n";
        return true;
    }

    std::cout << text << "\n";
    return true;
}

bool request_file(SOCKET socket_handle,
                  MessageType type,
                  const std::string& payload_text,
                  const std::filesystem::path& output_path_override = {}) {
    std::string error;
    if (!remote_lab::send_packet(socket_handle, type, remote_lab::string_payload(payload_text), error)) {
        std::cout << "Send failed: " << error << "\n";
        return false;
    }

    Packet reply;
    if (!remote_lab::receive_packet(socket_handle, reply, error)) {
        std::cout << "Receive failed: " << error << "\n";
        return false;
    }
    if (reply.type == MessageType::Error) {
        std::cout << "Server error: " << remote_lab::payload_to_string(reply.payload) << "\n";
        return true;
    }
    if (reply.type != MessageType::File) {
        std::cout << "Unexpected reply type.\n";
        return true;
    }

    std::string name;
    std::vector<std::uint8_t> data;
    if (!remote_lab::parse_named_payload(reply.payload, name, data, error)) {
        std::cout << "Invalid file reply: " << error << "\n";
        return true;
    }

    std::filesystem::path output_path = output_path_override.empty()
        ? std::filesystem::current_path() / name
        : output_path_override;

    if (!remote_lab::write_binary_file(output_path, data, error)) {
        std::cout << "Save failed: " << error << "\n";
        return true;
    }

    std::cout << "Saved " << data.size() << " bytes to " << remote_lab::narrow_path(output_path) << "\n";
    return true;
}

bool upload_file(SOCKET socket_handle) {
    const std::string path_text = prompt_line("Local file path to upload: ");
    if (path_text.empty()) {
        std::cout << "No file selected.\n";
        return true;
    }

    std::filesystem::path input_path(path_text);
    std::vector<std::uint8_t> data;
    std::string error;
    if (!remote_lab::read_binary_file(input_path, data, error)) {
        std::cout << error << "\n";
        return true;
    }

    const std::string original_name = remote_lab::narrow_path(input_path.filename());
    const auto payload = remote_lab::make_named_payload(original_name, data);
    if (!remote_lab::send_packet(socket_handle, MessageType::UploadFile, payload, error)) {
        std::cout << "Send failed: " << error << "\n";
        return false;
    }

    Packet reply;
    if (!remote_lab::receive_packet(socket_handle, reply, error)) {
        std::cout << "Receive failed: " << error << "\n";
        return false;
    }
    std::cout << remote_lab::payload_to_string(reply.payload) << "\n";
    return true;
}

void print_menu() {
    std::cout
        << "\n==== Remote Lab Client ====\n"
        << "1. Output string on server\n"
        << "2. Shutdown server host in 60 seconds\n"
        << "3. Cancel pending server shutdown\n"
        << "4. Get server C-drive experiment file list\n"
        << "5. Capture server desktop screenshot\n"
        << "6. Delete a selected server file\n"
        << "7. Upload local file to server myFile.txt\n"
        << "8. Download selected server file as myFile.txt\n"
        << "0. Quit\n"
        << "Select: ";
}

} // namespace

int main(int argc, char** argv) {
    try {
        ClientConfig config;
        if (!parse_args(argc, argv, config)) {
            return 0;
        }

        remote_lab::WsaSession wsa;
        if (!wsa.ok()) {
            std::cerr << wsa.error() << "\n";
            return 1;
        }

        SOCKET socket_handle = connect_to_server(config);
        std::cout << "Connected to " << config.host << ":" << config.port << "\n";

        bool running = true;
        while (running) {
            print_menu();
            std::string choice;
            std::getline(std::cin, choice);

            if (choice == "1") {
                const std::string text = prompt_line("String to print on server: ");
                running = request_text(socket_handle, MessageType::Echo, text);
            } else if (choice == "2") {
                running = request_text(socket_handle, MessageType::Shutdown);
            } else if (choice == "3") {
                running = request_text(socket_handle, MessageType::CancelShutdown);
            } else if (choice == "4") {
                running = request_text(socket_handle, MessageType::ListFiles);
            } else if (choice == "5") {
                const std::string save_path = prompt_line("Save screenshot path [server_screen.bmp]: ");
                const std::filesystem::path output = save_path.empty()
                    ? std::filesystem::current_path() / "server_screen.bmp"
                    : std::filesystem::path(save_path);
                running = request_file(socket_handle, MessageType::Screenshot, {}, output);
            } else if (choice == "6") {
                const std::string relative = prompt_line("RelativePath from list to delete: ");
                running = request_text(socket_handle, MessageType::DeleteFile, relative);
            } else if (choice == "7") {
                running = upload_file(socket_handle);
            } else if (choice == "8") {
                const std::string relative = prompt_line("RelativePath from list to download: ");
                const std::string output_dir = prompt_line("Save directory [.]: ");
                const std::filesystem::path output_path = (output_dir.empty()
                    ? std::filesystem::current_path()
                    : std::filesystem::path(output_dir)) / "myFile.txt";
                running = request_file(socket_handle, MessageType::DownloadFile, relative, output_path);
            } else if (choice == "0") {
                std::string error;
                remote_lab::send_packet(socket_handle, MessageType::Quit, {}, error);
                running = false;
            } else {
                std::cout << "Unknown selection.\n";
            }
        }

        remote_lab::close_socket(socket_handle);
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "client error: " << ex.what() << "\n";
        return 1;
    }
}
