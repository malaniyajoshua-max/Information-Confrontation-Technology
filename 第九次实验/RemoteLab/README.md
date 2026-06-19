# Remote Lab Trojan Experiment

This is a coursework implementation of a controlled remote command experiment.
It uses a complete C++17 console project, TCP sockets, explicit server startup,
and a small custom packet protocol.

The program is intended for a controlled classroom or VM lab. It does not include
stealth startup, persistence, privilege bypass, or background hiding.

## Features

The client menu maps to the experiment requirements:

1. Output a string on the server.
2. Ask the server host to shut down in 60 seconds.
3. Cancel a pending server shutdown.
4. Get the server-side C-drive experiment file list.
5. Capture the server desktop screenshot and save it on the client.
6. Delete a selected server-side file.
7. Upload a selected client file and save it on the server as `myFile.txt`.
8. Download a selected server file and save it on the client as `myFile.txt`.

## Safety Model

By default, file operations are restricted to:

```text
C:\TrojanLab\server_files
```

Treat this directory as the experiment's "server C-drive file list". The server
creates `sample_server.txt` automatically so the list/download/delete features
can be demonstrated without touching important system files.

The shutdown feature is real and uses a 60-second delay. Test it only inside a
disposable VM. The client cancel command runs:

```text
shutdown /s /t 60
shutdown /a
```

## Build With Visual Studio

Open a Developer PowerShell or Developer Command Prompt for Visual Studio and run:

```bat
cd C:\Users\hp\Downloads\RemoteLab
build_msvc.bat
```

Generated files:

```text
build\remote_lab_server.exe
build\remote_lab_client.exe
build\remote_lab_server_gui.exe
build\remote_lab_client_gui.exe
```

## Build With CMake

If CMake is installed:

```bat
cmake -S . -B build-cmake -A x64
cmake --build build-cmake --config Release
```

## Run On One Machine

Recommended GUI mode:

1. Double-click `build\remote_lab_server_gui.exe`.
2. Keep `Bind IP` as `127.0.0.1`, `Port` as `5050`, and click `Start server`.
3. Double-click `build\remote_lab_client_gui.exe`.
4. Keep `Server IP` as `127.0.0.1`, `Port` as `5050`, and click `Connect`.

Console fallback, terminal 1:

```bat
build\remote_lab_server.exe --host 127.0.0.1 --port 5050 --root C:\TrojanLab\server_files
```

Console fallback, terminal 2:

```bat
build\remote_lab_client.exe 127.0.0.1 5050
```

## Run In A VM Lab

1. Copy the project folder into the Windows 10 x64 VM.
2. Build it inside the VM with `build_msvc.bat`.
3. Start the GUI server inside the VM:

```bat
build\remote_lab_server_gui.exe
```

Set `Bind IP` to `0.0.0.0`, keep port `5050`, and click `Start server`.

Console fallback:

```bat
build\remote_lab_server.exe --host 0.0.0.0 --port 5050 --root C:\TrojanLab\server_files
```

4. Find the VM IP with:

```bat
ipconfig
```

5. Start the GUI client on the host or another VM:

```bat
build\remote_lab_client_gui.exe
```

Set `Server IP` to the VM IP address, keep port `5050`, and click `Connect`.

Console fallback:

```bat
build\remote_lab_client.exe VM_IP_ADDRESS 5050
```

6. If Windows Firewall blocks the server port, allow TCP port `5050` only inside
   the VM lab network.

For the 60-second shutdown demo in a disposable VM, start the server normally:

```bat
build\remote_lab_server.exe --host 0.0.0.0 --port 5050 --root C:\TrojanLab\server_files
```

In GUI mode, click `Shutdown in 60s` on the client, then click `Cancel shutdown`
within 60 seconds to run `shutdown /a`.

## Protocol Summary

Each TCP message has a 12-byte header:

```text
magic:   4 bytes, "RLB1"
type:    4 bytes, big-endian uint32
length:  4 bytes, big-endian uint32
payload: length bytes
```

Large binary data such as screenshots and downloaded files are transferred as a
named payload:

```text
name_length: 4 bytes, big-endian uint32
name:        name_length bytes
data:        remaining bytes
```

The packet format keeps command types clear, preserves exact file sizes, and
uses a bounded maximum payload size.
