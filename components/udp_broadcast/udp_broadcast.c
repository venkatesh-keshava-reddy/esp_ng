/* UDP Broadcast - Adapted from ESP-IDF UDP client and multicast examples
 * Reference: D:\esp\esp-idf\examples\protocols\sockets\udp_client\
 * Reference: D:\esp\esp-idf\examples\protocols\sockets\udp_multicast\
 */

#include "udp_broadcast.h"
#include "config_mgr.h"
#include "event_bus.h"
#include "net_mgr.h"
#include "diag.h"
#include "version.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include <math.h>

static const char *TAG = "udp_broadcast";

// Frequency limits (CLAUDE_TASKS.md requirement)
#define MIN_FREQ_HZ 0.2f
#define MAX_FREQ_HZ 5.0f
#define MAX_PAYLOAD_SIZE 512

// State
static esp_timer_handle_t s_timer = NULL;
static SemaphoreHandle_t s_mutex = NULL;
static TaskHandle_t s_broadcast_task = NULL;
static QueueHandle_t s_broadcast_queue = NULL;
static bool s_task_should_run = false;
static int s_socket = -1;
static struct sockaddr_in s_dest_addr = {0};
static udp_cfg_t s_config = {0};
static bool s_is_running = false;
static bool s_is_paused = true;  // Start paused, wait for NET_READY

// Statistics
static uint32_t s_packets_sent = 0;
static uint32_t s_bytes_sent = 0;
static uint32_t s_send_errors = 0;

// Forward declarations for helpers
static void broadcast_timer_callback(void* arg);
static void broadcast_task(void* arg);
static esp_err_t udp_broadcast_start_timer_and_socket(void);
static void udp_broadcast_stop_timer_and_socket(void);

/**
 * Validate and clamp frequency to acceptable range
 * IMPLEMENTATION_PLAN.md step 8: explicit frequency validation
 */
static float validate_frequency(float freq_hz)
{
    if (freq_hz < MIN_FREQ_HZ) {
        ESP_LOGW(TAG, "Frequency %.2f Hz too low, clamping to %.2f Hz", freq_hz, MIN_FREQ_HZ);
        return MIN_FREQ_HZ;
    }
    if (freq_hz > MAX_FREQ_HZ) {
        ESP_LOGW(TAG, "Frequency %.2f Hz too high, clamping to %.2f Hz", freq_hz, MAX_FREQ_HZ);
        return MAX_FREQ_HZ;
    }
    return freq_hz;
}

/**
 * Build JSON payload with device status
 * JSON fields from CLAUDE_TASKS.md:
 * - device_id, ip, mac, fw_version, uptime_s, heap_free, rssi
 * - ntrip_state, ntrip_bytes_rx, ts_unix
 */
static int build_json_payload(char* buf, size_t buf_len)
{
    if (!buf || buf_len == 0) {
        return -1;
    }

    // Get device ID
    char device_id[32] = {0};
    config_mgr_get_string("sys/device_id", device_id, sizeof(device_id));

    // Get IP address
    char ip_str[16] = "0.0.0.0";
    net_mgr_get_ip(ip_str, sizeof(ip_str));

    // Get MAC address
    char mac_str[18] = "00:00:00:00:00:00";
    net_mgr_get_mac(mac_str, sizeof(mac_str));

    // Get firmware version
    const char* fw_version = version_get_string();

    // Get uptime
    uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);

    // Get heap free
    uint32_t heap_free = esp_get_free_heap_size();

    // Get RSSI
    int rssi = 0;
    net_mgr_get_rssi(&rssi);

    // Get NTRIP state (placeholder - will be implemented in Task 6)
    const char* ntrip_state = "disabled";
    uint32_t ntrip_bytes_rx = 0;

    // Get Unix timestamp
    struct timeval tv_now;
    gettimeofday(&tv_now, NULL);
    int64_t ts_unix = (int64_t)tv_now.tv_sec;

    // Build JSON (no secrets, per requirement)
    int len = snprintf(buf, buf_len,
        "{"
        "\"device_id\":\"%s\","
        "\"ip\":\"%s\","
        "\"mac\":\"%s\","
        "\"fw_version\":\"%s\","
        "\"uptime_s\":%lu,"
        "\"heap_free\":%lu,"
        "\"rssi\":%d,"
        "\"ntrip_state\":\"%s\","
        "\"ntrip_bytes_rx\":%lu,"
        "\"ts_unix\":%lld"
        "}",
        device_id, ip_str, mac_str, fw_version,
        uptime_s, heap_free, rssi,
        ntrip_state, ntrip_bytes_rx, ts_unix
    );

    // Check payload size (CLAUDE_TASKS.md requirement: max 512 bytes)
    if (len >= (int)buf_len) {
        ESP_LOGW(TAG, "JSON payload truncated: would be %d bytes, clamped to %zu bytes", len, buf_len - 1);
        // Clamp to actual bytes written (buf_len - 1 for null terminator)
        len = (int)buf_len - 1;
    }

    return len;
}

