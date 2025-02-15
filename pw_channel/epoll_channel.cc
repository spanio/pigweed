// Copyright 2024 The Pigweed Authors
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.

#include "pw_channel/epoll_channel.h"

#include <fcntl.h>
#include <unistd.h>

#include "pw_log/log.h"
#include "pw_status/try.h"

namespace pw::channel {

void EpollChannel::Register() {
  if (fcntl(channel_fd_, F_SETFL, O_NONBLOCK) != 0) {
    PW_LOG_ERROR("Failed to make channel file descriptor nonblocking: %s",
                 std::strerror(errno));
    set_closed();
    return;
  }

  if (!dispatcher_
           ->NativeRegisterFileDescriptor(
               channel_fd_, async2::Dispatcher::FileDescriptorType::kReadWrite)
           .ok()) {
    set_closed();
    return;
  }

  ready_to_write_ = true;
}

async2::Poll<Result<multibuf::MultiBuf>> EpollChannel::DoPendRead(
    async2::Context& cx) {
  if (!is_read_open()) {
    return Status::FailedPrecondition();
  }

  if (!allocation_future_.has_value()) {
    allocation_future_ =
        allocator_->AllocateContiguousAsync(kMinimumReadSize, kDesiredReadSize);
  }
  async2::Poll<std::optional<multibuf::MultiBuf>> maybe_multibuf =
      allocation_future_->Pend(cx);
  if (maybe_multibuf.IsPending()) {
    return async2::Pending();
  }

  allocation_future_ = std::nullopt;

  if (!maybe_multibuf->has_value()) {
    PW_LOG_ERROR("Failed to allocate multibuf for reading");
    return Status::ResourceExhausted();
  }

  multibuf::MultiBuf buf = std::move(**maybe_multibuf);
  multibuf::Chunk& chunk = *buf.ChunkBegin();

  int bytes_read = read(channel_fd_, chunk.data(), chunk.size());
  if (bytes_read >= 0) {
    buf.Truncate(bytes_read);
    return async2::Ready(std::move(buf));
  }

  if (errno == EAGAIN) {
    // EAGAIN on a non-blocking read indicates that there is no data available.
    // Put the task to sleep until the dispatcher is notified that the file
    // descriptor is active.
    async2::Waker waker = cx.GetWaker(async2::WaitReason::Unspecified());
    cx.dispatcher().NativeAddReadWakerForFileDescriptor(channel_fd_,
                                                        std::move(waker));
    return async2::Pending();
  }

  return Status::Internal();
}

async2::Poll<Status> EpollChannel::DoPendReadyToWrite(async2::Context& cx) {
  if (!is_write_open()) {
    return Status::FailedPrecondition();
  }

  if (!ready_to_write_) {
    // The previous write operation failed. Block the task until the dispatcher
    // receives a notification for the channel's file descriptor.
    ready_to_write_ = true;
    async2::Waker waker = cx.GetWaker(async2::WaitReason::Unspecified());
    cx.dispatcher().NativeAddWriteWakerForFileDescriptor(channel_fd_,
                                                         std::move(waker));
    return async2::Pending();
  }

  return OkStatus();
}

Result<channel::WriteToken> EpollChannel::DoWrite(multibuf::MultiBuf&& data) {
  if (!is_write_open()) {
    return Status::FailedPrecondition();
  }

  const uint32_t token = write_token_++;

  for (multibuf::Chunk& chunk : data.Chunks()) {
    if (write(channel_fd_, chunk.data(), chunk.size()) < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // The file descriptor is not currently available. The next call to
        // `PendReadyToWrite` will put the task to sleep until it is writable
        // again.
        ready_to_write_ = false;
        return Status::Unavailable();
      }

      PW_LOG_ERROR("Epoll channel write failed: %s", std::strerror(errno));
      return Status::Internal();
    }
  }

  return CreateWriteToken(token);
}

void EpollChannel::Cleanup() {
  if (is_read_or_write_open()) {
    dispatcher_->NativeUnregisterFileDescriptor(channel_fd_).IgnoreError();
    set_closed();
  }
  close(channel_fd_);
}

}  // namespace pw::channel
