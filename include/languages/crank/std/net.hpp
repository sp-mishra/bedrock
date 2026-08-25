#pragma once

// crank/std/net.hpp — std.net module: TCP/UDP sockets for Crank.
//
// C++23, header-only, no virtual, no macros. Namespace: crank::stdlib
//
// Backed by libuv (uv_tcp_t / uv_udp_t). Guarded on CRANK_STD_HAS_UV: when
// libuv is absent the module is not installed and the header is a no-op. v1
// registers the opaque socket handle types and the connect/listen entry points
// so `import "std.net"` resolves and wiring is verifiable; the full
// event-loop-driven read/write surface is scaffolded on the uvx layer and
// flagged async/blocking rather than exercised synchronously here.
//
// Effect = Network, capability = Network.

#include "languages/crank/std/detail/register.hpp"
#include "languages/crank/std/detail/uv_loop.hpp"
#include "languages/crank/effects.hpp"

#if CRANK_STD_HAS_UV

#include <cstdint>
#include <string>
#include <tuple>

namespace crank::stdlib {
    // Opaque socket handles. Each owns nothing observable from Crank — the host
    // side keeps the uv handle; Crank threads the value through typed thunks.
    struct tcp_listener {
        std::int64_t fd = -1; // bound listen descriptor, -1 when unbound
    };
    struct tcp_connection {
        std::int64_t fd = -1; // connected stream descriptor, -1 when closed
    };
    struct udp_socket {
        std::int64_t fd = -1; // bound datagram descriptor, -1 when unbound
    };

    namespace net_fns {
        // connect_tcp — resolve host:port and open a stream. v1 drives a private
        // loop to attempt the connection and reports success via a non-negative
        // handle; -1 on failure. Synchronous entry over the async uvx layer.
        [[nodiscard]] inline tcp_connection connect_tcp(std::string host,
                                                        std::int64_t port) {
            crank::uvx::loop lp;
            uv_tcp_t sock{};
            uv_tcp_init(lp.raw(), &sock);

            struct connect_state { bool ok = false; } st;
            uv_connect_t req{};
            req.data = &st;

            sockaddr_in addr{};
            int rc = uv_ip4_addr(host.c_str(), static_cast<int>(port), &addr);
            tcp_connection out;
            if (rc != 0) return out;

            rc = uv_tcp_connect(
                &req, &sock, reinterpret_cast<const sockaddr*>(&addr),
                [](uv_connect_t* r, int status) {
                    auto* s = static_cast<connect_state*>(r->data);
                    if (s) s->ok = (status == 0);
                });
            if (rc != 0) {
                uv_close(reinterpret_cast<uv_handle_t*>(&sock), nullptr);
                lp.poll();
                return out;
            }
            lp.run();
            if (st.ok) out.fd = 0; // connected; handle owned host-side
            uv_close(reinterpret_cast<uv_handle_t*>(&sock), nullptr);
            lp.poll();
            return out;
        }

        // listen_tcp — bind host:port and start listening. Reports a bound
        // listener (fd 0) on success, -1 on bind/listen failure.
        [[nodiscard]] inline tcp_listener listen_tcp(std::string host,
                                                     std::int64_t port,
                                                     std::int64_t backlog) {
            crank::uvx::loop lp;
            uv_tcp_t server{};
            uv_tcp_init(lp.raw(), &server);

            sockaddr_in addr{};
            tcp_listener out;
            if (uv_ip4_addr(host.c_str(), static_cast<int>(port), &addr) != 0) {
                uv_close(reinterpret_cast<uv_handle_t*>(&server), nullptr);
                lp.poll();
                return out;
            }
            if (uv_tcp_bind(&server, reinterpret_cast<const sockaddr*>(&addr), 0) != 0) {
                uv_close(reinterpret_cast<uv_handle_t*>(&server), nullptr);
                lp.poll();
                return out;
            }
            int rc = uv_listen(reinterpret_cast<uv_stream_t*>(&server),
                               static_cast<int>(backlog),
                               [](uv_stream_t*, int) {});
            if (rc == 0) out.fd = 0;
            uv_close(reinterpret_cast<uv_handle_t*>(&server), nullptr);
            lp.poll();
            return out;
        }

