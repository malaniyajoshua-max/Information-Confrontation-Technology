#include "common.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using remote_lab::MessageType;
using remote_lab::Packet;

namespace {

constexpr int IDC_HOST = 1001;
constexpr int IDC_PORT = 1002;
constexpr int IDC_ROOT = 1003;
constexpr int IDC_START = 1005;
constexpr int IDC_STOP = 1006;
constexpr int IDC_STATUS = 1007;
constexpr int IDC_LOG = 1008;
constexpr UINT WM_APP_LOG = WM_APP + 1;
constexpr UINT WM_APP_STATUS = WM_APP + 2;

struct ServerConfig {
    std::string bind_host = "127.0.0.1";
    unsigned short port = 5050;
    std::filesystem::path root = "C:\\TrojanLab\\server_files";
};

struct ServerGuiState {
    HWND window = nullptr;
    HWND host_edit = nullptr;
    HWND port_edit = nullptr;
    HWND root_edit = nullptr;
    HWND start_button = nullptr;
    HWND stop_button = nullptr;
    HWND status_label = nullptr;
    HWND log_edit = nullptr;

    std::atomic_bool running{false};
    SOCKET listen_socket = INVALID_SOCKET;
    SOCKET client_socket = INVALID_SOCKET;
    std::mutex socket_mutex;
    std::thread worker;
};

ServerGuiState g_state;

std::string get_window_text(HWND hwnd) {
    const int length = GetWindowTextLengthA(hwnd);
    std::string text(static_cast<std::size_t>(length), '\0');
    if (length > 0) {
        GetWindowTextA(hwnd, text.data(), length + 1);
    }
    return text;
}

void post_owned_message(UINT message, const std::string& text) {
    PostMessageA(g_state.window, message, 0, reinterpret_cast<LPARAM>(new std::string(text)));
}

void append_log(HWND edit, const std::string& text) {
    const int length = GetWindowTextLengthA(edit);
    SendMessageA(edit, EM_SETSEL, static_cast<WPARAM>(length), static_cast<LPARAM>(length));
    std::string line = text;
    line += "\r\n";
    SendMessageA(edit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
}

std::string current_time_text() {
    const auto now = std::chrono::system_clock::now();
    const auto time_value = std::chrono::system_clock::to_time_t(now);
    std::tm local_time{};
    localtime_s(&local_time, &time_value);

    std::ostringstream oss;
    oss << std::put_time(&local_time, "%H:%M:%S");
    return oss.str();
}

void log_event(const std::string& text) {
    post_owned_message(WM_APP_LOG, "[" + current_time_text() + "] " + text);
}

HMENU control_id(int id) {
    return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));
}

bool starts_with_case_insensitive(std::string text, std::string prefix) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(prefix.begin(), prefix.end(), prefix.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
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
        error = "absolute paths are not accepted";
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
        log_event("Output string: " + text);
        return reply_text(client, MessageType::Ok, "server printed string: " + text);
    }
    case MessageType::Shutdown: {
        log_event("Shutdown command received");
        const int code = std::system("shutdown /s /t 60");
        return reply_text(client, code == 0 ? MessageType::Ok : MessageType::Error,
                          code == 0 ? "server host will shut down in 60 seconds" : "shutdown command failed");
    }
    case MessageType::CancelShutdown: {
        log_event("Cancel shutdown command received");
        const int code = std::system("shutdown /a");
        return reply_text(client, code == 0 ? MessageType::Ok : MessageType::Error,
                          code == 0 ? "pending shutdown was cancelled" : "cancel shutdown command failed");
    }
    case MessageType::ListFiles:
        log_event("File list requested");
        return reply_text(client, MessageType::Text, list_files(config.root));
    case MessageType::Screenshot: {
        log_event("Screenshot requested");
        std::vector<std::uint8_t> bmp;
        if (!capture_screen_bmp(bmp, error)) {
            return reply_text(client, MessageType::Error, error);
        }
        const auto payload = remote_lab::make_named_payload("server_screen.bmp", bmp);
        if (!remote_lab::send_packet(client, MessageType::File, payload, error)) {
            log_event("screenshot reply failed: " + error);
            return false;
        }
        return true;
    }
    case MessageType::DeleteFile: {
        const std::string relative = remote_lab::payload_to_string(packet.payload);
        log_event("Delete requested: " + relative);
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
        log_event("Upload received from " + original_name + ", saving as " + remote_lab::narrow_path(output_path));
        if (!remote_lab::write_binary_file(output_path, data, error)) {
            return reply_text(client, MessageType::Error, error);
        }
        return reply_text(client, MessageType::Ok, "uploaded content saved to server file myFile.txt");
    }
    case MessageType::DownloadFile: {
        const std::string relative = remote_lab::payload_to_string(packet.payload);
        log_event("Download requested: " + relative);
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
        const auto payload = remote_lab::make_named_payload("myFile.txt", data);
        if (!remote_lab::send_packet(client, MessageType::File, payload, error)) {
            log_event("download reply failed: " + error);
            return false;
        }
        return true;
    }
    case MessageType::Quit:
        log_event("Client requested disconnect");
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

