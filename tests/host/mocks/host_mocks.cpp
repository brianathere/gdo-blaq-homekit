#include "host_mocks.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <esp_err.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/semphr.h>
#include <nvs.h>
#include <sha/sha_core.h>

extern "C" {
#include "sha.h"
}

namespace {

std::recursive_mutex g_nvs_mutex;
std::map<std::string, std::map<std::string, std::vector<uint8_t>>> g_nvs;
std::map<nvs_handle_t, std::string> g_handles;
std::vector<std::unique_ptr<std::recursive_mutex>> g_semaphores;
nvs_handle_t g_next_handle = 1;
uint32_t g_time_ms;
uint32_t g_rng = 0x12345678U;

uint32_t next_random(void)
{
    g_rng = (1664525U * g_rng) + 1013904223U;
    return g_rng;
}

template <typename T>
esp_err_t get_scalar(nvs_handle_t handle, const char *key, T *out_value)
{
    if (!key || !out_value) {
        return ESP_ERR_INVALID_ARG;
    }
    std::lock_guard<std::recursive_mutex> guard(g_nvs_mutex);
    auto h = g_handles.find(handle);
    if (h == g_handles.end()) {
        return ESP_ERR_INVALID_ARG;
    }
    auto ns = g_nvs.find(h->second);
    if (ns == g_nvs.end()) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    auto value = ns->second.find(key);
    if (value == ns->second.end() || value->second.size() != sizeof(T)) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    std::memcpy(out_value, value->second.data(), sizeof(T));
    return ESP_OK;
}

template <typename T>
esp_err_t set_scalar(nvs_handle_t handle, const char *key, T value)
{
    if (!key) {
        return ESP_ERR_INVALID_ARG;
    }
    std::lock_guard<std::recursive_mutex> guard(g_nvs_mutex);
    auto h = g_handles.find(handle);
    if (h == g_handles.end()) {
        return ESP_ERR_INVALID_ARG;
    }
    std::vector<uint8_t> bytes(sizeof(T));
    std::memcpy(bytes.data(), &value, sizeof(T));
    g_nvs[h->second][key] = bytes;
    return ESP_OK;
}

} // namespace

extern "C" void host_nvs_reset(void)
{
    std::lock_guard<std::recursive_mutex> guard(g_nvs_mutex);
    g_nvs.clear();
    g_handles.clear();
    g_next_handle = 1;
}

extern "C" void host_time_set_ms(uint32_t ms)
{
    g_time_ms = ms;
}

extern "C" void host_time_advance_ms(uint32_t ms)
{
    g_time_ms += ms;
}

extern "C" void host_random_seed(uint32_t seed)
{
    g_rng = seed ? seed : 0x12345678U;
}

extern "C" const char *esp_err_to_name(esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return "ESP_OK";
    case ESP_FAIL:
        return "ESP_FAIL";
    case ESP_ERR_NO_MEM:
        return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG:
        return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE:
        return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE:
        return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_FOUND:
        return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_NOT_FINISHED:
        return "ESP_ERR_NOT_FINISHED";
    case ESP_ERR_TIMEOUT:
        return "ESP_ERR_TIMEOUT";
    case ESP_ERR_NVS_NOT_FOUND:
        return "ESP_ERR_NVS_NOT_FOUND";
    case ESP_ERR_NVS_NOT_ENOUGH_SPACE:
        return "ESP_ERR_NVS_NOT_ENOUGH_SPACE";
    case ESP_ERR_WIFI_NOT_INIT:
        return "ESP_ERR_WIFI_NOT_INIT";
    default:
        return "ESP_ERR_UNKNOWN";
    }
}

extern "C" void esp_fill_random(void *buf, size_t len)
{
    auto *bytes = static_cast<uint8_t *>(buf);
    for (size_t i = 0; i < len; ++i) {
        if ((i % 4) == 0) {
            next_random();
        }
        bytes[i] = static_cast<uint8_t>(g_rng >> ((i % 4) * 8));
    }
}

extern "C" uint32_t esp_random(void)
{
    return next_random();
}

extern "C" int64_t esp_timer_get_time(void)
{
    return static_cast<int64_t>(g_time_ms) * 1000;
}

