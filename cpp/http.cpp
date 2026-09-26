#include "http.hpp"

#include <curl/curl.h>

#include <cstdio>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace ml {

namespace {

void ensure_curl_init() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

size_t write_string(char* ptr, size_t size, size_t nmemb, void* userdata) {
    static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
    return size * nmemb;
}

size_t write_file(char* ptr, size_t size, size_t nmemb, void* userdata) {
    return std::fwrite(ptr, size, nmemb, static_cast<FILE*>(userdata)) * size;
}

struct Easy {
    CURL* h;
    curl_slist* headers = nullptr;
    char err[CURL_ERROR_SIZE] = {0};
    Easy(const std::string& url, int timeout, const std::vector<std::string>& extra) {
        ensure_curl_init();
        h = curl_easy_init();
        if (!h) throw std::runtime_error("curl_easy_init failed");
        curl_easy_setopt(h, CURLOPT_URL, url.c_str());
        curl_easy_setopt(h, CURLOPT_USERAGENT, "MonkeyLauncher");
        curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, static_cast<long>(timeout));
        curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, static_cast<long>(timeout));
        curl_easy_setopt(h, CURLOPT_ERRORBUFFER, err);
        for (const auto& x : extra) headers = curl_slist_append(headers, x.c_str());
        if (headers) curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers);
    }
    ~Easy() {
        curl_slist_free_all(headers);
        curl_easy_cleanup(h);
    }
    long perform() {
        CURLcode rc = curl_easy_perform(h);
        if (rc != CURLE_OK)
            throw std::runtime_error(err[0] ? err : curl_easy_strerror(rc));
        long status = 0;
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
        return status;
    }
};

}  // namespace

HttpResponse http_get(const std::string& url, int timeout,
                      const std::vector<std::string>& extra_headers) {
    Easy e(url, timeout, extra_headers);
    HttpResponse resp;
    curl_easy_setopt(e.h, CURLOPT_WRITEFUNCTION, write_string);
    curl_easy_setopt(e.h, CURLOPT_WRITEDATA, &resp.body);
    resp.status = e.perform();
    return resp;
}

long http_download(const std::string& url, const fs::path& dest, int timeout) {
    Easy e(url, timeout, {});
    std::unique_ptr<FILE, int (*)(FILE*)> f(std::fopen(dest.c_str(), "wb"), std::fclose);
    if (!f) throw std::runtime_error("cannot write " + dest.string());
    curl_easy_setopt(e.h, CURLOPT_WRITEFUNCTION, write_file);
    curl_easy_setopt(e.h, CURLOPT_WRITEDATA, f.get());
    return e.perform();
}

std::string url_encode(const std::string& s) {
    ensure_curl_init();
    CURL* h = curl_easy_init();
    char* enc = curl_easy_escape(h, s.c_str(), static_cast<int>(s.size()));
    std::string out = enc ? enc : "";
    curl_free(enc);
    curl_easy_cleanup(h);
    return out;
}

}  // namespace ml