void worker_thread(ServerConfig config) {
    try {
        seed_lab_files(config.root);
        SOCKET listen_socket = create_listen_socket(config);
        {
            std::lock_guard<std::mutex> lock(g_state.socket_mutex);
            g_state.listen_socket = listen_socket;
        }

        post_owned_message(WM_APP_STATUS, "Listening");
        log_event("Listening on " + config.bind_host + ":" + std::to_string(config.port));
        log_event("Experiment root: " + remote_lab::narrow_path(config.root));
        log_event("Real shutdown enabled: shutdown /s /t 60");

        while (g_state.running.load()) {
            sockaddr_in client_address{};
            int client_len = sizeof(client_address);
            SOCKET client = accept(listen_socket, reinterpret_cast<sockaddr*>(&client_address), &client_len);
            if (client == INVALID_SOCKET) {
                if (g_state.running.load()) {
                    log_event(remote_lab::last_socket_error("accept"));
                }
                break;
            }

            {
                std::lock_guard<std::mutex> lock(g_state.socket_mutex);
                g_state.client_socket = client;
            }

            char ip_text[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &client_address.sin_addr, ip_text, sizeof(ip_text));
            post_owned_message(WM_APP_STATUS, "Client connected");
            log_event(std::string("Client connected: ") + ip_text);

            std::string error;
            Packet packet;
            while (g_state.running.load() && remote_lab::receive_packet(client, packet, error)) {
                if (!handle_packet(client, packet, config)) {
                    break;
                }
            }

            if (!error.empty() && g_state.running.load()) {
                log_event("Client disconnected: " + error);
            }
            remote_lab::close_socket(client);
            {
                std::lock_guard<std::mutex> lock(g_state.socket_mutex);
                g_state.client_socket = INVALID_SOCKET;
            }
            if (g_state.running.load()) {
                post_owned_message(WM_APP_STATUS, "Listening");
            }
        }

        remote_lab::close_socket(listen_socket);
        {
            std::lock_guard<std::mutex> lock(g_state.socket_mutex);
            g_state.listen_socket = INVALID_SOCKET;
        }
    } catch (const std::exception& ex) {
        log_event(std::string("Server error: ") + ex.what());
    }

    g_state.running.store(false);
    post_owned_message(WM_APP_STATUS, "Stopped");
    EnableWindow(g_state.start_button, TRUE);
    EnableWindow(g_state.stop_button, FALSE);
}

void stop_server() {
    const bool was_running = g_state.running.exchange(false);
    if (was_running) {
        std::lock_guard<std::mutex> lock(g_state.socket_mutex);
        remote_lab::close_socket(g_state.client_socket);
        remote_lab::close_socket(g_state.listen_socket);
        g_state.client_socket = INVALID_SOCKET;
        g_state.listen_socket = INVALID_SOCKET;
    }
    if (g_state.worker.joinable() && g_state.worker.get_id() != std::this_thread::get_id()) {
        g_state.worker.join();
    }
}

bool read_config(ServerConfig& config, std::string& error) {
    config.bind_host = get_window_text(g_state.host_edit);
    const std::string port_text = get_window_text(g_state.port_edit);
    config.root = get_window_text(g_state.root_edit);

    try {
        const int port_value = std::stoi(port_text);
        if (port_value <= 0 || port_value > 65535) {
            error = "port must be in 1..65535";
            return false;
        }
        config.port = static_cast<unsigned short>(port_value);
    } catch (...) {
        error = "invalid port";
        return false;
    }

    if (config.bind_host.empty()) {
        error = "host cannot be empty";
        return false;
    }
    if (config.root.empty()) {
        error = "root directory cannot be empty";
        return false;
    }
    return true;
}