/**
 * Create socket for configured mode
 * Based on ESP-IDF udp_client and udp_multicast examples
 */
static int create_socket(void)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket: errno %d", errno);
        return -1;
    }

    // Set TTL for broadcast/multicast
    if (s_config.mode == UDP_MODE_BROADCAST || s_config.mode == UDP_MODE_MULTICAST) {
        uint8_t ttl = s_config.ttl;

        if (s_config.mode == UDP_MODE_BROADCAST) {
            // Enable broadcast on socket (from udp_client pattern)
            int broadcast = 1;
            if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast)) < 0) {
                ESP_LOGE(TAG, "Failed to set SO_BROADCAST: errno %d", errno);
                close(sock);
                return -1;
            }
        } else {
            // Set multicast TTL (from udp_multicast pattern)
            if (setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0) {
                ESP_LOGE(TAG, "Failed to set IP_MULTICAST_TTL: errno %d", errno);
                close(sock);
                return -1;
            }
        }

        ESP_LOGI(TAG, "Socket TTL set to %d", ttl);
    }

    // For multicast, join the group (from udp_multicast pattern)
    if (s_config.mode == UDP_MODE_MULTICAST) {
        struct ip_mreq imreq = {0};

        // Parse multicast address
        if (inet_aton(s_config.addr, &imreq.imr_multiaddr) == 0) {
            ESP_LOGE(TAG, "Invalid multicast address: %s", s_config.addr);
            close(sock);
            return -1;
        }

        // Validate it's actually a multicast address
        if (!IP_MULTICAST(ntohl(imreq.imr_multiaddr.s_addr))) {
            ESP_LOGW(TAG, "Address %s is not a valid multicast address", s_config.addr);
        }

        // Use INADDR_ANY for interface (listen on all interfaces)
        imreq.imr_interface.s_addr = IPADDR_ANY;

        // Join multicast group
        if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &imreq, sizeof(imreq)) < 0) {
            ESP_LOGE(TAG, "Failed to join multicast group: errno %d", errno);
            close(sock);
            return -1;
        }

        ESP_LOGI(TAG, "Joined multicast group %s", s_config.addr);
    }

    return sock;
}

/**
 * Close socket and leave multicast group if needed
 */
static void close_socket(void)
{
    if (s_socket < 0) {
        return;
    }

    // Leave multicast group before closing (from udp_multicast pattern)
    if (s_config.mode == UDP_MODE_MULTICAST) {
        struct ip_mreq imreq = {0};
        if (inet_aton(s_config.addr, &imreq.imr_multiaddr) != 0) {
            imreq.imr_interface.s_addr = IPADDR_ANY;
            if (setsockopt(s_socket, IPPROTO_IP, IP_DROP_MEMBERSHIP, &imreq, sizeof(imreq)) < 0) {
                ESP_LOGW(TAG, "Failed to leave multicast group: errno %d", errno);
            } else {
                ESP_LOGI(TAG, "Left multicast group %s", s_config.addr);
            }
        }
    }

    shutdown(s_socket, SHUT_RDWR);
    close(s_socket);
    s_socket = -1;
}

