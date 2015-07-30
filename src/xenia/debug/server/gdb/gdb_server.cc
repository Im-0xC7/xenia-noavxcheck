/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/debug/server/gdb/gdb_server.h"

#include <gflags/gflags.h>

#include "xenia/base/logging.h"
#include "xenia/debug/debugger.h"
#include "xenia/debug/server/gdb/gdb_command_processor.h"

DEFINE_int32(gdb_server_port, 9000, "Debugger gdbserver TCP port.");

namespace xe {
namespace debug {
namespace server {
namespace gdb {

constexpr size_t kReceiveBufferSize = 32 * 1024;

GdbServer::GdbServer(Debugger* debugger) : DebugServer(debugger) {
  receive_buffer_.resize(kReceiveBufferSize);
  command_processor_ = std::make_unique<GdbCommandProcessor>(this, debugger);
}

GdbServer::~GdbServer() = default;

bool GdbServer::Initialize() {
  socket_server_ = SocketServer::Create(uint16_t(FLAGS_gdb_server_port),
                                        [this](std::unique_ptr<Socket> client) {
                                          AcceptClient(std::move(client));
                                        });
  if (!socket_server_) {
    XELOGE("Unable to create GDB socket server - port in use?");
    return false;
  }

  return true;
}

void GdbServer::PostSynchronous(std::function<void()> fn) { assert_always(); }

void GdbServer::AcceptClient(std::unique_ptr<Socket> client) {
  // If we have an existing client, kill it and join its thread.
  if (client_) {
    // TODO(benvanik): GDB say goodbye?

    client_->Close();
    xe::threading::Wait(client_thread_.get(), true);
    client_thread_.reset();
  }

  // Take ownership of the new one.
  client_ = std::move(client);

  // Create a thread to manage the connection.
  client_thread_ = xe::threading::Thread::Create({}, [this]() {
    // TODO(benvanik): GDB protocol stuff? Do we say hi?
    // TODO(benvanik): move hello to thread

    // Let the debugger know we are present.
    debugger_->set_attached(true);

    // Junk just to poke the remote client.
    client_->Send("(gdb)\n");

    // Main loop.
    bool running = true;
    while (running) {
      auto wait_result = xe::threading::Wait(client_->wait_handle(), true);
      switch (wait_result) {
        case xe::threading::WaitResult::kSuccess:
          // Event (read or close).
          running = HandleClientEvent();
          continue;
        case xe::threading::WaitResult::kAbandoned:
        case xe::threading::WaitResult::kFailed:
          // Error - kill the thread.
          running = false;
          continue;
        default:
          // Eh. Continue.
          continue;
      }
    }

    // Kill client (likely already disconnected).
    client_.reset();

    // Notify debugger we are no longer attached.
    debugger_->set_attached(false);
  });
}

bool GdbServer::HandleClientEvent() {
  if (!client_->is_connected()) {
    // Known-disconnected.
    return false;
  }
  // Attempt to read into our buffer.
  size_t bytes_read =
      client_->Receive(receive_buffer_.data(), receive_buffer_.capacity());
  if (bytes_read == -1) {
    // Disconnected.
    return false;
  } else if (bytes_read == 0) {
    // No data available. Wait again.
    return true;
  }

  // Pass off the command processor to do with it what it wants.
  if (!command_processor_->ProcessBuffer(receive_buffer_.data(), bytes_read)) {
    // Error.
    XELOGE("Error processing incoming GDB command; forcing disconnect");
    return false;
  }

  return true;
}

}  // namespace gdb
}  // namespace server
}  // namespace debug
}  // namespace xe
