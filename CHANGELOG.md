# Changelog

## V1.5
* [Feature] DNS-over-QUIC default port changed. New port is 8853.
    Now an address like `quic://dns.adguard.com` is transformed into `quic://dns.adguard.com:8853`
    So to force the use of the old port `784` specify it strictly - `quic://dns.adguard.com:784`.
* [Feature] Allow retrieving the library version
    * see `ag::dnsproxy::version()`
    * see `AGDnsProxy.libraryVersion` (Apple)
    * see `com.adguard.dnsproxy.DnsProxy.version()` (Android)
    * see `ag_dnsproxy_version()` (C API)
* [Feature] Add a "pretty URL" function for DNS stamps
    * see `ag::server_stamp::pretty_url()`
    * see `AGDnsStamp.prettyUrl`, `AGDnsStamp.prettierUrl` (Apple)
    * see `com.adguard.dnsproxy.DnsStamp.getPrettyUrl()`,
          `com.adguard.dnsproxy.DnsStamp.getPrettierUrl()` (Android)
    * see `ag_dns_stamp::pretty_url`, `ag_dns_stamp::prettier_url` (C API)

## V1.4
* [Feature] API change: allow in-memory filters<p>
    see `ag::dnsfilter::filter_params`
* [Feature] Optimistic DNS caching<p>
    see `ag::dnsproxy_settings::optimistic_cache`