/**
 * Helper: Start timer and create socket when network is ready
 * Called by NET_READY event handler
 * Assumes mutex is held by caller
 */
static esp_err_t udp_broadcast_start_timer_and_socket(void)
{
    esp_err_t ret;

    // Close existing socket if any (handle quick bounce)
    if (s_socket >= 0) {
        ESP_LOGW(TAG, "Socket already exists, closing before restart");
        close_socket();
    }

    // Create socket
    s_socket = create_socket();
    if (s_socket < 0) {
        ESP_LOGE(TAG, "Failed to create socket");
        return ESP_FAIL;
    }

    // Reset stats on network reconnect
    s_packets_sent = 0;
    s_bytes_sent = 0;
    s_send_errors = 0;

    // Create periodic timer if not exists
    if (!s_timer) {
        esp_timer_create_args_t timer_args = {
            .callback = &broadcast_timer_callback,
            .name = "udp_broadcast"
        };
        ret = esp_timer_create(&timer_args, &s_timer);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create timer: %s", esp_err_to_name(ret));
            close_socket();
            return ret;
        }
    }

    // Calculate timer period from config (stored in millihertz)
    float freq_hz = (float)s_config.freq_mhz / 1000.0f;
    float validated_freq = validate_frequency(freq_hz);
    uint64_t period_us = (uint64_t)(1000000.0f / validated_freq);

    // Start or restart periodic timer
    if (esp_timer_is_active(s_timer)) {
        esp_timer_stop(s_timer);
    }

    ret = esp_timer_start_periodic(s_timer, period_us);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start timer: %s", esp_err_to_name(ret));
        close_socket();
        return ret;
    }

    s_is_paused = false;
    ESP_LOGI(TAG, "UDP broadcasts active: %s:%d @ %.2f Hz",
             s_config.addr, s_config.port, validated_freq);

    return ESP_OK;
}

/**
 * Helper: Stop timer and close socket when network is lost
 * Called by NET_LOST event handler
 * Assumes mutex is held by caller
 */
static void udp_broadcast_stop_timer_and_socket(void)
{
    s_is_paused = true;

    // Stop timer (but don't delete - we'll reuse it)
    if (s_timer && esp_timer_is_active(s_timer)) {
        esp_timer_stop(s_timer);
        ESP_LOGI(TAG, "Timer stopped");
    }

    // Close socket
    close_socket();

    ESP_LOGI(TAG, "UDP broadcasts paused (network lost)");
}

/**
 * Send UDP broadcast packet
 */
static void send_udp_packet(void)
{
    // Don't send if paused
    if (s_is_paused) {
        return;
    }

    // Build JSON payload
    char payload[MAX_PAYLOAD_SIZE];
    int payload_len = build_json_payload(payload, sizeof(payload));
    if (payload_len < 0) {
        ESP_LOGE(TAG, "Failed to build JSON payload");
        s_send_errors++;
        return;
    }

    // Send packet (from udp_client pattern)
    int sent = sendto(s_socket, payload, payload_len, 0,
                      (struct sockaddr *)&s_dest_addr, sizeof(s_dest_addr));

    if (sent < 0) {
        ESP_LOGE(TAG, "sendto failed: errno %d", errno);
        s_send_errors++;
        return;
    }

    s_packets_sent++;
    s_bytes_sent += sent;

    ESP_LOGD(TAG, "Sent %d bytes to %s:%d", sent, s_config.addr, s_config.port);
}

/**
 * Timer callback for periodic broadcast
 * Queues work to dedicated task instead of doing network I/O directly
 * This prevents blocking other esp_timer callbacks when WiFi is busy
 */
