#include "common.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <commdlg.h>

#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using remote_lab::MessageType;
using remote_lab::Packet;

namespace {

constexpr int IDC_HOST = 2001;
constexpr int IDC_PORT = 2002;
constexpr int IDC_CONNECT = 2003;
constexpr int IDC_DISCONNECT = 2004;
constexpr int IDC_STATUS = 2005;
constexpr int IDC_MESSAGE = 2006;
constexpr int IDC_SEND_MESSAGE = 2007;
constexpr int IDC_SHUTDOWN = 2008;
constexpr int IDC_CANCEL_SHUTDOWN = 2009;
constexpr int IDC_REFRESH_LIST = 2010;
constexpr int IDC_FILE_LIST = 2011;
constexpr int IDC_SCREENSHOT = 2012;
constexpr int IDC_UPLOAD = 2013;
constexpr int IDC_DOWNLOAD = 2014;
constexpr int IDC_DELETE = 2015;
constexpr int IDC_LOG = 2016;

struct ClientGuiState {
    HWND window = nullptr;
    HWND host_edit = nullptr;
    HWND port_edit = nullptr;
    HWND connect_button = nullptr;
    HWND disconnect_button = nullptr;
    HWND status_label = nullptr;
    HWND message_edit = nullptr;
    HWND file_list = nullptr;
    HWND log_edit = nullptr;

    SOCKET socket_handle = INVALID_SOCKET;
};

ClientGuiState g_state;

std::string get_window_text(HWND hwnd) {
    const int length = GetWindowTextLengthA(hwnd);
    std::string text(static_cast<std::size_t>(length), '\0');
    if (length > 0) {
        GetWindowTextA(hwnd, text.data(), length + 1);
    }
    return text;
}

void append_log(const std::string& text) {
    const int length = GetWindowTextLengthA(g_state.log_edit);
    SendMessageA(g_state.log_edit, EM_SETSEL, static_cast<WPARAM>(length), static_cast<LPARAM>(length));
    std::string line = text;
    line += "\r\n";
    SendMessageA(g_state.log_edit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
}

void set_connected_state(bool connected) {
    SetWindowTextA(g_state.status_label, connected ? "Connected" : "Disconnected");
    EnableWindow(g_state.connect_button, connected ? FALSE : TRUE);
    EnableWindow(g_state.disconnect_button, connected ? TRUE : FALSE);
}

void show_error(const std::string& text) {
    MessageBoxA(g_state.window, text.c_str(), "Remote Lab Client", MB_ICONERROR);
    append_log("Error: " + text);
}

HMENU control_id(int id) {
    return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));
}

SOCKET connect_to_server(const std::string& host, unsigned short port) {
    SOCKET socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_handle == INVALID_SOCKET) {
        throw std::runtime_error(remote_lab::last_socket_error("socket"));
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
        remote_lab::close_socket(socket_handle);
        throw std::runtime_error("invalid server IP: " + host);
    }

    if (connect(socket_handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        const auto error = remote_lab::last_socket_error("connect");
        remote_lab::close_socket(socket_handle);
        throw std::runtime_error(error);
    }
    return socket_handle;
}

bool ensure_connected() {
    if (g_state.socket_handle == INVALID_SOCKET) {
        show_error("Connect to the server first.");
        return false;
    }
    return true;
}

bool send_request(MessageType type, const std::vector<std::uint8_t>& payload, Packet& reply) {
    if (!ensure_connected()) {
        return false;
    }

    std::string error;
    if (!remote_lab::send_packet(g_state.socket_handle, type, payload, error)) {
        show_error("Send failed: " + error);
        return false;
    }
    if (!remote_lab::receive_packet(g_state.socket_handle, reply, error)) {
        show_error("Receive failed: " + error);
        return false;
    }
    return true;
}

bool send_text_command(MessageType type, const std::string& payload_text = {}) {
    Packet reply;
    if (!send_request(type, remote_lab::string_payload(payload_text), reply)) {
        return false;
    }

    const std::string text = remote_lab::payload_to_string(reply.payload);
    if (reply.type == MessageType::Error) {
        show_error(text);
    } else {
        append_log(text);
    }
    return reply.type != MessageType::Error;
}

std::string trim_left(std::string text) {
    const auto pos = text.find_first_not_of(" \t\r\n");
    if (pos == std::string::npos) {
        return {};
    }
    return text.substr(pos);
}

void update_file_list_from_text(const std::string& text) {
    SendMessageA(g_state.file_list, LB_RESETCONTENT, 0, 0);

    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream row(line);
        std::string type;
        std::string size;
        row >> type >> size;
        if (type != "FILE" && type != "DIR") {
            continue;
        }

        std::string rest;
        std::getline(row, rest);
        rest = trim_left(rest);
        if (!rest.empty()) {
            SendMessageA(g_state.file_list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(rest.c_str()));
        }
    }
}

