// Copyright (c) 2016 Alexander Gallego. All rights reserved.
//
#include "smf/rpc_recv_context.h"

#include <chrono>

#include <core/timer.hh>
// seastar BUG: when compiling w/ -O3
// some headers are not included on timer.hh
// which causes missed impl symbols. We should revisit
// after upgrade our build system to upstream cmake
#include <core/reactor.hh>
#include <util/noncopyable_function.hh>

#include "smf/log.h"
#include "smf/rpc_generated.h"
#include "smf/rpc_header_ostream.h"
#include "smf/rpc_header_utils.h"

namespace smf {

rpc_recv_context::rpc_recv_context(
  seastar::lw_shared_ptr<rpc_connection_limits> server_instance_limits,
  seastar::socket_address address, rpc::header hdr,
  seastar::temporary_buffer<char> body)
  : rpc_server_limits(server_instance_limits), remote_address(address),
    header(hdr), payload(std::move(body)) {
  assert(header.size() == payload.size());
}

rpc_recv_context::rpc_recv_context(rpc_recv_context &&o) noexcept
  : rpc_server_limits(o.rpc_server_limits), remote_address(o.remote_address),
    header(std::move(o.header)), payload(std::move(o.payload)) {}

rpc_recv_context::~rpc_recv_context() {}

seastar::future<seastar::temporary_buffer<char>>
read_payload(rpc_connection *conn, size_t payload_size) {
  auto timeout = conn->limits->max_body_parsing_duration;
  seastar::timer<> body_timeout;
  body_timeout.set_callback([timeout, conn] {
    LOG_ERROR(
      "Parsing the body of the connnection exceeded max_timeout: {}ms",
      std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count());
    conn->set_error("Connection body parsing exceeded timeout");
    conn->socket.shutdown_input();
  });
  body_timeout.arm(conn->limits->max_body_parsing_duration);
  return conn->istream.read_exactly(payload_size)
    .then([body_timeout = std::move(body_timeout)](auto payload) mutable {
      body_timeout.cancel();
      return seastar::make_ready_future<decltype(payload)>(std::move(payload));
    });
}

constexpr uint32_t
max_flatbuffers_size() {
  // 2GB - 1 is the max a flatbuffers::vector<uint8_t> can hold
  //
  // needed so that we have access to the internal typedefs
  using namespace flatbuffers;  // NOLINT
  return static_cast<uint32_t>(FLATBUFFERS_MAX_BUFFER_SIZE);
}

seastar::future<stdx::optional<rpc_recv_context>>
rpc_recv_context::parse_payload(rpc_connection *conn, rpc::header hdr) {
  using ret_type = stdx::optional<rpc_recv_context>;
  return read_payload(conn, hdr.size())
    .then([conn, hdr](seastar::temporary_buffer<char> body) mutable {
      if (hdr.size() != body.size()) {
        LOG_ERROR("Read incorrect number of bytes `{}`, expected header: `{}`",
                  body.size(), hdr);
        return seastar::make_ready_future<ret_type>(stdx::nullopt);
      }
      if (hdr.size() > max_flatbuffers_size()) {
        LOG_ERROR("Bad payload. Body is >  FLATBUFFERS_MAX_BUFFER_SIZE");
        return seastar::make_ready_future<ret_type>(stdx::nullopt);
      }
      if (hdr.bitflags() &
          rpc::header_bit_flags::header_bit_flags_has_payload_headers) {
        LOG_ERROR("Reading payload headers is not yet implemented");
        return seastar::make_ready_future<ret_type>(stdx::nullopt);
      }

      const uint32_t xx = rpc_checksum_payload(body.get(), body.size());
      if (xx != hdr.checksum()) {
        LOG_ERROR("Payload checksum `{}` does not match header checksum `{}`",
                  xx, hdr.checksum());
        return seastar::make_ready_future<ret_type>(stdx::nullopt);
      }

      rpc_recv_context ctx(conn->limits, conn->remote_address, hdr,
                           std::move(body));
      return seastar::make_ready_future<ret_type>(
        stdx::optional<rpc_recv_context>(std::move(ctx)));
    });
}

seastar::future<stdx::optional<rpc::header>>
rpc_recv_context::parse_header(rpc_connection *conn) {
  using ret_type = stdx::optional<rpc::header>;

  static constexpr size_t kRPCHeaderSize = sizeof(rpc::header);
  DLOG_THROW_IF(
    conn->istream_active_parser != 0,
    "without this line you can have interleaved reads on the buffer");

  conn->istream_active_parser++;
  return conn->istream.read_exactly(kRPCHeaderSize)
    .then([conn](seastar::temporary_buffer<char> header) {
      if (kRPCHeaderSize != header.size()) {
        LOG_ERROR_IF(conn->is_valid(),
                     "Invalid header size `{}`, expected `{}`, skipping req",
                     header.size(), kRPCHeaderSize);
        return seastar::make_ready_future<ret_type>(stdx::nullopt);
      }
      auto hdr = rpc::header();
      std::memcpy(&hdr, header.get(), kRPCHeaderSize);
      if (hdr.size() == 0) {
        LOG_ERROR("Emty body to parse. skipping");
        return seastar::make_ready_future<ret_type>(stdx::nullopt);
      }
      if (hdr.compression() > rpc::compression_flags_MAX) {
        LOG_ERROR("Compression out of range", hdr);
        return seastar::make_ready_future<ret_type>(stdx::nullopt);
      }
      if (hdr.checksum() <= 0) {
        LOG_ERROR("checksum is empty");
        return seastar::make_ready_future<ret_type>(stdx::nullopt);
      }
      if (hdr.meta() <= 0) {
        LOG_ERROR("meta is empty");
        return seastar::make_ready_future<ret_type>(stdx::nullopt);
      }
      if (hdr.compression() ==
          rpc::compression_flags::compression_flags_disabled) {
        hdr.mutate_compression(rpc::compression_flags::compression_flags_none);
      }
      return seastar::make_ready_future<ret_type>(std::move(hdr));
    })
    .finally([conn] { conn->istream_active_parser--; });
}

}  // namespace smf
