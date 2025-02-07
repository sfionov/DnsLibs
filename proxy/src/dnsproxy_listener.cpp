#include "dnsproxy_listener.h"

#include <ag_socket_address.h>
#include <ag_net_consts.h>
#include <uv.h>
#include <thread>
#include <atomic>
#include <magic_enum.hpp>
#include <algorithm>
#include <cassert>
#include <csignal>


#define log_id(l_, lvl_, id_, fmt_, ...) lvl_##log(l_, "[{}] " fmt_, id_, ##__VA_ARGS__)


// Set the libuv thread pool size. Must happen before any libuv usage to have effect.
static const int THREAD_POOL_SIZE_RESULT [[maybe_unused]] = uv_os_setenv("UV_THREADPOOL_SIZE", "24");

// For TCP this could be arbitrarily small, but we would prefer to catch the whole request in one buffer.
static constexpr size_t TCP_RECV_BUF_SIZE = ag::UDP_RECV_BUF_SIZE + 2; // + 2 for payload length

static void udp_alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
    buf->base = new char[ag::UDP_RECV_BUF_SIZE];
    buf->len = ag::UDP_RECV_BUF_SIZE;
}

static void dealloc_buf(const uv_buf_t *buf) {
    delete[] buf->base;
}

// Abstract base for listeners, does uv initialization/stopping
class listener_base : public ag::dnsproxy_listener {
protected:
    ag::logger m_log;
    ag::dnsproxy *m_proxy{nullptr};
    std::thread m_loop_thread;
    using uv_loop_ptr = std::unique_ptr<uv_loop_t, ag::ftor<&uv_loop_delete>>;
    uv_loop_ptr m_loop;
    uv_async_t m_escape_hatch{};
    ag::socket_address m_address;
    ag::listener_settings m_settings;

    // Subclass initializes its handles, callbacks, etc.
    // The loop is initialized, but isn't yet running at this point
    // Called on event loop's thread
    // Return nullopt if the loop should run (success)
    // Close any uv_*_init'ed handles before returning in case of an error!
    virtual ag::err_string before_run() = 0;

    // Subclass cleans up to allow the event loop to exit
    // (close handles, cancel pending work, etc.)
    // Called on event loop's thread
    virtual void before_stop() = 0;

private:
    static void escape_hatch_cb(uv_async_t *handle) {
        auto *self = (listener_base *) handle->data;
        self->before_stop();
        uv_close((uv_handle_t *) &self->m_escape_hatch, nullptr);
    }

    static int run_loop(uv_loop_t *loop, uv_run_mode mode) {
#ifdef __MACH__
        static auto ensure_sigpipe_ignored [[maybe_unused]] = signal(SIGPIPE, SIG_IGN);

#elif defined EVTHREAD_USE_PTHREADS_IMPLEMENTED
        // Block SIGPIPE
        sigset_t sigset, oldset;
        sigemptyset(&sigset);
        sigaddset(&sigset, SIGPIPE);
        pthread_sigmask(SIG_BLOCK, &sigset, &oldset);
#endif // EVTHREAD_USE_PTHREADS_IMPLEMENTED

        auto err = uv_run(loop, mode);

#if defined(EVTHREAD_USE_PTHREADS_IMPLEMENTED) && !defined(__MACH__)
        // Restore SIGPIPE state
        pthread_sigmask(SIG_SETMASK, &oldset, nullptr);
#endif
        return err;
    }

public:
    /**
     * @return std::nullopt if ok, error string otherwise
     */
    ag::err_string init(const ag::listener_settings &settings, ag::dnsproxy *proxy) {
        m_settings = settings;
#ifdef _WIN32
        m_settings.fd = -1; // Unsupported on Windows
#else
        m_settings.fd = dup(m_settings.fd); // Take ownership
#endif

        m_proxy = proxy;
        if (!m_proxy) {
            return "Proxy is not set";
        }

        if (m_settings.fd == -1) {
            m_address = ag::socket_address{m_settings.address, m_settings.port};
            if (!m_address.valid()) {
                return fmt::format("Invalid address: {}", settings.address);
            }
        }

        m_log = ag::create_logger(fmt::format("listener({} {})",
                                              magic_enum::enum_name(settings.protocol),
                                              m_address.str()));

        int err = 0;
        // Init the loop
        if (m_loop = uv_loop_ptr(uv_loop_new()); m_loop == nullptr) {
            return "Failed to create uv loop";
        }

        // Init the escape hatch
        if ((err = uv_async_init(m_loop.get(), &m_escape_hatch, escape_hatch_cb))) {
            return fmt::format("uv_async_init failed: {}", uv_strerror(err));
        }
        m_escape_hatch.data = this;

        const auto err_str = before_run();
        if (err_str.has_value()) {
            uv_close((uv_handle_t *) &m_escape_hatch, nullptr);

            // Run the loop once to let libuv close the handles cleanly
            err = run_loop(m_loop.get(), UV_RUN_DEFAULT);
            assert(0 == err);

            return err_str;
        }

        m_loop_thread = std::thread([this]() {
            run_loop(m_loop.get(), UV_RUN_DEFAULT);
            infolog(m_log, "Finished listening");
        });

        return std::nullopt;
    }