std::string selected_file_name() {
    const LRESULT index = SendMessageA(g_state.file_list, LB_GETCURSEL, 0, 0);
    if (index == LB_ERR) {
        return {};
    }

    const LRESULT length = SendMessageA(g_state.file_list, LB_GETTEXTLEN, static_cast<WPARAM>(index), 0);
    if (length == LB_ERR) {
        return {};
    }

    std::string text(static_cast<std::size_t>(length), '\0');
    SendMessageA(g_state.file_list, LB_GETTEXT, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(text.data()));
    return text;
}

bool choose_open_file(std::string& path) {
    char buffer[MAX_PATH] = {};
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_state.window;
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = "All Files\0*.*\0Text Files\0*.txt\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
    if (!GetOpenFileNameA(&ofn)) {
        return false;
    }
    path = buffer;
    return true;
}

bool choose_save_file(const char* default_name, std::string& path) {
    char buffer[MAX_PATH] = {};
    lstrcpynA(buffer, default_name, MAX_PATH);

    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_state.window;
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = "All Files\0*.*\0Bitmap Files\0*.bmp\0Text Files\0*.txt\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameA(&ofn)) {
        return false;
    }
    path = buffer;
    return true;
}

void connect_action() {
    if (g_state.socket_handle != INVALID_SOCKET) {
        return;
    }

    try {
        const std::string host = get_window_text(g_state.host_edit);
        const int port_value = std::stoi(get_window_text(g_state.port_edit));
        if (port_value <= 0 || port_value > 65535) {
            throw std::runtime_error("port must be in 1..65535");
        }

        g_state.socket_handle = connect_to_server(host, static_cast<unsigned short>(port_value));
        set_connected_state(true);
        append_log("Connected to " + host + ":" + std::to_string(port_value));
    } catch (const std::exception& ex) {
        show_error(ex.what());
    }
}

void disconnect_action() {
    if (g_state.socket_handle == INVALID_SOCKET) {
        return;
    }

    std::string error;
    remote_lab::send_packet(g_state.socket_handle, MessageType::Quit, {}, error);
    remote_lab::close_socket(g_state.socket_handle);
    g_state.socket_handle = INVALID_SOCKET;
    set_connected_state(false);
    append_log("Disconnected.");
}

void send_message_action() {
    const std::string text = get_window_text(g_state.message_edit);
    if (text.empty()) {
        show_error("Enter a string first.");
        return;
    }
    send_text_command(MessageType::Echo, text);
}

void refresh_list_action() {
    Packet reply;
    if (!send_request(MessageType::ListFiles, {}, reply)) {
        return;
    }

    const std::string text = remote_lab::payload_to_string(reply.payload);
    if (reply.type == MessageType::Error) {
        show_error(text);
        return;
    }
    append_log(text);
    update_file_list_from_text(text);
}

void screenshot_action() {
    std::string output_path;
    if (!choose_save_file("server_screen.bmp", output_path)) {
        return;
    }

    Packet reply;
    if (!send_request(MessageType::Screenshot, {}, reply)) {
        return;
    }
    if (reply.type == MessageType::Error) {
        show_error(remote_lab::payload_to_string(reply.payload));
        return;
    }

    std::string name;
    std::vector<std::uint8_t> data;
    std::string error;
    if (!remote_lab::parse_named_payload(reply.payload, name, data, error)) {
        show_error(error);
        return;
    }
    if (!remote_lab::write_binary_file(output_path, data, error)) {
        show_error(error);
        return;
    }
    append_log("Screenshot saved to " + output_path);
}

void upload_action() {
    std::string input_path;
    if (!choose_open_file(input_path)) {
        return;
    }

    std::vector<std::uint8_t> data;
    std::string error;
    if (!remote_lab::read_binary_file(input_path, data, error)) {
        show_error(error);
        return;
    }

    const std::filesystem::path path(input_path);
    const auto payload = remote_lab::make_named_payload(remote_lab::narrow_path(path.filename()), data);
    Packet reply;
    if (!send_request(MessageType::UploadFile, payload, reply)) {
        return;
    }

    const std::string text = remote_lab::payload_to_string(reply.payload);
    if (reply.type == MessageType::Error) {
        show_error(text);
    } else {
        append_log(text);
        refresh_list_action();
    }
}

void download_action() {
    const std::string relative = selected_file_name();
    if (relative.empty()) {
        show_error("Select a server file first.");
        return;
    }

    std::string output_path;
    if (!choose_save_file("myFile.txt", output_path)) {
        return;
    }

    Packet reply;
    if (!send_request(MessageType::DownloadFile, remote_lab::string_payload(relative), reply)) {
        return;
    }
    if (reply.type == MessageType::Error) {
        show_error(remote_lab::payload_to_string(reply.payload));
        return;
    }

    std::string name;
    std::vector<std::uint8_t> data;
    std::string error;
    if (!remote_lab::parse_named_payload(reply.payload, name, data, error)) {
        show_error(error);
        return;
    }
    if (!remote_lab::write_binary_file(output_path, data, error)) {
        show_error(error);
        return;
    }
    append_log("Downloaded " + relative + " to " + output_path);
}

