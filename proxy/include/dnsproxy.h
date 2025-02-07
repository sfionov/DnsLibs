#pragma once

#include <memory>
#include <ag_defs.h>
#include "dnsproxy_settings.h"
#include "dnsproxy_events.h"

namespace ag {

/**
 * DNS proxy module is intended to incapsulate DNS messages processing.
 * It parses, filters, communicates with a DNS resolver and generates answer to a client.
 */
class dnsproxy {
public:
    dnsproxy();
    ~dnsproxy();

    dnsproxy(const dnsproxy &) = delete;
    dnsproxy(dnsproxy &&) = delete;
    dnsproxy &operator=(const dnsproxy &) = delete;
    dnsproxy &operator=(dnsproxy &&) = delete;

    /**
     * @brief Initialize the DNS proxy
     *
     * @param settings proxy settings (see `dnsproxy_settings`)
     * @param events proxy events (see `dnsproxy_events`)
     * @return {true, opt_warning_description} or {false, error_description}
     */
    std::pair<bool, err_string> init(dnsproxy_settings settings, dnsproxy_events events);

    /**
     * @brief Deinitialize the DNS proxy
     */
    void deinit();

    /**
     * @brief Get the DNS proxy settings
     * @return Current settings
     */
    const dnsproxy_settings &get_settings() const;

    /**
     * @brief Handle a DNS message
     *
     * @param message message from client
     * @return a blocked DNS message in case of the message was blocked,
     *         a DNS resolver response in case of the message was passed,
     *         an empty buffer in case of error
     */
    std::vector<uint8_t> handle_message(ag::uint8_view message);

    /**
     * @brief Return the DNS proxy library version
     *
     * The caller does not take ownership of the returned string.
     */
    static const char *version();

private:
    struct impl;
    std::unique_ptr<impl> pimpl;
};

}