    ~listener_base() override {
        await_shutdown();
        evutil_closesocket(m_settings.fd);
    }

    void shutdown() final {
        // The next invocation of escape_hatch_cb will close all handles, allowing the loop to exit
        if (this == m_escape_hatch.data) { // Check async initialized
            uv_async_send(&m_escape_hatch);
        }
    }

    void await_shutdown() final {
        if (m_loop_thread.joinable()) { // Allow await_shutdown() to be called more than once
            m_loop_thread.join();
        }
    }
};

class listener_udp : public listener_base {
private:
    struct task {
        uv_work_t work_req{};
        listener_udp *self;
        ag::socket_address peer;
        uv_buf_t request;
        ag::uint8_vector response; // Filled in work_cb

        // Takes ownership of request buffer
        task(listener_udp *self, const sockaddr *addr, uv_buf_t request)
                : self(self), peer(addr), request(request) {

            work_req.data = this;
        }

        ~task() {
            dealloc_buf(&request);
        }
    };

    uv_udp_t m_udp_handle{};
    ag::hash_set<task *> m_pending; // Messages not yet processed by the proxy

    static void work_cb(uv_work_t *req) {
        auto *m = (task *) req->data;
        m->response = m->self->m_proxy->handle_message({(uint8_t *) m->request.base, m->request.len});
    }

    static void send_cb(uv_udp_send_t *req, int status) {
        auto *m = (task *) req->data;
        if (status != 0) {
            dbglog(m->self->m_log, "{} error: {}", __func__, uv_strerror(status));
        }
        delete req;
        delete m;
    }

    static void after_work_cb(uv_work_t *req, int status) {
        auto *m = (task *) req->data;

        m->self->m_pending.erase(m);

        if (status == UV_ECANCELED) {
            delete m;
            return;
        }

        auto resp_buf = uv_buf_init((char *) m->response.data(), m->response.size());

        auto *send_req = new uv_udp_send_t;
        send_req->data = m;

        const int err = uv_udp_send(send_req, &m->self->m_udp_handle, &resp_buf, 1, m->peer.c_sockaddr(), send_cb);
        if (err < 0) {
            dbglog(m->self->m_log, "uv_udp_send failed: {}", uv_strerror(err));
            delete send_req;
            delete m;
        }
    }

    static void recv_cb(uv_udp_t *handle, ssize_t nread, const uv_buf_t *buf,
                        const struct sockaddr *addr, unsigned flags) {
        auto *self = (listener_udp *) handle->data;

        if (nread < 0) {
            dbglog(self->m_log, "{} failed: {}", __func__, uv_strerror(nread));
            dealloc_buf(buf);
            return;
        }
        if (nread == 0) {
            dbglog(self->m_log, "{}: received empty packet", __func__);
            dealloc_buf(buf);
            return;
        }
        if (flags & UV_UDP_PARTIAL) {
            dbglog(self->m_log, "{} failed: truncated", __func__);
            dealloc_buf(buf);
            return;
        }

        auto *m = new task(self, addr, *buf);
        uv_queue_work(self->m_loop.get(), &m->work_req, work_cb, after_work_cb);
        self->m_pending.insert(m);
    }

protected:
    ag::err_string before_run() override {
        int err = 0;

        // Init UDP
        if ((err = uv_udp_init(m_loop.get(), &m_udp_handle)) < 0) {
            return fmt::format("uv_udp_init failed: {}", uv_strerror(err));
        }
        m_udp_handle.data = this;

        if (m_settings.fd == -1) {
            if ((err = uv_udp_bind(&m_udp_handle, m_address.c_sockaddr(), UV_UDP_REUSEADDR)) < 0) {
                uv_close((uv_handle_t *) &m_udp_handle, nullptr);
                return fmt::format("uv_udp_bind failed: {}", uv_strerror(err));
            }
        } else {
            if ((err = uv_udp_open(&m_udp_handle, m_settings.fd)) < 0) {
                uv_close((uv_handle_t *) &m_udp_handle, nullptr);
                return fmt::format("uv_udp_open failed: {}", uv_strerror(err));
            }
            m_settings.fd = -1; // uv_udp_open took ownership
        }

        if ((err = uv_udp_recv_start(&m_udp_handle, udp_alloc_cb, recv_cb)) < 0) {
            uv_close((uv_handle_t *) &m_udp_handle, nullptr);
            return fmt::format("uv_udp_recv_start failed: {}", uv_strerror(err));
        }

        sockaddr_storage name{};
        int namelen = sizeof(name);
        uv_udp_getsockname(&m_udp_handle, (sockaddr *) &name, &namelen);
        infolog(m_log, "Listening on {} (UDP)", ag::socket_address((sockaddr *) &name).str(), m_address.str());

        return std::nullopt;
    }