void delete_action() {
    const std::string relative = selected_file_name();
    if (relative.empty()) {
        show_error("Select a server file first.");
        return;
    }
    const std::string prompt = "Delete server file '" + relative + "'?";
    if (MessageBoxA(g_state.window, prompt.c_str(), "Confirm delete", MB_ICONWARNING | MB_YESNO) != IDYES) {
        return;
    }

    if (send_text_command(MessageType::DeleteFile, relative)) {
        refresh_list_action();
    }
}

HWND make_label(HWND parent, const char* text, int x, int y, int w, int h) {
    return CreateWindowExA(0, "STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, h, parent, nullptr, nullptr, nullptr);
}

HWND make_edit(HWND parent, int id, const char* text, int x, int y, int w, int h, DWORD extra_style = 0) {
    return CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", text,
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | extra_style,
                           x, y, w, h, parent, control_id(id), nullptr, nullptr);
}

HWND make_button(HWND parent, int id, const char* text, int x, int y, int w, int h) {
    return CreateWindowExA(0, "BUTTON", text,
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                           x, y, w, h, parent, control_id(id), nullptr, nullptr);
}

void create_controls(HWND hwnd) {
    make_label(hwnd, "Server IP", 16, 18, 70, 22);
    g_state.host_edit = make_edit(hwnd, IDC_HOST, "127.0.0.1", 92, 14, 130, 24);
    make_label(hwnd, "Port", 236, 18, 40, 22);
    g_state.port_edit = make_edit(hwnd, IDC_PORT, "5050", 280, 14, 70, 24);
    g_state.connect_button = make_button(hwnd, IDC_CONNECT, "Connect", 368, 12, 90, 28);
    g_state.disconnect_button = make_button(hwnd, IDC_DISCONNECT, "Disconnect", 468, 12, 100, 28);
    EnableWindow(g_state.disconnect_button, FALSE);
    g_state.status_label = CreateWindowExA(WS_EX_CLIENTEDGE, "STATIC", "Disconnected",
                                           WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                                           584, 14, 130, 24, hwnd,
                                           control_id(IDC_STATUS), nullptr, nullptr);

    make_label(hwnd, "String", 16, 58, 70, 22);
    g_state.message_edit = make_edit(hwnd, IDC_MESSAGE, "hello server", 92, 54, 432, 24);
    make_button(hwnd, IDC_SEND_MESSAGE, "Send string", 540, 52, 174, 28);

    make_button(hwnd, IDC_SHUTDOWN, "Shutdown in 60s", 16, 94, 150, 32);
    make_button(hwnd, IDC_CANCEL_SHUTDOWN, "Cancel shutdown", 176, 94, 150, 32);
    make_button(hwnd, IDC_REFRESH_LIST, "Refresh file list", 336, 94, 150, 32);
    make_button(hwnd, IDC_SCREENSHOT, "Screenshot", 496, 94, 102, 32);
    make_button(hwnd, IDC_UPLOAD, "Upload", 608, 94, 106, 32);

    make_label(hwnd, "Server files", 16, 140, 120, 22);
    g_state.file_list = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", "",
                                        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY,
                                        16, 164, 300, 318, hwnd,
                                        control_id(IDC_FILE_LIST), nullptr, nullptr);
    make_button(hwnd, IDC_DOWNLOAD, "Download selected", 16, 494, 145, 32);
    make_button(hwnd, IDC_DELETE, "Delete selected", 171, 494, 145, 32);

    make_label(hwnd, "Result log", 336, 140, 120, 22);
    g_state.log_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                                       WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE |
                                           ES_AUTOVSCROLL | ES_READONLY,
                                       336, 164, 378, 362, hwnd,
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
        case IDC_CONNECT:
            connect_action();
            return 0;
        case IDC_DISCONNECT:
            disconnect_action();
            return 0;
        case IDC_SEND_MESSAGE:
            send_message_action();
            return 0;
        case IDC_SHUTDOWN:
            send_text_command(MessageType::Shutdown);
            return 0;
        case IDC_CANCEL_SHUTDOWN:
            send_text_command(MessageType::CancelShutdown);
            return 0;
        case IDC_REFRESH_LIST:
            refresh_list_action();
            return 0;
        case IDC_SCREENSHOT:
            screenshot_action();
            return 0;
        case IDC_UPLOAD:
            upload_action();
            return 0;
        case IDC_DOWNLOAD:
            download_action();
            return 0;
        case IDC_DELETE:
            delete_action();
            return 0;
        default:
            return 0;
        }
    case WM_CLOSE:
        disconnect_action();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        disconnect_action();
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

    const char* class_name = "RemoteLabClientGui";
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

    HWND hwnd = CreateWindowExA(0, class_name, "Remote Lab Client",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, 750, 585,
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