void start_server() {
    if (g_state.running.load()) {
        return;
    }

    ServerConfig config;
    std::string error;
    if (!read_config(config, error)) {
        MessageBoxA(g_state.window, error.c_str(), "Invalid configuration", MB_ICONERROR);
        return;
    }

    g_state.running.store(true);
    EnableWindow(g_state.start_button, FALSE);
    EnableWindow(g_state.stop_button, TRUE);
    SetWindowTextA(g_state.status_label, "Starting");
    g_state.worker = std::thread(worker_thread, config);
}

HWND make_label(HWND parent, const char* text, int x, int y, int w, int h) {
    return CreateWindowExA(0, "STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, h, parent, nullptr, nullptr, nullptr);
}

HWND make_edit(HWND parent, int id, const char* text, int x, int y, int w, int h, DWORD extra_style = 0) {
    return CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", text,
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | extra_style,
                           x, y, w, h, parent, control_id(id), nullptr, nullptr);
}

void create_controls(HWND hwnd) {
    make_label(hwnd, "Bind IP", 16, 18, 60, 22);
    g_state.host_edit = make_edit(hwnd, IDC_HOST, "127.0.0.1", 86, 14, 130, 24);
    make_label(hwnd, "Port", 230, 18, 40, 22);
    g_state.port_edit = make_edit(hwnd, IDC_PORT, "5050", 274, 14, 70, 24);
    make_label(hwnd, "Root", 16, 52, 60, 22);
    g_state.root_edit = make_edit(hwnd, IDC_ROOT, "C:\\TrojanLab\\server_files", 86, 48, 608, 24);

    g_state.start_button = CreateWindowExA(0, "BUTTON", "Start server",
                                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                           464, 12, 110, 28, hwnd,
                                           control_id(IDC_START), nullptr, nullptr);
    g_state.stop_button = CreateWindowExA(0, "BUTTON", "Stop server",
                                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                          584, 12, 110, 28, hwnd,
                                          control_id(IDC_STOP), nullptr, nullptr);
    EnableWindow(g_state.stop_button, FALSE);

    make_label(hwnd, "Status", 16, 86, 60, 22);
    g_state.status_label = CreateWindowExA(WS_EX_CLIENTEDGE, "STATIC", "Stopped",
                                           WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                                           86, 82, 608, 26, hwnd,
                                           control_id(IDC_STATUS), nullptr, nullptr);

    make_label(hwnd, "Server log", 16, 122, 120, 22);
    g_state.log_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                                       WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE |
                                           ES_AUTOVSCROLL | ES_READONLY,
                                       16, 146, 678, 350, hwnd,
                                       control_id(IDC_LOG), nullptr, nullptr);
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE:
        g_state.window = hwnd;
        create_controls(hwnd);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case IDC_START:
            start_server();
            return 0;
        case IDC_STOP:
            stop_server();
            return 0;
        default:
            return 0;
        }
    case WM_APP_LOG: {
        std::unique_ptr<std::string> text(reinterpret_cast<std::string*>(lparam));
        append_log(g_state.log_edit, *text);
        return 0;
    }
    case WM_APP_STATUS: {
        std::unique_ptr<std::string> text(reinterpret_cast<std::string*>(lparam));
        SetWindowTextA(g_state.status_label, text->c_str());
        return 0;
    }
    case WM_CLOSE:
        stop_server();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        stop_server();
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcA(hwnd, message, wparam, lparam);
    }
}

} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show_command) {
    remote_lab::WsaSession wsa;
    if (!wsa.ok()) {
        MessageBoxA(nullptr, wsa.error().c_str(), "WinSock error", MB_ICONERROR);
        return 1;
    }

    const char* class_name = "RemoteLabServerGui";
    WNDCLASSA wc{};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.lpszClassName = class_name;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

    if (!RegisterClassA(&wc)) {
        MessageBoxA(nullptr, "RegisterClass failed", "Error", MB_ICONERROR);
        return 1;
    }

    HWND hwnd = CreateWindowExA(0, class_name, "Remote Lab Server",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, 730, 560,
                                nullptr, nullptr, instance, nullptr);
    if (hwnd == nullptr) {
        MessageBoxA(nullptr, "CreateWindow failed", "Error", MB_ICONERROR);
        return 1;
    }

    ShowWindow(hwnd, show_command);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return static_cast<int>(msg.wParam);
}
