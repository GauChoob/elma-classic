#include "eol/api.h"
#include "eol/curl.h"
#include "log.h"
#include "main.h"
#include <format>

#ifdef DEBUG
#define HOST_URL "https://apitest.elma.online/api/" // TODO some meson or EolConf setting maybe
#define LGR_DL ""
#else
#define HOST_URL "https://api.elma.online/api/"
#define LGR_DL "?dl"
#endif

namespace eol_api {

// SHARED RESOURCES

slist* headers_binary;
slist* headers_json;

// Share object shared among all sequential handles.
// Multithreaded support is possible but mutex is not yet implemented
share_object* share;

// Handle to be used with easy_handle.perform_to_filesystem() or easy_handle.perform_to_json()
easy_handle* sequential_handle;

// Handle to be used with multi_handle.perform()
easy_handle* parallel_handle;

void init() {
    bool static initialized = false;
    ELMA_ASSERT(!initialized);
    initialized = true;

    CURLcode code = curl_global_init(CURL_GLOBAL_ALL);
    if (code != CURLE_OK) {
        internal_error(curl_easy_strerror(code));
    }

    headers_binary = new slist();
    headers_binary->append("Accept: application/octet-stream");
    headers_binary->append("Accept-Encoding: identity");

    headers_json = new slist();
    headers_json->append("Accept: application/json");
    headers_json->append("Accept-Encoding: identity");

    // multi-threaded support can be added in the future with CURLSHOPT_LOCKFUNC
    share = new share_object();
    share->setopt(CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
    share->setopt(CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
    share->setopt(CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);

    sequential_handle = new easy_handle(nullptr);
    ELMA_ASSERT(sequential_handle);
    sequential_handle->setopt(CURLOPT_SHARE, share->get());
    sequential_handle->setopt(CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    sequential_handle->setopt(CURLOPT_TIMEOUT_MS, 60000L);
    sequential_handle->setopt(CURLOPT_MAXFILESIZE, 40L * 1024L * 1024L);
    sequential_handle->setopt(CURLOPT_USERAGENT, "Eol Client/" ELMA_VERSION);
#ifdef DEBUG // TODO some custom meson setting
    // sequential_handle->setopt(CURLOPT_VERBOSE, 1L);
#endif

    parallel_handle = new easy_handle(nullptr);
    ELMA_ASSERT(parallel_handle);
    parallel_handle->setopt(CURLOPT_MAXFILESIZE, 40L * 1024L * 1024L);
    parallel_handle->setopt(CURLOPT_USERAGENT, "Eol Client/" ELMA_VERSION);
#ifdef DEBUG // TODO some custom meson setting
    // parallel_handle->setopt(CURLOPT_VERBOSE, 1L);
#endif
}

void cleanup() {
    delete parallel_handle;
    parallel_handle = nullptr;

    delete sequential_handle;
    sequential_handle = nullptr;

    delete share;
    share = nullptr;

    delete headers_binary;
    headers_binary = nullptr;

    delete headers_json;
    headers_json = nullptr;

    curl_global_cleanup();
}

multi_handle base_multi_handle() {
    multi_handle curl = multi_handle();
    curl.setopt(CURLMOPT_MAX_TOTAL_CONNECTIONS, 5L);
    curl.setopt(CURLMOPT_MAX_HOST_CONNECTIONS, 5L);
    return curl;
}

// API FUNCTIONS

// https://api.elma.online/api/lgr/info
std::pair<json, std::string> lgr_info() {
    easy_handle curl = easy_handle(sequential_handle);
    curl.setopt(CURLOPT_URL, HOST_URL "lgr/info");
    curl.setopt(CURLOPT_HTTPHEADER, headers_json->get());
    curl.setopt_write_to_json();
    return curl.perform_to_json();
}

// https://api.elma.online/api/lgr/get/LGRNAME.lgr?dl
std::optional<std::string> lgr_get(const std::string& lgr_name) {
    easy_handle curl = easy_handle(sequential_handle);
    std::string url = std::format(HOST_URL "lgr/get/{}" LGR_DL, url_encode(lgr_name));
    curl.setopt(CURLOPT_URL, url.c_str());
    curl.setopt(CURLOPT_HTTPHEADER, headers_binary->get());
    curl.setopt_write_to_filesystem(std::format("lgr/{}.lgr", lgr_name));
    return curl.perform_to_filesystem();
}

// Download multiple lgrs
// https://api.elma.online/api/lgr/get/LGRNAME.lgr?dl
void lgr_get_multi(const std::vector<std::string>& lgr_names) {
    multi_handle curl = base_multi_handle();
    for (const std::string& lgr_name : lgr_names) {
        easy_handle* handle = new easy_handle(parallel_handle);
        std::string url = std::format(HOST_URL "lgr/get/{}" LGR_DL, url_encode(lgr_name));
        handle->setopt(CURLOPT_URL, url.c_str());
        handle->setopt(CURLOPT_HTTPHEADER, headers_binary->get());
        handle->setopt_write_to_filesystem(std::format("lgr/{}.lgr", lgr_name));
        curl.add_handle(lgr_name + ".lgr", handle);
    }

    while (curl.perform()) {
        const multi_handle_summary* summary = curl.get_summary();
        LOG_INFO("{}/{} ({} failures)", summary->completed, summary->total, summary->failed);
        curl.poll(1000);
    }

    // TODO: print debug info
    LOG_INFO("Summary:");
    const auto& results = curl.get_results();
    for (const auto& result : results) {
        if (result.succeeded) {
            LOG_INFO("Downloaded {}", result.name);
        }
    }
    for (const auto& result : results) {
        if (!result.succeeded) {
            LOG_INFO("Failed to download {} - {}", result.name, result.error_message);
        }
    }
}

// TEST STUFF THAT GOES IN A DIFFERENT FILE

void update_lgrs() {
    // https://api.elma.online/api/lgr/info
    auto [info, err] = eol_api::lgr_info();
    if (info.is_null()) {
        LOG_DEBUG("Failed to update lgrs: {}", err);
        return;
    }

    // Make and validate a list of lgrs that we want to update
    std::vector<std::string> lgr_names;
    ELMA_ASSERT(info.is_array());
    for (const json& lgr : info) {
        const json& lgr_name_j = lgr["LGRName"];
        if (lgr_name_j.is_null() || !lgr_name_j.is_string()) {
            continue;
        }
        const std::string& lgr_name = lgr_name_j.get_ref<const std::string&>();

        const json& crc_j = lgr["CRC"];
        if (crc_j.is_null() || !crc_j.is_string()) {
            continue;
        }
        const std::string& crc = crc_j.get_ref<const std::string&>();

        // TODO temp check
        if (std::filesystem::exists(std::format("lgr/{}.lgr", lgr_name))) {
            // TODO: if we already have lgr with identical CRC, then skip the lgr
            // TODO: if CRC is different, then maybe overwrite the lgr based on setting
            // continue;
        }

        const json& tags_j = lgr["Tags"];
        if (tags_j.is_null() || !tags_j.is_array()) {
            continue;
        }
        bool low_quality = false;
        for (const json& tag : tags_j) {
            if (tag["Name"] == "Low Quality") {
                low_quality = true;
                break;
            }
        }
        if (low_quality) {
            continue;
        }

        lgr_names.push_back(lgr_name);
    }

    // Download the lgrs
    lgr_get_multi(lgr_names);
}

} // namespace eol_api
