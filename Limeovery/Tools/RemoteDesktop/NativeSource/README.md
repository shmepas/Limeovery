# Native Remote Desktop Host and Viewer

This folder contains the C++ source used to build the bundled `holokostya.exe` and `Viewer.exe`.

## Build

Open `NativeRemoteDesktop.sln` in Visual Studio 2022 with the Desktop development with C++ workload, the MSVC v143 toolset, and a Windows 10/11 SDK installed. Build the `Release | x64` configuration. The Microsoft C++/WinRT NuGet package required by Host is included under `packages`.

Copy the resulting `holokostya.exe` and `Viewer.exe` to the parent `RemoteDesktop` folder. The Host executable includes a console icon resource and is launched by Limeovery with a PIN through redirected standard input; Viewer uses the same input method for the PIN. The PIN handshake and encrypted video framing must remain compatible between the two executables.

## Connection security

The Host creates a fresh challenge for each connection and verifies the 12-digit device PIN using PBKDF2-HMAC-SHA-256 and HMAC-SHA-256. Video access units are encrypted and authenticated with AES-256-GCM. The PIN is not sent over the socket. Limeovery stores it using Windows DPAPI in the current Windows user profile.
