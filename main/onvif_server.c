/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ONVIF Profile T implementation for ESP32-P4.
 *
 * Provides two services:
 *
 *   WS-Discovery (UDP 239.255.255.250:3702)
 *     Announces the device on startup (Hello) and responds to network
 *     scans (ProbeMatch), allowing ONVIF clients to find the camera
 *     without manual IP entry.
 *
 *   ONVIF HTTP/SOAP server (TCP port CONFIG_ONVIF_HTTP_PORT, default 80)
 *     Device service  : /onvif/device_service
 *     Media2 service  : /onvif/media_service
 *
 *     Returns the existing RTSP stream URI (rtsp://<ip>:554/stream)
 *     from GetStreamUri so that any Profile T client can play the
 *     H.264 feed encoded by the hardware H.264 block.
 *
 * Authentication is deliberately omitted — suitable for trusted LAN use.
 * The implementation uses a single-request-per-connection HTTP model and
 * allocates all large buffers from PSRAM (MALLOC_CAP_SPIRAM).
 */

#include "onvif_server.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "onvif";

/* ---- Compile-time constants --------------------------------------------- */

#define ONVIF_HTTP_PORT         CONFIG_ONVIF_HTTP_PORT
#define ONVIF_DISC_PORT         3702
#define ONVIF_MCAST_ADDR        "239.255.255.250"

/* Task parameters */
#define ONVIF_HTTP_TASK_STACK   12288
#define ONVIF_DISC_TASK_STACK   4096
#define ONVIF_TASK_PRIO         5

/* Buffer sizes (PSRAM-allocated) */
#define ONVIF_HTTP_REQ_SIZE     4096   /* Max SOAP request */
#define ONVIF_RESP_BUF_SIZE     8192   /* Max SOAP response (envelope + body) */
#define ONVIF_DISC_BUF_SIZE     2048   /* WS-Discovery message */

/* SOAP envelope namespaces injected into every response */
#define SOAP_NS \
    " xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\"" \
    " xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\"" \
    " xmlns:tr2=\"http://www.onvif.org/ver20/media/wsdl\"" \
    " xmlns:tt=\"http://www.onvif.org/ver10/schema\"" \
    " xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\""

/* ---- Static device identity (set once from MAC at startup) -------------- */

static char s_device_uuid[64];   /* urn:uuid:... */
static char s_serial_number[20]; /* hex MAC string */

/* ---- Utility helpers ---------------------------------------------------- */

static void get_local_ip(char *buf, size_t buflen)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
    if (!netif) {
        snprintf(buf, buflen, "0.0.0.0");
        return;
    }
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        snprintf(buf, buflen, IPSTR, IP2STR(&ip_info.ip));
    } else {
        snprintf(buf, buflen, "0.0.0.0");
    }
}

/* Wait up to timeout_ms for a non-zero IP on the Ethernet interface. */
static bool wait_for_ip(int timeout_ms)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
    if (!netif) {
        return false;
    }
    for (int elapsed = 0; elapsed < timeout_ms; elapsed += 200) {
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr != 0) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return false;
}

/*
 * Derive a stable UUID (v4-ish) and serial number from the Ethernet MAC.
 * UUID format: xxxxxxxx-xxxx-4xxx-8xxx-xxxxxxxxxxxx
 */