    void before_stop() override {
        uv_close((uv_handle_t *) &m_udp_handle, nullptr);

        for (auto *m : m_pending) {
            uv_cancel((uv_req_t *) &m->work_req);
        }
    }
};

class tcp_dns_payload_parser {
private:
    enum class state {
        RD_SIZE, RD_PAYLOAD
    };
    state m_state;
    uint16_t m_size;
    ag::uint8_vector m_data;

public:
    tcp_dns_payload_parser() : m_state{state::RD_SIZE}, m_size{0} {
    }

    // Push more data to this parser
    void push_data(ag::uint8_view data) {
        m_data.insert(m_data.end(), data.begin(), data.end());
    }

    // Initialize `out` to contain the next parsed payload
    // Return true if successful or false if more data is needed (in which case `out` won't be modified)
    bool next_payload(ag::uint8_vector &out) {
        if (m_state == state::RD_SIZE) {
            if (m_data.size() < 2) {
                return false; // Need more data
            }
            m_size = *(uint16_t *) m_data.data();
            m_size = ntohs(m_size);
            m_state = state::RD_PAYLOAD;
        }
        if (m_state == state::RD_PAYLOAD) {
            if (m_data.size() < (size_t) 2 + m_size) {
                return false; // Need more data
            }
            out = ag::uint8_vector(m_data.begin() + 2, m_data.begin() + 2 + m_size);
            m_data.erase(m_data.begin(), m_data.begin() + 2 + m_size);
            m_state = state::RD_SIZE;
        }
        return true;
    }
};

class tcp_dns_connection {
public:
    explicit tcp_dns_connection(uint64_t id)
            : m_id{id}
            , m_log(ag::create_logger(__func__))
            , m_tcp((uv_tcp_t *) malloc(sizeof(uv_tcp_t))) // Deleted in close_cb
            , m_idle_timer((uv_timer_t *) malloc(sizeof(uv_timer_t))) // Deleted in close_cb
    {
        this->m_tcp->data = this;
        this->m_idle_timer->data = this;
    }

    // Call after *handle() is properly initialized
    void start(uv_loop_t *loop,
               ag::dnsproxy *proxy,
               bool persistent,
               std::chrono::milliseconds idle_timeout,
               std::function<void(uint64_t)> close_callback) {
        log_id(m_log, trace, m_id, "{}", __func__);

        assert(proxy);
        assert(idle_timeout.count());

        uv_timer_init(loop, m_idle_timer);

        m_proxy = proxy;
        m_persistent = persistent;
        m_idle_timeout = idle_timeout;
        m_close_callback = std::move(close_callback);
        do_read();
    }

    void close() {
        do_close();
    }

    uint64_t id() {
        return m_id;
    }

    uv_tcp_t *handle() {
        return m_tcp;
    }

private:
    struct work {
        uv_work_t req{};
        tcp_dns_connection *c;
        ag::uint8_vector payload;
        bool canceled;
        std::mutex mtx;

        work(tcp_dns_connection *c, ag::uint8_vector &&payload)
                : c{c},
                  payload{std::move(payload)},
                  canceled{false} {
            this->req.data = this;
        }
    };

    struct write {
        uv_write_t req{};
        ag::uint8_vector payload;
        uint16_t size_be; // Big-endian size
        uv_buf_t bufs[2];

