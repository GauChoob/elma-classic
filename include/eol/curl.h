#ifndef EOL_CURL
#define EOL_CURL

#include "main.h"
#include <curl/curl.h>
#include <memory>
#include <optional>
#include <string>

#define JSON_DIAGNOSTICS 1
#include <nlohmann/json.hpp>
using json = nlohmann::ordered_json;

// Implementation of curl_slist with error-checking and self-clean-up
class slist {
    curl_slist* list = nullptr;

  public:
    slist() = default;
    ~slist() { curl_slist_free_all(list); };

    void append(const char* string) {
        list = curl_slist_append(list, string);
        ELMA_ASSERT(list);
    }

    curl_slist* get() { return list; };

    // cannot copy curl_slist
    slist(const slist&) = delete;
    slist& operator=(const slist&) = delete;
};

// Implementation of libcurl-easy with error-checking and self-clean-up
class easy_handle {
    CURL* handle = nullptr;
    std::string custom_error = {};
    std::unique_ptr<char[]> error_buffer;

    // Download to file
    std::string file_name = {};
    FILE* file_h = nullptr;

    // Download to buffer
    std::vector<unsigned char> data_buffer;

    static std::size_t write_callback_filesystem(char* ptr, size_t size, size_t nmemb,
                                                 void* userdata);
    static std::size_t write_callback_buffer(char* ptr, size_t size, size_t nmemb, void* userdata);

    std::string error_message(CURLcode code);
    json get_json();

  public:
    template <typename T> void setopt(CURLoption option, T value) {
        CURLcode code = curl_easy_setopt(handle, option, value);
        if (code != CURLE_OK) {
            internal_error("curl_easy_setopt() failed: " + error_message(code));
        }
    }

    template <typename T> void getinfo(CURLINFO info, T ptr) {
        CURLcode code = curl_easy_getinfo(handle, info, ptr);
        if (code != CURLE_OK) {
            internal_error("curl_easy_getinfo() failed: " + error_message(code));
        }
    }

    // Return an error message if fails
    std::optional<std::string> perform_to_filesystem();

    // Return a json or an error message if fails (if json.is_null())
    std::pair<json, std::string> perform_to_json();

    easy_handle(easy_handle* base);
    ~easy_handle();

    void setopt_write_to_filesystem(std::string destination);
    void setopt_write_to_json();

    friend class multi_handle;
};

struct multi_handle_summary {
    int succeeded = 0;
    int failed = 0;
    int completed = 0;
    int total = 0;
};

struct multi_handle_result {
    std::string name;
    easy_handle* handle = nullptr;
    bool succeeded = false;
    bool completed = false;
    std::string error_message = "Pending perform()";
};

// Implementation of libcurl-multi with error-checking and self-clean-up
class multi_handle {
    CURLM* multi = nullptr;
    multi_handle_summary summary = {};
    std::vector<multi_handle_result> results;

    static std::string error_message(CURLMcode code);

  public:
    template <typename T> void setopt(CURLMoption option, T value) {
        CURLMcode code = curl_multi_setopt(multi, option, value);
        if (code != CURLM_OK) {
            internal_error("curl_multi_setopt() failed: " + error_message(code));
        }
    }

    void add_handle(std::string name, easy_handle* handle);

    // returns false when done
    bool perform();

    // sleep until we need to call perform again
    void poll(int timeout_ms);

    multi_handle();
    ~multi_handle();

    const multi_handle_summary* get_summary() const { return &summary; }
    const std::vector<multi_handle_result>& get_results() const { return results; }
};

// Implementation of libcurl-share with error-checking and self-clean-up
class share_object {
    CURLSH* share = nullptr;

    static std::string error_message(CURLSHcode code);

  public:
    template <typename T> void setopt(CURLSHoption option, T value) {
        CURLSHcode code = curl_share_setopt(share, option, value);
        if (code != CURLSHE_OK) {
            internal_error("curl_share_setopt() failed: " + error_message(code));
        }
    }

    share_object();
    ~share_object();

    CURLSH* get() { return share; }
};

#endif