static void gen_device_identity(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_ETH);

    /*
     * Build a deterministic UUID derived from the Ethernet MAC address.
     * Format: xxxxxxxx-xxxx-4xxx-8xxx-xxxxxxxxxxxx
     * Version nibble set to 4, variant nibble set to 8..b.
     * Not a true random UUIDv4 — MAC bytes fill all fields for stability.
     */
    snprintf(s_device_uuid, sizeof(s_device_uuid),
             "urn:uuid:%02x%02x%02x%02x-%02x%02x-4%01x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5],
             mac[0] & 0x0f, mac[1],
             (mac[2] & 0x3f) | 0x80, mac[3],
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    snprintf(s_serial_number, sizeof(s_serial_number),
             "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* ---- HTTP / SOAP helpers ------------------------------------------------ */

/*
 * Read a complete HTTP request into buf:
 *   - reads until the header terminator "\r\n\r\n"
 *   - then reads the remaining body bytes from Content-Length
 * Returns total bytes read, -1 on error / timeout.
 */
static int read_http_request(int fd, char *buf, int bufsize)
{
    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int total = 0;
    int content_length = 0;
    int header_end = -1;

    while (total < bufsize - 1) {
        int n = recv(fd, buf + total, bufsize - total - 1, 0);
        if (n <= 0) {
            if (total == 0) {
                return -1;
            }
            break;
        }
        total += n;
        buf[total] = '\0';

        /* Locate end-of-headers on the first pass */
        if (header_end < 0) {
            char *p = strstr(buf, "\r\n\r\n");
            if (p) {
                header_end = (int)(p - buf) + 4;
                /* Extract Content-Length */
                const char *cl = strstr(buf, "Content-Length:");
                if (!cl) {
                    cl = strstr(buf, "content-length:");
                }
                if (cl) {
                    content_length = atoi(cl + 15);
                }
            }
        }

        /* Stop once we have the full body */
        if (header_end > 0) {
            int body_received = total - header_end;
            if (content_length <= 0 || body_received >= content_length) {
                break;
            }
        }
    }

    return total;
}

/*
 * Send an HTTP 200 OK response whose body is a complete SOAP envelope
 * wrapping soap_body.  Allocates the response buffer from PSRAM.
 */
static void send_soap_ok(int fd, const char *soap_body)
{
    static const char SOAP_OPEN[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<s:Envelope" SOAP_NS "><s:Body>";
    static const char SOAP_CLOSE[] = "</s:Body></s:Envelope>";

    int body_len = (int)(sizeof(SOAP_OPEN) - 1 + strlen(soap_body)
                         + sizeof(SOAP_CLOSE) - 1);

    char *resp = heap_caps_malloc(ONVIF_RESP_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!resp) {
        ESP_LOGE(TAG, "send_soap_ok: PSRAM alloc failed");
        return;
    }

    /* HTTP header */
    int n = snprintf(resp, ONVIF_RESP_BUF_SIZE,
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: application/soap+xml; charset=utf-8\r\n"
                     "Content-Length: %d\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     body_len);

    /* SOAP envelope */
    int remaining = ONVIF_RESP_BUF_SIZE - n;
    if (remaining > body_len + 1) {
        memcpy(resp + n, SOAP_OPEN, sizeof(SOAP_OPEN) - 1);
        n += (int)(sizeof(SOAP_OPEN) - 1);
        memcpy(resp + n, soap_body, strlen(soap_body));
        n += (int)strlen(soap_body);
        memcpy(resp + n, SOAP_CLOSE, sizeof(SOAP_CLOSE) - 1);
        n += (int)(sizeof(SOAP_CLOSE) - 1);
    } else {
        ESP_LOGE(TAG, "send_soap_ok: response too large (%d bytes)", body_len);
        heap_caps_free(resp);
        return;
    }

    send(fd, resp, n, 0);
    heap_caps_free(resp);
}

/* Send a minimal HTTP error response. */
static void send_http_error(int fd, int code, const char *reason)
{
    char resp[128];
    snprintf(resp, sizeof(resp),
             "HTTP/1.1 %d %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
             code, reason);
    send(fd, resp, strlen(resp), 0);
}

/*
 * Copy the last path component from a URL-like string into dst[63].
 * Stops at '"', '\'', '\r', '\n', ';', or end-of-string.
 * Returns true if at least one character was copied.
 */
static bool last_path_component(const char *url, char *dst)
{
    const char *slash = strrchr(url, '/');
    if (!slash) {
        return false;
    }
    const char *start = slash + 1;
    int i = 0;
    while (start[i] && start[i] != '"' && start[i] != '\'' &&
           start[i] != '\r' && start[i] != '\n' &&
           start[i] != ';'  && i < 63) {
        dst[i] = start[i];
        i++;
    }
    dst[i] = '\0';
    return i > 0;
}

/*
 * Extract the SOAP method name from an HTTP/SOAP request buffer.
 *
 * Tries (in order):
 *   1. HTTP SOAPAction header value (last path component)
 *   2. action parameter in Content-Type (SOAP 1.2)
 *   3. First element name inside <s:Body> / <soap:Body>
 *
 * Returns a pointer to a static buffer, or NULL if nothing found.
 */
static const char *extract_soap_method(const char *req)
{
    static char method[64];

    /* 1. SOAPAction header */
    const char *sa = strstr(req, "SOAPAction:");
    if (!sa) {
        sa = strstr(req, "soapaction:");
    }
    if (sa) {
        sa += 11; /* skip "SOAPAction:" */
        while (*sa == ' ' || *sa == '\t' || *sa == '"') {
            sa++;
        }
        if (last_path_component(sa, method)) {
            return method;
        }
    }

    /* 2. action= parameter in Content-Type (SOAP 1.2 style) */
    const char *ct = strstr(req, "action=");
    if (ct) {
        ct += 7;
        while (*ct == '"' || *ct == ' ') {
            ct++;
        }
        if (last_path_component(ct, method)) {
            return method;
        }
    }

    /* 3. First element in <s:Body> (namespace-aware) */
    const char *body = strstr(req, ":Body>");
    if (body) {
        const char *p = body + 6; /* skip ":Body>" */
        while (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t') {
            p++;
        }
        if (*p == '<') {
            p++; /* skip '<' */
            /* Skip namespace prefix (everything up to and including ':') */
            const char *colon = NULL;
            const char *start = p;
            const char *scan = p;
            while (*scan && *scan != ' ' && *scan != '>' && *scan != '/') {
                if (*scan == ':') {
                    colon = scan;
                }
                scan++;
            }
            if (colon) {
                start = colon + 1;
            }
            int len = (int)(scan - start);
            if (len > 0 && len < 63) {
                memcpy(method, start, len);
                method[len] = '\0';
                return method;
            }
        }
    }

    return NULL;
}

/* ---- SOAP response builders --------------------------------------------- */

static void handle_GetSystemDateAndTime(int fd)
{
    send_soap_ok(fd,
        "<tds:GetSystemDateAndTimeResponse>"
          "<tds:SystemDateAndTime>"
            "<tt:DateTimeType>Manual</tt:DateTimeType>"
            "<tt:DaylightSavings>false</tt:DaylightSavings>"
            "<tt:TimeZone><tt:TZ>UTC</tt:TZ></tt:TimeZone>"
            "<tt:UTCDateTime>"
              "<tt:Time>"
                "<tt:Hour>0</tt:Hour>"
                "<tt:Minute>0</tt:Minute>"
                "<tt:Second>0</tt:Second>"
              "</tt:Time>"
              "<tt:Date>"
                "<tt:Year>2025</tt:Year>"
                "<tt:Month>1</tt:Month>"
                "<tt:Day>1</tt:Day>"
              "</tt:Date>"
            "</tt:UTCDateTime>"
          "</tds:SystemDateAndTime>"
        "</tds:GetSystemDateAndTimeResponse>");
}

static void handle_GetDeviceInformation(int fd)
{
    char body[512];
    snprintf(body, sizeof(body),
        "<tds:GetDeviceInformationResponse>"
          "<tds:Manufacturer>Espressif</tds:Manufacturer>"
          "<tds:Model>ESP32-P4-UVC-Camera</tds:Model>"
          "<tds:FirmwareVersion>1.0.0</tds:FirmwareVersion>"
          "<tds:SerialNumber>%s</tds:SerialNumber>"
          "<tds:HardwareId>ESP32-P4</tds:HardwareId>"
        "</tds:GetDeviceInformationResponse>",
        s_serial_number);
    send_soap_ok(fd, body);
}

static void handle_GetCapabilities(int fd)
{
    char local_ip[32];
    get_local_ip(local_ip, sizeof(local_ip));

    char body[1024];
    snprintf(body, sizeof(body),
        "<tds:GetCapabilitiesResponse>"
          "<tds:Capabilities>"
            "<tt:Device>"
              "<tt:XAddr>http://%s:%d/onvif/device_service</tt:XAddr>"
              "<tt:System>"
                "<tt:DiscoveryResolve>false</tt:DiscoveryResolve>"
                "<tt:DiscoveryBye>false</tt:DiscoveryBye>"
                "<tt:RemoteDiscovery>false</tt:RemoteDiscovery>"
                "<tt:SystemBackup>false</tt:SystemBackup>"
                "<tt:SystemLogging>false</tt:SystemLogging>"
                "<tt:FirmwareUpgrade>false</tt:FirmwareUpgrade>"
              "</tt:System>"
              "<tt:IO>"
                "<tt:InputConnectors>0</tt:InputConnectors>"
                "<tt:RelayOutputs>0</tt:RelayOutputs>"
              "</tt:IO>"
            "</tt:Device>"
            "<tt:Media>"
              "<tt:XAddr>http://%s:%d/onvif/media_service</tt:XAddr>"
              "<tt:StreamingCapabilities>"
                "<tt:RTPMulticast>false</tt:RTPMulticast>"
                "<tt:RTP_TCP>false</tt:RTP_TCP>"
                "<tt:RTP_RTSP_TCP>true</tt:RTP_RTSP_TCP>"
              "</tt:StreamingCapabilities>"
            "</tt:Media>"
          "</tds:Capabilities>"
        "</tds:GetCapabilitiesResponse>",
        local_ip, ONVIF_HTTP_PORT,
        local_ip, ONVIF_HTTP_PORT);
    send_soap_ok(fd, body);
}

static void handle_GetServices(int fd)
{
    char local_ip[32];
    get_local_ip(local_ip, sizeof(local_ip));

    char body[1024];
    snprintf(body, sizeof(body),
        "<tds:GetServicesResponse>"
          "<tds:Service>"
            "<tds:Namespace>http://www.onvif.org/ver10/device/wsdl</tds:Namespace>"
            "<tds:XAddr>http://%s:%d/onvif/device_service</tds:XAddr>"
            "<tds:Version>"
              "<tt:Major>2</tt:Major>"
              "<tt:Minor>60</tt:Minor>"
            "</tds:Version>"
          "</tds:Service>"
          "<tds:Service>"
            "<tds:Namespace>http://www.onvif.org/ver20/media/wsdl</tds:Namespace>"
            "<tds:XAddr>http://%s:%d/onvif/media_service</tds:XAddr>"
            "<tds:Version>"
              "<tt:Major>2</tt:Major>"
              "<tt:Minor>0</tt:Minor>"
            "</tds:Version>"
          "</tds:Service>"
        "</tds:GetServicesResponse>",
        local_ip, ONVIF_HTTP_PORT,
        local_ip, ONVIF_HTTP_PORT);
    send_soap_ok(fd, body);
}

/* Device service capabilities */
static void handle_GetServiceCapabilities_device(int fd)
{
    send_soap_ok(fd,
        "<tds:GetServiceCapabilitiesResponse>"
          "<tds:Capabilities>"
            "<tds:Network IPFilter=\"false\" ZeroConfiguration=\"false\""
              " IPVersion6=\"false\" DynDNS=\"false\""
              " Dot11Configuration=\"false\" Dot1XConfigurations=\"0\""
              " HostnameFromDHCP=\"false\" NTP=\"0\" DHCPv6=\"false\"/>"
            "<tds:Security TLS1.0=\"false\" TLS1.1=\"false\" TLS1.2=\"false\""
              " OnboardKeyGeneration=\"false\" AccessPolicyConfig=\"false\""
              " DefaultAccessPolicy=\"false\" Dot1X=\"false\""
              " RemoteUserHandling=\"false\" X.509Token=\"false\""
              " SAMLToken=\"false\" KerberosToken=\"false\""
              " UsernameToken=\"false\" HttpDigest=\"false\""
              " RELToken=\"false\" SupportedEAPMethods=\"0\""
              " MaxUsers=\"1\" MaxUserNameLength=\"32\""
              " MaxPasswordLength=\"64\"/>"
            "<tds:System DiscoveryResolve=\"false\" DiscoveryBye=\"false\""
              " RemoteDiscovery=\"false\" SystemBackup=\"false\""
              " SystemLogging=\"false\" FirmwareUpgrade=\"false\"/>"
          "</tds:Capabilities>"
        "</tds:GetServiceCapabilitiesResponse>");
}

/* Media2 service capabilities */
static void handle_GetServiceCapabilities_media(int fd)
{
    send_soap_ok(fd,
        "<tr2:GetServiceCapabilitiesResponse>"
          "<tr2:Capabilities>"
            "<tr2:ProfileCapabilities MaximumNumberOfProfiles=\"1\"/>"
            "<tr2:StreamingCapabilities RTSPStreaming=\"true\""
              " RTPMulticast=\"false\" RTP_RTSP_TCP=\"true\""
              " NonAggregateControl=\"false\""
              " AutoStartMulticast=\"false\"/>"
          "</tr2:Capabilities>"
        "</tr2:GetServiceCapabilitiesResponse>");
}

static void handle_GetProfiles(int fd)
{
    char body[2048];
    snprintf(body, sizeof(body),
        "<tr2:GetProfilesResponse>"
          "<tr2:Profiles token=\"profile_h264\" fixed=\"true\">"
            "<tt:Name>H264_1080p_Profile</tt:Name>"
            "<tr2:Configurations>"
              "<tr2:VideoSource token=\"vs_config\">"
                "<tt:Name>VideoSourceConfig</tt:Name>"
                "<tt:UseCount>1</tt:UseCount>"
                "<tt:SourceToken>video_source</tt:SourceToken>"
                "<tt:Bounds x=\"0\" y=\"0\" width=\"1920\" height=\"1080\"/>"
              "</tr2:VideoSource>"
              "<tr2:VideoEncoder token=\"ve_config\">"
                "<tt:Name>H264Encoder</tt:Name>"
                "<tt:UseCount>1</tt:UseCount>"
                "<tt:Encoding>H264</tt:Encoding>"
                "<tt:Resolution>"
                  "<tt:Width>1920</tt:Width>"
                  "<tt:Height>1080</tt:Height>"
                "</tt:Resolution>"
                "<tt:Quality>7.5</tt:Quality>"
                "<tt:RateControl>"
                  "<tt:FrameRateLimit>30</tt:FrameRateLimit>"
                  "<tt:EncodingInterval>1</tt:EncodingInterval>"
                  "<tt:BitrateLimit>%d</tt:BitrateLimit>"
                "</tt:RateControl>"
                "<tt:H264>"
                  "<tt:GovLength>%d</tt:GovLength>"
                  "<tt:H264Profile>Baseline</tt:H264Profile>"
                "</tt:H264>"
                "<tt:Multicast>"
                  "<tt:Address>"
                    "<tt:Type>IPv4</tt:Type>"
                    "<tt:IPv4Address>0.0.0.0</tt:IPv4Address>"
                  "</tt:Address>"
                  "<tt:Port>0</tt:Port>"
                  "<tt:TTL>0</tt:TTL>"
                  "<tt:AutoStart>false</tt:AutoStart>"
                "</tt:Multicast>"
                "<tt:SessionTimeout>PT60S</tt:SessionTimeout>"
              "</tr2:VideoEncoder>"
            "</tr2:Configurations>"
          "</tr2:Profiles>"
        "</tr2:GetProfilesResponse>",
        CONFIG_RTSP_H264_BITRATE / 1000,  /* kbps */
        CONFIG_RTSP_H264_I_PERIOD);
    send_soap_ok(fd, body);
}

static void handle_GetStreamUri(int fd)
{
    char local_ip[32];
    get_local_ip(local_ip, sizeof(local_ip));

    char body[256];
    snprintf(body, sizeof(body),
        "<tr2:GetStreamUriResponse>"
          "<tr2:Uri>rtsp://%s:%d/stream</tr2:Uri>"
        "</tr2:GetStreamUriResponse>",
        local_ip, CONFIG_ETH_RTSP_PORT);
    send_soap_ok(fd, body);
}

static void handle_GetVideoSources(int fd)
{
    send_soap_ok(fd,
        "<tr2:GetVideoSourcesResponse>"
          "<tr2:VideoSources token=\"video_source\">"
            "<tt:Framerate>30</tt:Framerate>"
            "<tt:Resolution>"
              "<tt:Width>1920</tt:Width>"
              "<tt:Height>1080</tt:Height>"
            "</tt:Resolution>"
            "<tt:Imaging/>"
          "</tr2:VideoSources>"
        "</tr2:GetVideoSourcesResponse>");
}

static void handle_GetVideoSourceConfigurations(int fd)
{
    send_soap_ok(fd,
        "<tr2:GetVideoSourceConfigurationsResponse>"
          "<tr2:Configurations token=\"vs_config\">"
            "<tt:Name>VideoSourceConfig</tt:Name>"
            "<tt:UseCount>1</tt:UseCount>"
            "<tt:SourceToken>video_source</tt:SourceToken>"
            "<tt:Bounds x=\"0\" y=\"0\" width=\"1920\" height=\"1080\"/>"
          "</tr2:Configurations>"
        "</tr2:GetVideoSourceConfigurationsResponse>");
}

static void handle_GetVideoEncoderConfigurations(int fd)
{
    char body[1024];
    snprintf(body, sizeof(body),
        "<tr2:GetVideoEncoderConfigurationsResponse>"
          "<tr2:Configurations token=\"ve_config\">"
            "<tt:Name>H264Encoder</tt:Name>"
            "<tt:UseCount>1</tt:UseCount>"
            "<tt:Encoding>H264</tt:Encoding>"
            "<tt:Resolution>"
              "<tt:Width>1920</tt:Width>"
              "<tt:Height>1080</tt:Height>"
            "</tt:Resolution>"
            "<tt:Quality>7.5</tt:Quality>"
            "<tt:RateControl>"
              "<tt:FrameRateLimit>30</tt:FrameRateLimit>"
              "<tt:EncodingInterval>1</tt:EncodingInterval>"
              "<tt:BitrateLimit>%d</tt:BitrateLimit>"
            "</tt:RateControl>"
            "<tt:H264>"
              "<tt:GovLength>%d</tt:GovLength>"
              "<tt:H264Profile>Baseline</tt:H264Profile>"
            "</tt:H264>"
          "</tr2:Configurations>"
        "</tr2:GetVideoEncoderConfigurationsResponse>",
        CONFIG_RTSP_H264_BITRATE / 1000,
        CONFIG_RTSP_H264_I_PERIOD);
    send_soap_ok(fd, body);
}

static void handle_GetVideoEncoderConfigurationOptions(int fd)
{
    send_soap_ok(fd,
        "<tr2:GetVideoEncoderConfigurationOptionsResponse>"
          "<tr2:Options token=\"ve_config\">"
            "<tr2:VideoEncoderOptionsExtension>"
              "<tt:H264>"
                "<tt:ResolutionsAvailable>"
                  "<tt:Width>1920</tt:Width>"
                  "<tt:Height>1080</tt:Height>"
                "</tt:ResolutionsAvailable>"
                "<tt:ResolutionsAvailable>"
                  "<tt:Width>1280</tt:Width>"
                  "<tt:Height>720</tt:Height>"
                "</tt:ResolutionsAvailable>"
                "<tt:ResolutionsAvailable>"
                  "<tt:Width>640</tt:Width>"
                  "<tt:Height>480</tt:Height>"
                "</tt:ResolutionsAvailable>"
                "<tt:GovLengthRange>"
                  "<tt:Min>1</tt:Min>"
                  "<tt:Max>120</tt:Max>"
                "</tt:GovLengthRange>"
                "<tt:FrameRateRange>"
                  "<tt:Min>1</tt:Min>"
                  "<tt:Max>30</tt:Max>"
                "</tt:FrameRateRange>"
                "<tt:EncodingIntervalRange>"
                  "<tt:Min>1</tt:Min>"
                  "<tt:Max>1</tt:Max>"
                "</tt:EncodingIntervalRange>"
                "<tt:H264ProfilesSupported>Baseline</tt:H264ProfilesSupported>"
              "</tt:H264>"
            "</tr2:VideoEncoderOptionsExtension>"
          "</tr2:Options>"
        "</tr2:GetVideoEncoderConfigurationOptionsResponse>");
}

/* SOAP fault for any unrecognised action */
static void send_soap_fault(int fd, const char *action)
{
    char body[384];
    snprintf(body, sizeof(body),
        "<s:Fault>"
          "<s:Code>"
            "<s:Value>s:Sender</s:Value>"
            "<s:Subcode>"
              "<s:Value>ter:ActionNotSupported</s:Value>"
            "</s:Subcode>"
          "</s:Code>"
          "<s:Reason>"
            "<s:Text xml:lang=\"en\">"
              "Optional Action Not Implemented: %s"
            "</s:Text>"
          "</s:Reason>"
        "</s:Fault>",
        action ? action : "unknown");
    send_soap_ok(fd, body);
}

/* ---- HTTP request dispatcher -------------------------------------------- */

static void handle_onvif_client(int fd)
{
    char *req = heap_caps_malloc(ONVIF_HTTP_REQ_SIZE, MALLOC_CAP_SPIRAM);
    if (!req) {
        send_http_error(fd, 500, "Internal Server Error");
        return;
    }

    int n = read_http_request(fd, req, ONVIF_HTTP_REQ_SIZE);
    if (n <= 0) {
        heap_caps_free(req);
        return;
    }

    /* Only handle POST */
    if (strncmp(req, "POST", 4) != 0) {
        send_http_error(fd, 405, "Method Not Allowed");
        heap_caps_free(req);
        return;
    }

    const char *method = extract_soap_method(req);
    if (!method) {
        send_http_error(fd, 400, "Bad Request");
        heap_caps_free(req);
        return;
    }

    ESP_LOGD(TAG, "SOAP method: %s", method);

    /* Route to handler.  GetServiceCapabilities is path-dependent. */
    if (strcmp(method, "GetSystemDateAndTime") == 0) {
        handle_GetSystemDateAndTime(fd);
    } else if (strcmp(method, "GetDeviceInformation") == 0) {
        handle_GetDeviceInformation(fd);
    } else if (strcmp(method, "GetCapabilities") == 0) {
        handle_GetCapabilities(fd);
    } else if (strcmp(method, "GetServices") == 0) {
        handle_GetServices(fd);
    } else if (strcmp(method, "GetServiceCapabilities") == 0) {
        if (strstr(req, "/onvif/media") != NULL) {
            handle_GetServiceCapabilities_media(fd);
        } else {
            handle_GetServiceCapabilities_device(fd);
        }
    } else if (strcmp(method, "GetProfiles") == 0) {
        handle_GetProfiles(fd);
    } else if (strcmp(method, "GetStreamUri") == 0) {
        handle_GetStreamUri(fd);
    } else if (strcmp(method, "GetVideoSources") == 0) {
        handle_GetVideoSources(fd);
    } else if (strcmp(method, "GetVideoSourceConfigurations") == 0) {
        handle_GetVideoSourceConfigurations(fd);
    } else if (strcmp(method, "GetVideoEncoderConfigurations") == 0) {
        handle_GetVideoEncoderConfigurations(fd);
    } else if (strcmp(method, "GetVideoEncoderConfigurationOptions") == 0) {
        handle_GetVideoEncoderConfigurationOptions(fd);
    } else {
        ESP_LOGW(TAG, "Unhandled SOAP method: %s", method);
        send_soap_fault(fd, method);
    }

    heap_caps_free(req);
}

/* ---- ONVIF HTTP/SOAP server task ---------------------------------------- */

static void http_server_task(void *arg)
{
    if (!wait_for_ip(30000)) {
        ESP_LOGW(TAG, "ONVIF HTTP: no IP in 30 s, server may be unreachable");
    }

    int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_fd < 0) {
        ESP_LOGE(TAG, "HTTP socket create failed: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port        = htons(ONVIF_HTTP_PORT),
    };

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "HTTP bind failed: errno %d", errno);
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }

    if (listen(listen_fd, 4) < 0) {
        ESP_LOGE(TAG, "HTTP listen failed: errno %d", errno);
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "ONVIF HTTP server listening on port %d", ONVIF_HTTP_PORT);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        handle_onvif_client(client_fd);
        close(client_fd);
    }
}

/* ---- WS-Discovery helpers ----------------------------------------------- */

/*
 * Build a WS-Discovery Hello message into buf.
 * Returns the message length or -1 if buf is too small.
 */
static int build_hello(char *buf, int bufsize, const char *local_ip)
{
    return snprintf(buf, bufsize,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<s:Envelope"
        " xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""
        " xmlns:a=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\""
        " xmlns:d=\"http://docs.oasis-open.org/ws-dd/ns/discovery/2009/01\""
        " xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\">"
        "<s:Header>"
          "<a:Action>"
            "http://docs.oasis-open.org/ws-dd/ns/discovery/2009/01/Hello"
          "</a:Action>"
          "<a:MessageID>%s-hello</a:MessageID>"
          "<a:To>"
            "urn:docs-oasis-open-org:ws-dd:ns:discovery:2009:01:Multicast"
          "</a:To>"
        "</s:Header>"
        "<s:Body>"
          "<d:Hello>"
            "<a:EndpointReference>"
              "<a:Address>%s</a:Address>"
            "</a:EndpointReference>"
            "<d:Types>dn:NetworkVideoTransmitter</d:Types>"
            "<d:Scopes>"
              "onvif://www.onvif.org/type/video_encoder "
              "onvif://www.onvif.org/Profile/T"
            "</d:Scopes>"
            "<d:XAddrs>http://%s:%d/onvif/device_service</d:XAddrs>"
            "<d:MetadataVersion>1</d:MetadataVersion>"
          "</d:Hello>"
        "</s:Body>"
        "</s:Envelope>",
        s_device_uuid,
        s_device_uuid,
        local_ip, ONVIF_HTTP_PORT);
}

/*
 * Build a WS-Discovery ProbeMatch response into buf.
 * relates_to: the MessageID from the Probe (for RelatesTo header).
 */
static int build_probe_match(char *buf, int bufsize, const char *local_ip,
                              const char *relates_to)
{
    return snprintf(buf, bufsize,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<s:Envelope"
        " xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""
        " xmlns:a=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\""
        " xmlns:d=\"http://docs.oasis-open.org/ws-dd/ns/discovery/2009/01\""
        " xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\">"
        "<s:Header>"
          "<a:Action>"
            "http://docs.oasis-open.org/ws-dd/ns/discovery/2009/01/ProbeMatches"
          "</a:Action>"
          "<a:MessageID>%s-match</a:MessageID>"
          "<a:RelatesTo>%s</a:RelatesTo>"
          "<a:To>"
            "http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous"
          "</a:To>"
        "</s:Header>"
        "<s:Body>"
          "<d:ProbeMatches>"
            "<d:ProbeMatch>"
              "<a:EndpointReference>"
                "<a:Address>%s</a:Address>"
              "</a:EndpointReference>"
              "<d:Types>dn:NetworkVideoTransmitter</d:Types>"
              "<d:Scopes>"
                "onvif://www.onvif.org/type/video_encoder "
                "onvif://www.onvif.org/Profile/T"
              "</d:Scopes>"
              "<d:XAddrs>http://%s:%d/onvif/device_service</d:XAddrs>"
              "<d:MetadataVersion>1</d:MetadataVersion>"
            "</d:ProbeMatch>"
          "</d:ProbeMatches>"
        "</s:Body>"
        "</s:Envelope>",
        s_device_uuid,
        relates_to,
        s_device_uuid,
        local_ip, ONVIF_HTTP_PORT);
}

/*
 * Extract the MessageID value from a WS-Discovery message.
 * Returns true on success.
 */
static bool extract_message_id(const char *msg, char *id_buf, int id_bufsize)
{
    const char *p = strstr(msg, ":MessageID>");
    if (!p) {
        return false;
    }
    p = strchr(p, '>');
    if (!p) {
        return false;
    }
    p++;

    const char *end = strchr(p, '<');
    if (!end) {
        return false;
    }

    int len = (int)(end - p);
    if (len <= 0 || len >= id_bufsize) {
        return false;
    }

    memcpy(id_buf, p, len);
    id_buf[len] = '\0';
    return true;
}

/* ---- WS-Discovery task -------------------------------------------------- */

static void discovery_task(void *arg)
{
    if (!wait_for_ip(30000)) {
        ESP_LOGW(TAG, "WS-Discovery: no IP after 30 s, aborting");
        vTaskDelete(NULL);
        return;
    }

    char local_ip[32];
    get_local_ip(local_ip, sizeof(local_ip));

    /* Create UDP socket */
    int disc_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (disc_fd < 0) {
        ESP_LOGE(TAG, "Discovery socket failed: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(disc_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in bind_addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port        = htons(ONVIF_DISC_PORT),
    };

    if (bind(disc_fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "Discovery bind failed: errno %d", errno);
        close(disc_fd);
        vTaskDelete(NULL);
        return;
    }

    /* Join WS-Discovery multicast group on the Ethernet interface */
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
    if (netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            struct ip_mreq mreq;
            mreq.imr_multiaddr.s_addr = inet_addr(ONVIF_MCAST_ADDR);
            mreq.imr_interface.s_addr = ip_info.ip.addr;
            if (setsockopt(disc_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                           &mreq, sizeof(mreq)) < 0) {
                ESP_LOGW(TAG, "Multicast join failed: errno %d (continuing)", errno);
            }
        }
    }

    /* Receive timeout so we can refresh the IP periodically */
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(disc_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char *buf = heap_caps_malloc(ONVIF_DISC_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "Discovery buffer alloc failed");
        close(disc_fd);
        vTaskDelete(NULL);
        return;
    }

    /* Send Hello on the multicast group so scanners see us immediately */
    struct sockaddr_in mcast_addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(ONVIF_DISC_PORT),
    };
    mcast_addr.sin_addr.s_addr = inet_addr(ONVIF_MCAST_ADDR);

    int hello_len = build_hello(buf, ONVIF_DISC_BUF_SIZE, local_ip);
    if (hello_len > 0 && hello_len < ONVIF_DISC_BUF_SIZE) {
        sendto(disc_fd, buf, hello_len, 0,
               (struct sockaddr *)&mcast_addr, sizeof(mcast_addr));
        ESP_LOGI(TAG, "WS-Discovery Hello sent (UUID=%s)", s_device_uuid);
    }

    ESP_LOGI(TAG, "WS-Discovery listening on %s:%d", ONVIF_MCAST_ADDR, ONVIF_DISC_PORT);

    while (1) {
        struct sockaddr_in sender;
        socklen_t sender_len = sizeof(sender);

        int n = recvfrom(disc_fd, buf, ONVIF_DISC_BUF_SIZE - 1, 0,
                         (struct sockaddr *)&sender, &sender_len);
        if (n <= 0) {
            /* Refresh local IP on each timeout in case of DHCP renewal */
            get_local_ip(local_ip, sizeof(local_ip));
            continue;
        }
        buf[n] = '\0';

        /* Only respond to Probe messages (ignore ProbeMatches, Hello, etc.) */
        if (!strstr(buf, "Probe") || strstr(buf, "ProbeMatch")) {
            continue;
        }

        /* Extract the probe's MessageID for the RelatesTo header */
        char probe_id[128] = "urn:uuid:unknown";
        extract_message_id(buf, probe_id, sizeof(probe_id));

        get_local_ip(local_ip, sizeof(local_ip));

        int match_len = build_probe_match(buf, ONVIF_DISC_BUF_SIZE,
                                          local_ip, probe_id);
        if (match_len > 0 && match_len < ONVIF_DISC_BUF_SIZE) {
            sendto(disc_fd, buf, match_len, 0,
                   (struct sockaddr *)&sender, sender_len);

            uint8_t *ip = (uint8_t *)&sender.sin_addr.s_addr;
            ESP_LOGI(TAG, "WS-Discovery ProbeMatch sent to %d.%d.%d.%d",
                     ip[0], ip[1], ip[2], ip[3]);
        }
    }

    /* Never reached, but clean up for completeness */
    heap_caps_free(buf);
    close(disc_fd);
}

/* ---- Public API --------------------------------------------------------- */

esp_err_t onvif_server_start(void)
{
    gen_device_identity();

    ESP_LOGI(TAG, "Starting ONVIF Profile T service");
    ESP_LOGI(TAG, "  Device UUID  : %s", s_device_uuid);
    ESP_LOGI(TAG, "  HTTP port    : %d", ONVIF_HTTP_PORT);

    BaseType_t ret = xTaskCreate(discovery_task, "onvif_disc",
                                  ONVIF_DISC_TASK_STACK, NULL,
                                  ONVIF_TASK_PRIO, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Discovery task create failed");
        return ESP_ERR_NO_MEM;
    }

    ret = xTaskCreate(http_server_task, "onvif_http",
                       ONVIF_HTTP_TASK_STACK, NULL,
                       ONVIF_TASK_PRIO, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "HTTP server task create failed");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