        explicit write(ag::uint8_vector &&payload) : payload(std::move(payload)) {
            this->req.data = this;
            this->size_be = this->payload.size();
            this->size_be = htons(this->size_be);
            bufs[0] = uv_buf_init((char *) &this->size_be, sizeof(this->size_be));
            bufs[1] = uv_buf_init((char *) this->payload.data(), this->payload.size());
        }
    };

    const uint64_t m_id;
    ag::logger m_log;
    ag::dnsproxy *m_proxy{};
    bool m_persistent{false};
    uint8_t m_incoming_buf[TCP_RECV_BUF_SIZE]{};
    uv_tcp_t *m_tcp{};
    uv_timer_t *m_idle_timer{};
    std::chrono::milliseconds m_idle_timeout{0};
    std::function<void(uint64_t)> m_close_callback;
    bool m_closed{false};
    tcp_dns_payload_parser m_parser;
    ag::hash_set<work *> m_pending_works;

    static void alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
        auto *c = (tcp_dns_connection *) handle->data;
        buf->base = (char *) c->m_incoming_buf;
        buf->len = sizeof(c->m_incoming_buf);
    }

    static void read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
        auto *c = (tcp_dns_connection *) stream->data;
        log_id(c->m_log, trace, c->m_id, "{} {}", __func__, nread);

        if (nread < 0) {
            c->do_close();
            return;
        }

        assert(buf->base == (char *) c->m_incoming_buf);
        c->m_parser.push_data({c->m_incoming_buf, (size_t) nread});

        ag::uint8_vector payload;
        while (c->m_parser.next_payload(payload)) {
            uv_timer_again(c->m_idle_timer);

            auto *w = new work(c, std::move(payload));

            uv_queue_work(stream->loop, &w->req, work_cb, after_work_cb);
            c->m_pending_works.insert(w);

            if (!c->m_persistent) { // Stop after the first request
                uv_read_stop(stream);
                break;
            }
        }
    }

    static void work_cb(uv_work_t *w_req) {
        auto *w = (work *) w_req->data;
        std::scoped_lock l{w->mtx};
        if (w->canceled) {
            return;
        }
        auto *c = w->c;
        w->payload = c->m_proxy->handle_message({w->payload.data(), w->payload.size()});
    }

    static void after_work_cb(uv_work_t *w_req, int status) {
        auto *w = (work *) w_req->data;
        {
            std::scoped_lock l{w->mtx};
            if (status == 0 && !w->canceled) {
                auto *c = w->c;
                c->m_pending_works.erase(w);
                c->do_write(std::move(w->payload));
            }
        }
        delete w;
    }

    static void write_cb(uv_write_t *w_req, int status) {
        auto *w = (write *) w_req->data;
        auto *h = (uv_handle_t *) w_req->handle;
        auto *c = (tcp_dns_connection *) h->data;
        // `c` might be nullptr at this point, e.g. the connection was closed,
        // but libuv still called the pending write callbacks.
        if (c) {
            log_id(c->m_log, trace, c->m_id, "{} {}", __func__, status);
            if (!c->m_persistent || status < 0) {
                c->do_close();
            }
        }
        delete w;
    }

    static void idle_timeout_cb(uv_timer_t *h) {
        auto *c = (tcp_dns_connection *) h->data;
        c->do_close();
    }

    void do_read() {
        if (uv_read_start((uv_stream_t *) m_tcp, alloc_cb, read_cb) < 0) {
            do_close();
            return;
        }
        uv_timer_start(m_idle_timer, idle_timeout_cb, m_idle_timeout.count(), m_idle_timeout.count());
    }

    void do_write(ag::uint8_vector &&payload) {
        auto *w = new write(std::move(payload));
        if (uv_write(&w->req, (uv_stream_t *) m_tcp, w->bufs, 2, write_cb) < 0) {
            delete w;
            do_close();
        }
    }

    static void close_cb(uv_handle_t *h) {
        free(h);
    }

    void do_close() {
        if (m_closed) {
            return;
        }
        m_closed = true;

        log_id(m_log, trace, m_id, "{}", __func__);
        uv_timer_stop(m_idle_timer);

        m_idle_timer->data = nullptr;
        uv_close((uv_handle_t *) m_idle_timer, close_cb);

        std::for_each(m_pending_works.begin(), m_pending_works.end(), [](work *w) {
            std::scoped_lock l{w->mtx};
            uv_cancel((uv_req_t *) &w->req);
            w->canceled = true;
        });

        m_tcp->data = nullptr;
        uv_close((uv_handle_t *) m_tcp, close_cb);

        if (m_close_callback) {
            m_close_callback(m_id);
        }
    }
};