static void broadcast_timer_callback(void* arg)
{
    (void)arg;

    // Quick check without mutex - if paused, don't queue work
    if (s_is_paused || s_socket < 0) {
        return;
    }

    // Signal broadcast task (non-blocking queue send from ISR)
    uint8_t trigger = 1;
    BaseType_t high_priority_woken = pdFALSE;
    xQueueSendFromISR(s_broadcast_queue, &trigger, &high_priority_woken);

    if (high_priority_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

/**
 * Dedicated task for UDP broadcast
 * Handles actual network I/O separately from timer callbacks
 */
static void broadcast_task(void* arg)
{
    (void)arg;
    uint8_t trigger;

    ESP_LOGI(TAG, "Broadcast task started");

    while (s_task_should_run) {
        // Wait for timer callback to signal (wake periodically to check stop flag)
        if (xQueueReceive(s_broadcast_queue, &trigger, pdMS_TO_TICKS(1000)) == pdTRUE) {
            // Lock mutex for thread safety
            if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                // Double-check state after acquiring mutex
                if (!s_is_paused && s_socket >= 0) {
                    send_udp_packet();
                }
                xSemaphoreGive(s_mutex);
            } else {
                ESP_LOGW(TAG, "Failed to acquire mutex for broadcast");
            }
        } else if (!s_task_should_run) {
            break;
        }
    }

    s_broadcast_task = NULL;
    ESP_LOGI(TAG, "Broadcast task exiting");
    vTaskDelete(NULL);
}

/**
 * Event handler for network events
 * Uses short blocking timeout to handle mutex contention gracefully
 * This ensures we don't miss state transitions during network bounces
 */
static void udp_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == DEVICE_EVENT) {
        if (event_id == DEVICE_EVENT_NET_READY) {
            ESP_LOGI(TAG, "Network ready, starting broadcasts");
            // Use short blocking timeout (100ms) to handle mutex contention
            // Critical: zero-timeout would cause missed state transitions during bounces
            if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                // Only start if module is running
                if (s_is_running) {
                    esp_err_t ret = udp_broadcast_start_timer_and_socket();
                    if (ret != ESP_OK) {
                        ESP_LOGE(TAG, "Failed to start timer and socket: %s", esp_err_to_name(ret));
                    }
                }
                xSemaphoreGive(s_mutex);
            } else {
                ESP_LOGE(TAG, "CRITICAL: Failed to acquire mutex in NET_READY handler after 100ms - broadcasts may not start!");
            }
        } else if (event_id == DEVICE_EVENT_NET_LOST) {
            ESP_LOGI(TAG, "Network lost, pausing broadcasts");
            // Use short blocking timeout (100ms) to handle mutex contention
            // Critical: zero-timeout would cause missed state transitions during bounces
            if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                if (s_is_running) {
                    udp_broadcast_stop_timer_and_socket();
                }
                xSemaphoreGive(s_mutex);
            } else {
                ESP_LOGE(TAG, "CRITICAL: Failed to acquire mutex in NET_LOST handler after 100ms - broadcasts may continue on dead network!");
            }
        }
    }
}