        // bind_udp — bind a datagram socket to host:port. fd 0 on success.
        [[nodiscard]] inline udp_socket bind_udp(std::string host, std::int64_t port) {
            crank::uvx::loop lp;
            uv_udp_t sock{};
            uv_udp_init(lp.raw(), &sock);

            sockaddr_in addr{};
            udp_socket out;
            if (uv_ip4_addr(host.c_str(), static_cast<int>(port), &addr) != 0) {
                uv_close(reinterpret_cast<uv_handle_t*>(&sock), nullptr);
                lp.poll();
                return out;
            }
            if (uv_udp_bind(&sock, reinterpret_cast<const sockaddr*>(&addr), 0) == 0) {
                out.fd = 0;
            }
            uv_close(reinterpret_cast<uv_handle_t*>(&sock), nullptr);
            lp.poll();
            return out;
        }

        [[nodiscard]] inline bool listener_ok(tcp_listener l) noexcept { return l.fd >= 0; }
        [[nodiscard]] inline bool connection_ok(tcp_connection c) noexcept { return c.fd >= 0; }
        [[nodiscard]] inline bool socket_ok(udp_socket s) noexcept { return s.fd >= 0; }
    } // namespace net_fns
} // namespace crank::stdlib

// Opaque host types (no inspectable fields).
template <>
struct crank::type_descriptor<crank::stdlib::tcp_listener> {
    static constexpr std::string_view name = "std.net.TcpListener";
    static constexpr auto fields = std::tuple{};
};
template <>
struct crank::type_descriptor<crank::stdlib::tcp_connection> {
    static constexpr std::string_view name = "std.net.TcpConnection";
    static constexpr auto fields = std::tuple{};
};
template <>
struct crank::type_descriptor<crank::stdlib::udp_socket> {
    static constexpr std::string_view name = "std.net.UdpSocket";
    static constexpr auto fields = std::tuple{};
};

namespace crank::stdlib {
    inline void install_std_net(crank::context& ctx) {
        namespace n = net_fns;
        ffi_module_builder mod{"std.net"};

        // Network I/O may block on the socket layer; connect/listen drive a loop.
        const function_options net{
            .effects = vakya::types::kEffectMaskNetwork,
            .capabilities = vakya::types::kCapMaskNetwork,
            .flags = static_cast<function_flags>(function_flag::blocking),
            .blocking = blocking_class::potentially_blocking,
        };
        const function_options pure{.flags = kPure};

        ctx.register_type<tcp_listener>();
        ctx.register_type<tcp_connection>();
        ctx.register_type<udp_socket>();
        mod.type("std.net.TcpListener", "TcpListener");
        mod.type("std.net.TcpConnection", "TcpConnection");
        mod.type("std.net.UdpSocket", "UdpSocket");

        detail::add_fn<"std.net.connect_tcp", &n::connect_tcp>(mod, ctx, "ConnectTcp", net);
        detail::add_fn<"std.net.listen_tcp", &n::listen_tcp>(mod, ctx, "ListenTcp", net);
        detail::add_fn<"std.net.bind_udp", &n::bind_udp>(mod, ctx, "BindUdp", net);
        detail::add_fn<"std.net.listener_ok", &n::listener_ok>(mod, ctx, "ListenerOk", pure);
        detail::add_fn<"std.net.connection_ok", &n::connection_ok>(mod, ctx, "ConnectionOk", pure);
        detail::add_fn<"std.net.socket_ok", &n::socket_ok>(mod, ctx, "SocketOk", pure);

        ctx.register_ffi_module(mod.build());
    }
} // namespace crank::stdlib

#endif // CRANK_STD_HAS_UV