class listener_tcp : public listener_base {
private:
    static constexpr auto BACKLOG = 128;

    uv_tcp_t m_tcp_handle{};
    uint64_t m_id_counter{0};
    ag::hash_map<uint64_t, std::unique_ptr<tcp_dns_connection>> m_connections;

    static void conn_cb(uv_stream_t *server, int status) {
        auto *self = (listener_tcp *) server->data;

        if (status < 0) {
            dbglog(self->m_log, "{}: connection failed: {}", __func__, uv_strerror(status));
            return;
        }

        auto conn = std::make_unique<tcp_dns_connection>(self->m_id_counter++);

        int err = uv_tcp_init(self->m_loop.get(), conn->handle());
        if (err < 0) {
            dbglog(self->m_log, "{}: uv_tcp_init failed: {}", __func__, uv_strerror(err));
            return;
        }

        if ((err = uv_accept(server, (uv_stream_t *) conn->handle())) < 0) {
            dbglog(self->m_log, "{}: uv_accept failed: {}", __func__, uv_strerror(err));
            return;
        }

        conn->start(self->m_loop.get(),
                    self->m_proxy,
                    self->m_settings.persistent,
                    self->m_settings.idle_timeout,
                    [self](uint64_t id) {
                        self->m_connections.erase(id);
                    });
        self->m_connections[conn->id()] = std::move(conn);
    }

protected:
    ag::err_string before_run() override {
        int err = 0;

        if ((err = uv_tcp_init(m_loop.get(), &m_tcp_handle)) < 0) {
            return fmt::format("uv_tcp_init failed: {}", uv_strerror(err));
        }
        m_tcp_handle.data = this;

        if (m_settings.fd == -1) {
            if ((err = uv_tcp_bind(&m_tcp_handle, m_address.c_sockaddr(), 0)) < 0) {
                uv_close((uv_handle_t *) &m_tcp_handle, nullptr);
                return fmt::format("uv_tcp_bind failed: {}", uv_strerror(err));
            }
        } else {
            if ((err = uv_tcp_open(&m_tcp_handle, m_settings.fd)) < 0) {
                uv_close((uv_handle_t *) &m_tcp_handle, nullptr);
                return fmt::format("uv_tcp_open failed: {}", uv_strerror(err));
            }
            m_settings.fd = -1; // uv_tcp_open took ownership
        }

        if ((err = uv_listen((uv_stream_t *) &m_tcp_handle, BACKLOG, conn_cb)) < 0) {
            uv_close((uv_handle_t *) &m_tcp_handle, nullptr);
            return fmt::format("uv_listen failed: {}", uv_strerror(err));
        }

        sockaddr_storage name{};
        int namelen = sizeof(name);
        uv_tcp_getsockname(&m_tcp_handle, (sockaddr *) &name, &namelen);
        infolog(m_log, "Listening on {} (TCP)", ag::socket_address((sockaddr *) &name).str(), m_address.str());

        return std::nullopt;
    }

    void before_stop() override {
        uv_close((uv_handle_t *) &m_tcp_handle, nullptr);
        for (auto i = m_connections.begin(); i != m_connections.end();) {
            // close removes current element from the list
            auto next = i;
            std::advance(next, 1);
            i->second->close();
            i = next;
        }
    }
};

ag::dnsproxy_listener::create_result ag::dnsproxy_listener::create_and_listen(const ag::listener_settings &settings,
                                                                              dnsproxy *proxy) {
    if (!proxy) {
        return {nullptr, "proxy is nullptr"};
    }

    std::unique_ptr<listener_base> ptr;
    switch (settings.protocol) {
    case ag::listener_protocol::UDP:
        ptr = std::make_unique<listener_udp>();
        break;
    case ag::listener_protocol::TCP:
        ptr = std::make_unique<listener_tcp>();
        break;
    default:
        return {nullptr, fmt::format("Protocol {} not implemented", magic_enum::enum_name(settings.protocol))};
    }

    auto err = ptr->init(settings, proxy);
    if (err.has_value()) {
        return {nullptr, err};
    }

    return {std::move(ptr), std::nullopt};
}
