#pragma once
#include <string>

#include "common.hpp"

namespace ml {

struct HttpResponse {
    long        status = 0;
    std::string body;
};

// GET with the MonkeyLauncher User-Agent. Throws std::runtime_error on
// transport errors (DNS, TLS, timeout…); HTTP error codes are returned in
// `status`, not thrown. `timeout` is the connect / stall timeout in seconds
// (like urllib's socket timeout), not a total deadline.
HttpResponse http_get(const std::string& url, int timeout,
                      const std::vector<std::string>& extra_headers = {});
// Same, streaming the body to a file (for release tarballs).
long http_download(const std::string& url, const fs::path& dest, int timeout);

std::string url_encode(const std::string& s);

}  // namespace ml
