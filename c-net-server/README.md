# C-Net Server

A modular, multi-client TCP chat server written in C17 for macOS. The server uses
BSD sockets and `kqueue` for non-blocking event-driven I/O. A small terminal client
uses a receiver thread so typing and incoming messages can happen concurrently.

## Features

- Multiple simultaneous TCP clients (configurable compile-time limit)
- macOS `kqueue` event loop and non-blocking server sockets
- Line-delimited text protocol
- Room chat (`/join`)
- Private messages (`/msg`)
- Nickname changes (`/nick`)
- User and room listings (`/who`, `/rooms`)
- Input and output buffer limits
- Basic event logging
- Graceful SIGINT/SIGTERM shutdown
- Separate server and client executables

## Requirements

- macOS
- Xcode Command Line Tools (`xcode-select --install`)
- CMake 3.16+ (optional; direct Clang build is also supported)

## Build with CMake

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Executables:
- `build/cnet-server`
- `build/cnet-client`

## Build directly with Apple Clang

```sh
clang -std=c17 -Wall -Wextra -Wpedantic -O2 \
  -Iinclude src/common.c src/server.c -o cnet-server

clang -std=c17 -Wall -Wextra -Wpedantic -O2 \
  -Iinclude src/common.c src/client.c -pthread -o cnet-client
```

## Run

Terminal 1:

```sh
./build/cnet-server 8080
```

Terminal 2:

```sh
./build/cnet-client 127.0.0.1 8080
```

Open more terminals and run the client again to simulate multiple users.

## Commands

| Command | Description |
|---|---|
| `/help` | Show commands |
| `/nick NAME` | Change nickname |
| `/join ROOM` | Join/create a room |
| `/msg NICK TEXT` | Send a private message |
| `/who` | List users in the current room |
| `/rooms` | List active rooms |
| `/quit` | Disconnect |

Any line that does not begin with `/` is broadcast to the current room.

Nicknames and room names may contain letters, digits, `_` and `-`, up to 32 characters.

## Protocol

Version 1 uses UTF-8 text lines terminated by `\\n`. The server accumulates partial TCP
reads and processes complete lines. TCP is a byte stream: one `send()` does not
correspond to one `recv()`.

## Security and deployment notes

This is a learning/portfolio project, not an internet-hardened production service.

- The server binds to `127.0.0.1` by default, so it is only reachable locally.
- There is no password authentication or TLS in this version.
- Do not expose it to an untrusted network as-is.
- Before public deployment, add TLS (for example with OpenSSL), authenticated
  accounts, secure password hashing (Argon2id or scrypt), rate limiting, connection
  timeouts, stronger resource accounting, and security-focused tests.
- The server currently uses fixed per-connection buffers and closes clients that
  exceed the configured limits.
- Review and test all changes with sanitizers and fuzzing before deployment.

## Suggested verification

1. Connect two or more clients.
2. Send a normal message; it should appear for other clients in the same room.
3. Change a nickname with `/nick Alice`.
4. Join a room with `/join dev`.
5. Send a private message with `/msg Guest123 hello`.
6. Run `/who` and `/rooms`.
7. Stop the server with Ctrl-C.

## Project layout

```text
include/common.h
src/common.c
src/server.c
src/client.c
CMakeLists.txt
```

The intentionally compact layout keeps this first complete release easy to build.
As the project grows, split server internals into network, protocol, core, and utility
modules and add unit/integration test targets.
