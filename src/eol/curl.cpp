#include "eol/curl.h"
#include "log.h"
#include "main.h"
#include <format>

std::size_t easy_handle::write_callback_filesystem(char* ptr, size_t size, size_t nmemb,
                                                   void* userdata) {
    easy_handle* this_ = (easy_handle*)userdata;

    // First pass
    if (!this_->file_h) {
        long response_code = -1;
        this_->getinfo(CURLINFO_RESPONSE_CODE, &response_code);
        if (response_code != 200) {
            this_->custom_error =
                std::format("Failed to download file: status code {}", response_code);
            return CURL_WRITEFUNC_ERROR;
        }

        this_->file_h = fopen(this_->file_name.c_str(), "wb");
        if (!this_->file_h) {
            return CURL_WRITEFUNC_ERROR;
        }
    }

    return fwrite(ptr, size, nmemb, this_->file_h);
}

std::size_t easy_handle::write_callback_buffer(char* ptr, size_t size, size_t nmemb,
                                               void* userdata) {
    easy_handle* this_ = static_cast<easy_handle*>(userdata);

    // First pass
    if (this_->data_buffer.empty()) {
        long response_code = -1;
        this_->getinfo(CURLINFO_RESPONSE_CODE, &response_code);
        if (response_code != 200) {
            this_->custom_error = std::format("Request failed: status code {}", response_code);
            return CURL_WRITEFUNC_ERROR;
        }

        curl_off_t content_length = -1;
        this_->getinfo(CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &content_length);
        if (content_length == -1) {
            // Missing/unknown Content-Length header returns a value of -1
            this_->data_buffer.reserve(1024);
        } else {
            this_->data_buffer.reserve(content_length);
        }
    }

#ifdef DEBUG
    ELMA_ASSERT(size == 1); // deprecated parameter
#endif

    this_->data_buffer.insert(this_->data_buffer.end(), ptr, ptr + nmemb);
    return nmemb;
}

std::string easy_handle::error_message(CURLcode code) {
    // Error message from this file
    if (!custom_error.empty()) {
        return custom_error;
    }
    // Error message from libcurl (may not modify this buffer)
    if (error_buffer[0]) {
        return std::string(error_buffer.get());
    }
    // Error code from libcurl (error_buffer may remain empty despite error occurring)
    return std::string(curl_easy_strerror(code));
}

json easy_handle::get_json() {
    ELMA_ASSERT(!data_buffer.empty());

    json j = json::parse(data_buffer, nullptr, false);
    if (j.is_discarded()) {
        return json{};
    }
    return std::move(j);
}

std::optional<std::string> easy_handle::perform_to_filesystem() {
    CURLcode code = curl_easy_perform(handle);

    if (file_h) {
        fclose(file_h);
        file_h = nullptr;
    }

    if (code != CURLE_OK) {
        return error_message(code);
    }
    return std::nullopt;
}

std::pair<json, std::string> easy_handle::perform_to_json() {
    CURLcode code = curl_easy_perform(handle);

    if (code != CURLE_OK) {
        return {json{}, error_message(code)};
    }

    json j = get_json();
    data_buffer.clear();
    if (j.is_null()) {
        return {json{}, "invalid json file"};
    }

    return {std::move(j), ""};
}

easy_handle::easy_handle(easy_handle* base) {
    if (base) {
        handle = curl_easy_duphandle(base->handle);
    } else {
        handle = curl_easy_init();
    }
    ELMA_ASSERT(handle);

    error_buffer = std::make_unique<char[]>(CURL_ERROR_SIZE);
    setopt(CURLOPT_ERRORBUFFER, error_buffer.get());

    setopt(CURLOPT_WRITEDATA, this);
}

easy_handle::~easy_handle() {
    curl_easy_cleanup(handle);
    handle = nullptr;

    if (file_h) {
        fclose(file_h);
        file_h = nullptr;
    }
}

void easy_handle::setopt_write_to_filesystem(std::string destination) {
    file_name = std::move(destination);
    setopt(CURLOPT_WRITEFUNCTION, write_callback_filesystem);
}

void easy_handle::setopt_write_to_json() { setopt(CURLOPT_WRITEFUNCTION, write_callback_buffer); }

std::string multi_handle::error_message(CURLMcode code) {
    return std::string(curl_multi_strerror(code));
}

void multi_handle::add_handle(std::string name, easy_handle* handle) {
    summary.total++;

    uintptr_t index = results.size();
    handle->setopt(CURLOPT_PRIVATE, index);
    multi_handle_result result;
    result.name = std::move(name);
    result.handle = handle;
    results.push_back(std::move(result));

    CURLMcode code = curl_multi_add_handle(multi, handle->handle);
    if (code != CURLM_OK) {
        internal_error(error_message(code));
    }
}

bool multi_handle::perform() {
    int running_handles;
    CURLMcode code = curl_multi_perform(multi, &running_handles);
    if (code != CURLM_OK) {
        internal_error(error_message(code));
    }

    // Remove completed handles and update statistics
    int msgs_in_queue;
    while (CURLMsg* message = curl_multi_info_read(multi, &msgs_in_queue)) {
        ELMA_ASSERT(message->msg == CURLMSG_DONE);

        uintptr_t index = -1;
        CURLcode code = curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &index);
        if (code != CURLE_OK) {
            internal_error("curl_easy_getinfo() failed: " + std::string(curl_easy_strerror(code)));
        }
        multi_handle_result* result = &results[index];

        result->completed = true;
        summary.completed++;
        CURLcode code_result = message->data.result;
        if (code_result == CURLM_OK) {
            result->succeeded = true;
            summary.succeeded++;
        } else {
            result->succeeded = false;
            result->error_message = result->handle->error_message(code_result);
            summary.failed++;
            LOG_DEBUG("Failed to download {} - {}", result->name, result->error_message);
        }

        CURLMcode code2 = curl_multi_remove_handle(multi, result->handle->handle);
        if (code2 != CURLM_OK) {
            internal_error(error_message(code2));
        }
        delete result->handle;
        result->handle = nullptr;
    }

    return running_handles != 0;
}

void multi_handle::poll(int timeout_ms) {
    int numfds;
    CURLMcode code = curl_multi_poll(multi, nullptr, 0, timeout_ms, &numfds);
    if (code != CURLM_OK) {
        internal_error(error_message(code));
    }
}

multi_handle::multi_handle() {
    multi = curl_multi_init();
    ELMA_ASSERT(multi);
}

multi_handle::~multi_handle() {
    CURLMcode code = curl_multi_cleanup(multi);
    if (code != CURLM_OK) {
        internal_error(error_message(code));
    }
    multi = nullptr;
}

std::string share_object::error_message(CURLSHcode code) {
    return std::string(curl_share_strerror(code));
}

share_object::share_object() {
    share = curl_share_init();
    ELMA_ASSERT(share);
}

share_object::~share_object() {
    CURLSHcode code = curl_share_cleanup(share);
    if (code != CURLSHE_OK) {
        internal_error(error_message(code));
    }
    share = nullptr;
}
