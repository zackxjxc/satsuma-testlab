// 基于 libzmq 原生 vmci:// transport 的有界请求/响应实现。
#include "satsuma/core/vmci.hpp"

#include <vmci_sockets.h>
#include <zmq.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "satsuma/core/errors.hpp"

namespace satsuma::transport {
namespace {

[[nodiscard]] int checked_timeout(const std::chrono::milliseconds timeout) {
    if (timeout.count() < 1 || timeout.count() > std::numeric_limits<int>::max()) {
        throw Error("VMCI timeout is outside the supported range");
    }
    return static_cast<int>(timeout.count());
}

void validate_port(const std::uint32_t port) {
    if (port == 0 || port == VMADDR_PORT_ANY) {
        throw Error("VMCI port must be in range 1..4294967294");
    }
}

void validate_metadata_size(const std::size_t size) {
    if (size > kVmciMaxMetadataBytes) {
        throw Error("VMCI metadata exceeds the 4 MiB limit");
    }
}

void validate_payload_size(const std::size_t size) {
    if (size > kVmciChunkBytes) {
        throw Error("VMCI payload exceeds the 1 MiB chunk limit");
    }
}

[[nodiscard]] nlohmann::json parse_metadata(const zmq::message_t& frame) {
    validate_metadata_size(frame.size());
    try {
        return nlohmann::json::parse(
            static_cast<const char*>(frame.data()),
            static_cast<const char*>(frame.data()) + frame.size());
    } catch (const std::exception& error) {
        throw Error("VMCI peer returned invalid JSON metadata: " + std::string(error.what()));
    }
}

[[nodiscard]] std::vector<std::byte> copy_payload(const zmq::message_t& frame) {
    validate_payload_size(frame.size());
    std::vector<std::byte> payload(frame.size());
    if (!payload.empty()) {
        std::memcpy(payload.data(), frame.data(), frame.size());
    }
    return payload;
}

void configure_socket(
    zmq::socket_t& socket,
    const int timeout_ms,
    const bool client) {
    socket.set(zmq::sockopt::linger, 0);
    socket.set(zmq::sockopt::rcvtimeo, timeout_ms);
    socket.set(zmq::sockopt::sndtimeo, timeout_ms);
    socket.set(
        zmq::sockopt::maxmsgsize,
        static_cast<std::int64_t>(std::max(kVmciMaxMetadataBytes, kVmciChunkBytes)));
    if (client) {
        socket.set(zmq::sockopt::immediate, 1);
        socket.set(zmq::sockopt::vmci_connect_timeout, timeout_ms);
    }
}

void send_message(zmq::socket_t& socket, const Message& message) {
    const std::string metadata = message.metadata.dump();
    validate_metadata_size(metadata.size());
    validate_payload_size(message.payload.size());
    const zmq::send_flags metadata_flags = message.payload.empty()
        ? zmq::send_flags::none
        : zmq::send_flags::sndmore;
    if (!socket.send(zmq::buffer(metadata), metadata_flags)) {
        throw Error("VMCI metadata send timed out");
    }
    if (!message.payload.empty() &&
        !socket.send(
            zmq::buffer(message.payload.data(), message.payload.size()),
            zmq::send_flags::none)) {
        throw Error("VMCI payload send timed out");
    }
}

[[nodiscard]] Message receive_message(zmq::socket_t& socket) {
    zmq::message_t metadata_frame;
    if (!socket.recv(metadata_frame, zmq::recv_flags::none)) {
        throw Error("VMCI metadata receive timed out");
    }
    Message message;
    message.metadata = parse_metadata(metadata_frame);
    if (metadata_frame.more()) {
        zmq::message_t payload_frame;
        if (!socket.recv(payload_frame, zmq::recv_flags::none)) {
            throw Error("VMCI payload receive timed out");
        }
        if (payload_frame.more()) {
            throw Error("VMCI message contains unexpected extra frames");
        }
        message.payload = copy_payload(payload_frame);
    }
    return message;
}

[[nodiscard]] std::string zmq_error_text(
    const std::string_view stage,
    const zmq::error_t& error) {
    return std::string(stage) + ": ZeroMQ error " + error.what() +
        " (" + std::to_string(error.num()) + ")";
}

}  // namespace

VmciInfo query_vmci_info() {
    const unsigned int version = VMCISock_Version();
    const int address_family = VMCISock_GetAFValue();
    const unsigned int local_cid = VMCISock_GetLocalCID();
    if (version == VMCI_SOCKETS_INVALID_VERSION) {
        throw Error("VMCI driver version query failed");
    }
    if (address_family <= 0 ||
        address_family > std::numeric_limits<unsigned short>::max()) {
        throw Error("VMCI address family query failed");
    }
    if (local_cid == VMADDR_CID_ANY) {
        throw Error("VMCI local CID query failed");
    }
    return {version, address_family, local_cid};
}

std::string make_bind_endpoint(const std::uint32_t port) {
    validate_port(port);
    return "vmci://@:" + std::to_string(port);
}

std::string make_connect_endpoint(const std::uint32_t cid, const std::uint32_t port) {
    validate_port(port);
    if (cid == VMADDR_CID_ANY) {
        throw Error("VMCI CID must be in range 0..4294967294");
    }
    return "vmci://" + std::to_string(cid) + ':' + std::to_string(port);
}

struct Client::Impl {
    Impl(std::string value, const std::chrono::milliseconds timeout)
        : endpoint(std::move(value)), timeout_ms(checked_timeout(timeout)), context(1) {
        if (zmq_has("vmci") != 1 && endpoint.starts_with("vmci://")) {
            throw Error("libzmq was built without native VMCI support");
        }
    }