extern "C" SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    auto mutex = std::make_unique<std::recursive_mutex>();
    auto *handle = mutex.get();
    g_semaphores.push_back(std::move(mutex));
    return handle;
}

extern "C" BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t)
{
    if (!semaphore) {
        return pdFALSE;
    }
    static_cast<std::recursive_mutex *>(semaphore)->lock();
    return pdTRUE;
}

extern "C" BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    if (!semaphore) {
        return pdFALSE;
    }
    static_cast<std::recursive_mutex *>(semaphore)->unlock();
    return pdTRUE;
}

extern "C" esp_err_t nvs_open(const char *name, nvs_open_mode_t open_mode, nvs_handle_t *out_handle)
{
    if (!name || !out_handle) {
        return ESP_ERR_INVALID_ARG;
    }
    std::lock_guard<std::recursive_mutex> guard(g_nvs_mutex);
    if (open_mode == NVS_READONLY && g_nvs.find(name) == g_nvs.end()) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (open_mode == NVS_READWRITE) {
        (void)g_nvs[name];
    }
    nvs_handle_t handle = g_next_handle++;
    g_handles[handle] = name;
    *out_handle = handle;
    return ESP_OK;
}

extern "C" void nvs_close(nvs_handle_t handle)
{
    std::lock_guard<std::recursive_mutex> guard(g_nvs_mutex);
    g_handles.erase(handle);
}

extern "C" esp_err_t nvs_commit(nvs_handle_t handle)
{
    std::lock_guard<std::recursive_mutex> guard(g_nvs_mutex);
    return g_handles.count(handle) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

extern "C" esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out_value)
{
    return get_scalar(handle, key, out_value);
}

extern "C" esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    return set_scalar(handle, key, value);
}

extern "C" esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *out_value)
{
    return get_scalar(handle, key, out_value);
}

extern "C" esp_err_t nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t value)
{
    return set_scalar(handle, key, value);
}

extern "C" esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value, size_t *length)
{
    if (!key || !length) {
        return ESP_ERR_INVALID_ARG;
    }
    std::lock_guard<std::recursive_mutex> guard(g_nvs_mutex);
    auto h = g_handles.find(handle);
    if (h == g_handles.end()) {
        return ESP_ERR_INVALID_ARG;
    }
    auto ns = g_nvs.find(h->second);
    if (ns == g_nvs.end()) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    auto value = ns->second.find(key);
    if (value == ns->second.end()) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (!out_value) {
        *length = value->second.size();
        return ESP_OK;
    }
    if (*length < value->second.size()) {
        *length = value->second.size();
        return ESP_ERR_INVALID_SIZE;
    }
    std::memcpy(out_value, value->second.data(), value->second.size());
    *length = value->second.size();
    return ESP_OK;
}

extern "C" esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length)
{
    if (!key || (!value && length > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    std::lock_guard<std::recursive_mutex> guard(g_nvs_mutex);
    auto h = g_handles.find(handle);
    if (h == g_handles.end()) {
        return ESP_ERR_INVALID_ARG;
    }
    if (length == 0) {
        g_nvs[h->second][key] = {};
    } else {
        const auto *bytes = static_cast<const uint8_t *>(value);
        g_nvs[h->second][key] = std::vector<uint8_t>(bytes, bytes + length);
    }
    return ESP_OK;
}

extern "C" esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    if (!key) {
        return ESP_ERR_INVALID_ARG;
    }
    std::lock_guard<std::recursive_mutex> guard(g_nvs_mutex);
    auto h = g_handles.find(handle);
    if (h == g_handles.end()) {
        return ESP_ERR_INVALID_ARG;
    }
    auto ns = g_nvs.find(h->second);
    if (ns == g_nvs.end() || ns->second.erase(key) == 0) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    return ESP_OK;
}

extern "C" esp_err_t esp_sha(int sha_type, const unsigned char *input, size_t ilen, unsigned char output[32])
{
    if (sha_type != SHA2_256 || !output || (!input && ilen > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    SHA256Context ctx;
    if (SHA256Reset(&ctx) != shaSuccess ||
        SHA256Input(&ctx, input, static_cast<unsigned int>(ilen)) != shaSuccess ||
        SHA256Result(&ctx, output) != shaSuccess) {
        return ESP_FAIL;
    }
    return ESP_OK;
}