esp_err_t udp_broadcast_start(void)
{
    esp_err_t ret;

    ESP_LOGI(TAG, "Starting UDP broadcast");

    // Create mutex if not exists
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) {
            ESP_LOGE(TAG, "Failed to create mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    // Create queue for timer->task communication
    if (!s_broadcast_queue) {
        s_broadcast_queue = xQueueCreate(10, sizeof(uint8_t));
        if (!s_broadcast_queue) {
            ESP_LOGE(TAG, "Failed to create broadcast queue");
            return ESP_ERR_NO_MEM;
        }
    }

    // Create dedicated broadcast task
    if (!s_broadcast_task) {
        s_task_should_run = true;
        BaseType_t task_created = xTaskCreate(
            broadcast_task,
            "udp_broadcast",
            4096,
            NULL,
            5,  // Priority 5 (normal)
            &s_broadcast_task
        );
        if (task_created != pdPASS) {
            ESP_LOGE(TAG, "Failed to create broadcast task");
            s_task_should_run = false;
            return ESP_ERR_NO_MEM;
        }
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire mutex");
        return ESP_FAIL;
    }

    // Check if already running
    if (s_is_running) {
        ESP_LOGW(TAG, "UDP broadcast already running");
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }

    // Load configuration from config_mgr
    uint32_t enabled = 0;
    ret = config_mgr_get_u32("udp/enabled", &enabled);
    if (ret != ESP_OK || enabled == 0) {
        ESP_LOGI(TAG, "UDP broadcast disabled in config");
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }

    // Load UDP configuration
    char addr[48] = {0};
    uint32_t port = 5005;
    uint32_t freq_mhz = 1000;  // Stored as millihertz (1 Hz = 1000 mHz)
    uint32_t ttl = 1;
    uint32_t mode = 0;

    config_mgr_get_string("udp/addr", addr, sizeof(addr));
    config_mgr_get_u32("udp/port", &port);
    if (config_mgr_get_u32("udp/freq_mhz", &freq_mhz) != ESP_OK) {
        // Backward compatibility for legacy key name
        config_mgr_get_u32("udp/freq_hz", &freq_mhz);
    }
    config_mgr_get_u32("udp/ttl", &ttl);
    config_mgr_get_u32("udp/mode", &mode);

    // Convert millihertz to Hz for validation
    float freq_hz = (float)freq_mhz / 1000.0f;

    // Build config structure (keep frequency in millihertz to preserve precision)
    s_config.mode = (udp_mode_t)mode;
    strlcpy(s_config.addr, addr, sizeof(s_config.addr));
    s_config.port = (uint16_t)port;
    s_config.freq_mhz = (uint16_t)freq_mhz;  // Store millihertz directly
    s_config.ttl = (uint8_t)ttl;

    // Validate frequency
    float validated_freq = validate_frequency(freq_hz);
    if (validated_freq != freq_hz) {
        ESP_LOGW(TAG, "Frequency adjusted from %.2f Hz to %.2f Hz", freq_hz, validated_freq);
    }

    // Setup destination address
    memset(&s_dest_addr, 0, sizeof(s_dest_addr));
    s_dest_addr.sin_family = AF_INET;
    s_dest_addr.sin_port = htons(s_config.port);
    if (inet_aton(s_config.addr, &s_dest_addr.sin_addr) == 0) {
        ESP_LOGE(TAG, "Invalid destination address: %s", s_config.addr);
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_ARG;
    }

    // Register event handlers for NET_READY/NET_LOST (before creating socket/timer)
    ret = esp_event_handler_register(DEVICE_EVENT, DEVICE_EVENT_NET_READY, &udp_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register NET_READY handler: %s", esp_err_to_name(ret));
        xSemaphoreGive(s_mutex);
        return ret;
    }

    ret = esp_event_handler_register(DEVICE_EVENT, DEVICE_EVENT_NET_LOST, &udp_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register NET_LOST handler: %s", esp_err_to_name(ret));
        esp_event_handler_unregister(DEVICE_EVENT, DEVICE_EVENT_NET_READY, &udp_event_handler);
        xSemaphoreGive(s_mutex);
        return ret;
    }

    s_is_running = true;
    s_is_paused = true;  // Start paused, will check network status below

    const char* mode_str = (s_config.mode == UDP_MODE_BROADCAST) ? "broadcast" :
                           (s_config.mode == UDP_MODE_MULTICAST) ? "multicast" : "unicast";
    ESP_LOGI(TAG, "UDP %s module initialized: %s:%d @ %.2f Hz (TTL=%d)",
             mode_str, s_config.addr, s_config.port, validated_freq, s_config.ttl);

    // CRITICAL: Check if network is already up (race condition fix)
    // If NET_READY already fired before handler registration, we need to start now
    if (net_mgr_is_ready()) {
        // Network is already up - start timer and socket immediately
        char ip_check[16] = "0.0.0.0";
        net_mgr_get_ip(ip_check, sizeof(ip_check));
        ESP_LOGI(TAG, "Network already ready (IP: %s), starting broadcasts immediately", ip_check);
        ret = udp_broadcast_start_timer_and_socket();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start timer and socket: %s", esp_err_to_name(ret));
            // Don't fail - will retry on next NET_READY event
        }
    } else {
        ESP_LOGI(TAG, "Waiting for network (NET_READY event)...");
    }

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t udp_broadcast_stop(void)
{
    ESP_LOGI(TAG, "Stopping UDP broadcast");

    if (!s_mutex) {
        return ESP_OK;
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire mutex");
        return ESP_FAIL;
    }

    if (!s_is_running) {
        ESP_LOGW(TAG, "UDP broadcast not running");
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }

    // Stop timer and close socket using helper
    udp_broadcast_stop_timer_and_socket();

    // Unregister event handlers
    esp_event_handler_unregister(DEVICE_EVENT, DEVICE_EVENT_NET_READY, &udp_event_handler);
    esp_event_handler_unregister(DEVICE_EVENT, DEVICE_EVENT_NET_LOST, &udp_event_handler);

    s_is_running = false;
    s_task_should_run = false;

    ESP_LOGI(TAG, "UDP broadcast stopped (sent %lu packets, %lu bytes, %lu errors)",
             s_packets_sent, s_bytes_sent, s_send_errors);

    // Post UDP_STOPPED event
    event_bus_post(DEVICE_EVENT, DEVICE_EVENT_UDP_STOPPED, NULL, 0, 0);

    xSemaphoreGive(s_mutex);

    // Wake broadcast task so it can exit if it's blocking on the queue
    if (s_broadcast_queue) {
        uint8_t trigger = 0;
        xQueueSend(s_broadcast_queue, &trigger, 0);
    }

    // Wait briefly for the task to cleanly exit
    for (int i = 0; i < 10 && s_broadcast_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    return ESP_OK;
}

esp_err_t udp_broadcast_apply_config(const udp_cfg_t* cfg)
{
    if (!cfg) {
        ESP_LOGE(TAG, "Invalid config pointer");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Applying new UDP config");

    // Convert millihertz to Hz for validation
    float freq_hz = (float)cfg->freq_mhz / 1000.0f;
    float validated_freq = validate_frequency(freq_hz);

    // Convert validated frequency back to millihertz
    // Use rounding to avoid truncation (e.g., 0.2*1000=199.999... → 200, not 199)
    uint32_t freq_mhz = (uint32_t)lroundf(validated_freq * 1000.0f);

    ESP_LOGI(TAG, "UDP config: mode=%d addr=%s port=%u freq=%.2f Hz (%lu mHz) ttl=%u",
             cfg->mode, cfg->addr, cfg->port, validated_freq, freq_mhz, cfg->ttl);

    // Restart if running
    bool was_running = s_is_running;
    if (was_running) {
        esp_err_t ret = udp_broadcast_stop();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to stop for config change: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    // Update config in NVS
    // Store frequency as millihertz to preserve fractional values (e.g., 0.2 Hz = 200 mHz)
    config_mgr_set_u32("udp/mode", (uint32_t)cfg->mode);
    config_mgr_set_string("udp/addr", cfg->addr);
    config_mgr_set_u32("udp/port", (uint32_t)cfg->port);
    config_mgr_set_u32("udp/freq_mhz", freq_mhz);  // Store as millihertz (key name matches units)
    config_mgr_set_u32("udp/ttl", (uint32_t)cfg->ttl);

    // Restart if was running
    if (was_running) {
        return udp_broadcast_start();
    }

    return ESP_OK;
}

esp_err_t udp_broadcast_publish_now(void)
{
    if (!s_mutex) {
        ESP_LOGE(TAG, "UDP broadcast not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire mutex");
        return ESP_FAIL;
    }

    if (!s_is_running) {
        ESP_LOGW(TAG, "UDP broadcast not running");
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    send_udp_packet();

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}