    void reset_socket() {
        socket.reset();
    }

    [[nodiscard]] zmq::socket_t& get_socket() {
        if (!socket.has_value()) {
            socket.emplace(context, zmq::socket_type::req);
            configure_socket(*socket, timeout_ms, true);
            socket->connect(endpoint);
        }
        return *socket;
    }

    std::string endpoint;
    int timeout_ms;
    zmq::context_t context;
    std::optional<zmq::socket_t> socket;
    std::mutex mutex;
};

Client::Client(std::string endpoint, const std::chrono::milliseconds timeout)
    : impl_(std::make_unique<Impl>(std::move(endpoint), timeout)) {}

Client::Client(
    const std::uint32_t cid,
    const std::uint32_t port,
    const std::chrono::milliseconds timeout)
    : Client(make_connect_endpoint(cid, port), timeout) {}

Client::~Client() = default;
Client::Client(Client&&) noexcept = default;
Client& Client::operator=(Client&&) noexcept = default;

Message Client::request(
    const nlohmann::json& metadata,
    const std::span<const std::byte> payload) {
    if (!metadata.is_object()) {
        throw Error("VMCI request metadata must be a JSON object");
    }
    validate_payload_size(payload.size());
    std::scoped_lock lock(impl_->mutex);
    try {
        zmq::socket_t& socket = impl_->get_socket();
        Message request_message{metadata, {payload.begin(), payload.end()}};
        send_message(socket, request_message);
        Message response = receive_message(socket);
        if (!response.metadata.value("ok", false)) {
            throw Error(
                "VMCI peer rejected request: " +
                response.metadata.value("error", std::string{"unknown error"}));
        }
        return response;
    } catch (const zmq::error_t& error) {
        impl_->reset_socket();
        throw Error(zmq_error_text("VMCI request", error));
    } catch (...) {
        impl_->reset_socket();
        throw;
    }
}

struct Server::Impl {
    Impl(
        std::string value,
        Handler value_handler,
        const std::chrono::milliseconds poll_interval)
        : endpoint(std::move(value)),
          handler(std::move(value_handler)),
          poll_interval_ms(checked_timeout(poll_interval)),
          context(1),
          socket(context, zmq::socket_type::rep) {
        if (!handler) {
            throw Error("VMCI server requires a request handler");
        }
        if (zmq_has("vmci") != 1 && endpoint.starts_with("vmci://")) {
            throw Error("libzmq was built without native VMCI support");
        }
        configure_socket(socket, poll_interval_ms, false);
        socket.bind(endpoint);
    }

    std::string endpoint;
    Handler handler;
    int poll_interval_ms;
    zmq::context_t context;
    zmq::socket_t socket;
};

Server::Server(
    std::string endpoint,
    Handler handler,
    const std::chrono::milliseconds poll_interval)
    try : impl_(std::make_unique<Impl>(
              std::move(endpoint), std::move(handler), poll_interval)) {
    } catch (const zmq::error_t& error) {
        throw Error(zmq_error_text("VMCI bind", error));
    }

Server::Server(
    const std::uint32_t port,
    Handler handler,
    const std::chrono::milliseconds poll_interval)
    : Server(make_bind_endpoint(port), std::move(handler), poll_interval) {}

Server::~Server() = default;
Server::Server(Server&&) noexcept = default;
Server& Server::operator=(Server&&) noexcept = default;

void Server::run(const std::stop_token stop_token) {
    while (!stop_token.stop_requested()) {
        try {
            Message request = receive_message(impl_->socket);
            Message response;
            try {
                response = impl_->handler(request);
                if (!response.metadata.is_object()) {
                    throw Error("VMCI handler returned non-object metadata");
                }
                response.metadata["ok"] = true;
            } catch (const std::exception& error) {
                response = Message{{{"ok", false}, {"error", error.what()}}, {}};
            } catch (...) {
                response = Message{{{"ok", false}, {"error", "unknown gateway error"}}, {}};
            }
            send_message(impl_->socket, response);
        } catch (const zmq::error_t& error) {
            if (error.num() != EAGAIN && !stop_token.stop_requested()) {
                throw Error(zmq_error_text("VMCI server", error));
            }
        } catch (const Error& error) {
            if (std::string_view(error.what()).find("timed out") == std::string_view::npos &&
                !stop_token.stop_requested()) {
                throw;
            }
        }
    }
}

}  // namespace satsuma::transport
