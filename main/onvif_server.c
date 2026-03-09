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
 *     Uses the esp_http_server component for connection management and
 *     the expat XML library for robust SOAP method extraction.
 *
 *     Device service  : /onvif/device_service
 *     Media2 service  : /onvif/media_service
 *
 *     Returns the existing RTSP stream URI (rtsp://<ip>:554/stream)
 *     from GetStreamUri so that any Profile T client can play the
 *     H.264 feed encoded by the hardware H.264 block.
 *
 * Authentication is deliberately omitted — suitable for trusted LAN use.
 */

#include "onvif_server.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "expat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "onvif";

/* ---- Compile-time constants --------------------------------------------- */

#define ONVIF_HTTP_PORT CONFIG_ONVIF_HTTP_PORT
#define ONVIF_DISC_PORT 3702
#define ONVIF_MCAST_ADDR "239.255.255.250"

/* Task / server parameters */
#define ONVIF_HTTP_TASK_STACK 12288
#define ONVIF_DISC_TASK_STACK 4096
#define ONVIF_TASK_PRIO 5

/* Maximum SOAP request body accepted from clients */
#define ONVIF_HTTP_REQ_SIZE 4096

/* WS-Discovery UDP message buffer */
#define ONVIF_DISC_BUF_SIZE 2048

/* SOAP envelope namespaces injected into every response */
#define SOAP_NS                                           \
  " xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""  \
  " xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\"" \
  " xmlns:tr2=\"http://www.onvif.org/ver20/media/wsdl\""  \
  " xmlns:tt=\"http://www.onvif.org/ver10/schema\""       \
  " xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\""

/* ---- Static device identity (set once from MAC at startup) -------------- */

static char s_device_uuid[64];   /* urn:uuid:... */
static char s_serial_number[20]; /* hex MAC string */

/* ---- Utility helpers ---------------------------------------------------- */

static void get_local_ip(char *buf, size_t buflen)
{
  esp_netif_t *netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
  if (!netif)
  {
    snprintf(buf, buflen, "0.0.0.0");
    return;
  }
  esp_netif_ip_info_t ip_info;
  if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0)
  {
    snprintf(buf, buflen, IPSTR, IP2STR(&ip_info.ip));
  }
  else
  {
    snprintf(buf, buflen, "0.0.0.0");
  }
}

/* Wait up to timeout_ms for a non-zero IP on the Ethernet interface. */
static bool wait_for_ip(int timeout_ms)
{
  esp_netif_t *netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
  if (!netif)
  {
    return false;
  }
  for (int elapsed = 0; elapsed < timeout_ms; elapsed += 200)
  {
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr != 0)
    {
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  return false;
}

/*
 * Derive a stable UUID and serial number from the Ethernet MAC.
 * Not a true random UUIDv4 — MAC bytes fill all fields for stability.
 */
static void gen_device_identity(void)
{
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_ETH);

  /*
   * Build a deterministic UUID derived from the Ethernet MAC address.
   * Format: xxxxxxxx-xxxx-4xxx-8xxx-xxxxxxxxxxxx
   * Version nibble set to 4, variant nibble set to 8..b.
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

/* ---- Expat XML parsing -------------------------------------------------- */

/*
 * Context used by the expat start-element callback to locate the
 * first child element of the SOAP <s:Body>, which is the method name.
 */
typedef struct
{
  char method[64];   /* Local name of the first Body child */
  bool in_body;      /* True once we have entered <s:Body> */
  bool found;        /* True once method[] has been filled */
  XML_Parser parser; /* Back-reference for early stop */
} soap_parse_ctx_t;

static void XMLCALL soap_start_element(void *user_data, const XML_Char *name,
                                       const XML_Char **attrs)
{
  soap_parse_ctx_t *ctx = (soap_parse_ctx_t *)user_data;
  if (ctx->found)
  {
    return;
  }

  /*
   * Expat passes element names as "prefix:local" with XML_ParserCreate.
   * Strip the namespace prefix to get the local name.
   */
  const char *local = strrchr(name, ':');
  local = local ? local + 1 : name;

  if (!ctx->in_body)
  {
    if (strcmp(local, "Body") == 0)
    {
      ctx->in_body = true;
    }
    return;
  }

  /* First child element of Body is the SOAP method */
  snprintf(ctx->method, sizeof(ctx->method), "%s", local);
  ctx->found = true;

  /* Stop the parse early — we have everything we need */
  XML_StopParser(ctx->parser, XML_FALSE);
}

/*
 * Parse a SOAP envelope with expat and write the SOAP method name (the local
 * name of the first child element of <s:Body>) into method_out.
 *
 * Returns true on success, false if parsing fails or no Body child is found.
 * Thread-safe: all state is on the caller's stack.
 */
static bool extract_soap_method(const char *xml_body, int xml_len,
                                char *method_out, int method_out_size)
{
  soap_parse_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));

  XML_Parser parser = XML_ParserCreate(NULL);
  if (!parser)
  {
    ESP_LOGE(TAG, "XML_ParserCreate failed");
    return false;
  }

  ctx.parser = parser;
  XML_SetUserData(parser, &ctx);
  XML_SetStartElementHandler(parser, soap_start_element);

  /*
   * XML_Parse returns XML_STATUS_ERROR when XML_StopParser is called
   * from within a handler (expected behaviour for early stop).
   * Only log a warning for genuine parse errors.
   */
  enum XML_Status status = XML_Parse(parser, xml_body, xml_len, XML_TRUE);
  if (status == XML_STATUS_ERROR && !ctx.found)
  {
    ESP_LOGW(TAG, "XML parse error at line %lu: %s",
             (unsigned long)XML_GetCurrentLineNumber(parser),
             XML_ErrorString(XML_GetErrorCode(parser)));
  }

  XML_ParserFree(parser);

  if (!ctx.found)
  {
    return false;
  }
  snprintf(method_out, method_out_size, "%s", ctx.method);
  return true;
}

/* ---- HTTP / SOAP helpers ------------------------------------------------ */

/*
 * Read the complete SOAP request body from an httpd_req_t into a
 * PSRAM-allocated buffer.  The caller must heap_caps_free(*body_out).
 *
 * Returns ESP_OK on success.  On error the buffer is freed internally
 * and *body_out is set to NULL.
 */
static esp_err_t read_soap_body(httpd_req_t *req, char **body_out, int *len_out)
{
  int content_len = req->content_len;
  if (content_len <= 0 || content_len > ONVIF_HTTP_REQ_SIZE)
  {
    ESP_LOGW(TAG, "Bad Content-Length: %d", content_len);
    return ESP_ERR_INVALID_SIZE;
  }

  char *body = heap_caps_malloc(content_len + 1, MALLOC_CAP_SPIRAM);
  if (!body)
  {
    ESP_LOGE(TAG, "read_soap_body: PSRAM alloc failed (%d bytes)", content_len);
    return ESP_ERR_NO_MEM;
  }

  int received = 0;
  while (received < content_len)
  {
    int r = httpd_req_recv(req, body + received, content_len - received);
    if (r <= 0)
    {
      if (r == HTTPD_SOCK_ERR_TIMEOUT)
      {
        continue; /* Retry on transient timeout */
      }
      ESP_LOGW(TAG, "httpd_req_recv failed: %d", r);
      heap_caps_free(body);
      return ESP_FAIL;
    }
    received += r;
  }
  body[received] = '\0';

  *body_out = body;
  *len_out = received;
  return ESP_OK;
}

/*
 * Send a SOAP 200 OK response whose body is a complete SOAP envelope
 * wrapping soap_body.  Uses chunked transfer encoding via esp_http_server
 * to avoid a large contiguous allocation.
 */
static esp_err_t send_soap_ok(httpd_req_t *req, const char *soap_body)
{
  static const char SOAP_OPEN[] =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
      "<s:Envelope" SOAP_NS "><s:Body>";
  static const char SOAP_CLOSE[] = "</s:Body></s:Envelope>";

  httpd_resp_set_type(req, "application/soap+xml; charset=utf-8");
  httpd_resp_send_chunk(req, SOAP_OPEN, HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, soap_body, HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, SOAP_CLOSE, HTTPD_RESP_USE_STRLEN);
  return httpd_resp_send_chunk(req, NULL, 0); /* terminate chunked response */
}

/* ---- SOAP response builders --------------------------------------------- */

static esp_err_t handle_GetSystemDateAndTime(httpd_req_t *req)
{
  return send_soap_ok(req,
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

static esp_err_t handle_GetDeviceInformation(httpd_req_t *req)
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
  return send_soap_ok(req, body);
}

static esp_err_t handle_GetCapabilities(httpd_req_t *req)
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
  return send_soap_ok(req, body);
}

static esp_err_t handle_GetServices(httpd_req_t *req)
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
  return send_soap_ok(req, body);
}

/* Device service capabilities */
static esp_err_t handle_GetServiceCapabilities_device(httpd_req_t *req)
{
  return send_soap_ok(req,
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
static esp_err_t handle_GetServiceCapabilities_media(httpd_req_t *req)
{
  return send_soap_ok(req,
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

static esp_err_t handle_GetProfiles(httpd_req_t *req)
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
           CONFIG_RTSP_H264_BITRATE / 1000, /* kbps */
           CONFIG_RTSP_H264_I_PERIOD);
  return send_soap_ok(req, body);
}

static esp_err_t handle_GetStreamUri(httpd_req_t *req)
{
  char local_ip[32];
  get_local_ip(local_ip, sizeof(local_ip));

  char body[256];
  snprintf(body, sizeof(body),
           "<tr2:GetStreamUriResponse>"
           "<tr2:Uri>rtsp://%s:%d/stream</tr2:Uri>"
           "</tr2:GetStreamUriResponse>",
           local_ip, CONFIG_ETH_RTSP_PORT);
  return send_soap_ok(req, body);
}

static esp_err_t handle_GetVideoSources(httpd_req_t *req)
{
  return send_soap_ok(req,
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

static esp_err_t handle_GetVideoSourceConfigurations(httpd_req_t *req)
{
  return send_soap_ok(req,
                      "<tr2:GetVideoSourceConfigurationsResponse>"
                      "<tr2:Configurations token=\"vs_config\">"
                      "<tt:Name>VideoSourceConfig</tt:Name>"
                      "<tt:UseCount>1</tt:UseCount>"
                      "<tt:SourceToken>video_source</tt:SourceToken>"
                      "<tt:Bounds x=\"0\" y=\"0\" width=\"1920\" height=\"1080\"/>"
                      "</tr2:Configurations>"
                      "</tr2:GetVideoSourceConfigurationsResponse>");
}

static esp_err_t handle_GetVideoEncoderConfigurations(httpd_req_t *req)
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
  return send_soap_ok(req, body);
}

static esp_err_t handle_GetVideoEncoderConfigurationOptions(httpd_req_t *req)
{
  return send_soap_ok(req,
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
static esp_err_t send_soap_fault(httpd_req_t *req, const char *action)
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
  return send_soap_ok(req, body);
}

/* ---- SOAP method dispatcher --------------------------------------------- */

/*
 * Route a parsed SOAP method name to the appropriate handler.
 * is_media_service is true when the request arrived on /onvif/media_service.
 */
static esp_err_t dispatch_soap_method(httpd_req_t *req, const char *method,
                                      bool is_media_service)
{
  if (strcmp(method, "GetSystemDateAndTime") == 0)
  {
    return handle_GetSystemDateAndTime(req);
  }
  if (strcmp(method, "GetDeviceInformation") == 0)
  {
    return handle_GetDeviceInformation(req);
  }
  if (strcmp(method, "GetCapabilities") == 0)
  {
    return handle_GetCapabilities(req);
  }
  if (strcmp(method, "GetServices") == 0)
  {
    return handle_GetServices(req);
  }
  if (strcmp(method, "GetServiceCapabilities") == 0)
  {
    return is_media_service
               ? handle_GetServiceCapabilities_media(req)
               : handle_GetServiceCapabilities_device(req);
  }
  if (strcmp(method, "GetProfiles") == 0)
  {
    return handle_GetProfiles(req);
  }
  if (strcmp(method, "GetStreamUri") == 0)
  {
    return handle_GetStreamUri(req);
  }
  if (strcmp(method, "GetVideoSources") == 0)
  {
    return handle_GetVideoSources(req);
  }
  if (strcmp(method, "GetVideoSourceConfigurations") == 0)
  {
    return handle_GetVideoSourceConfigurations(req);
  }
  if (strcmp(method, "GetVideoEncoderConfigurations") == 0)
  {
    return handle_GetVideoEncoderConfigurations(req);
  }
  if (strcmp(method, "GetVideoEncoderConfigurationOptions") == 0)
  {
    return handle_GetVideoEncoderConfigurationOptions(req);
  }

  ESP_LOGW(TAG, "Unhandled SOAP method: %s", method);
  return send_soap_fault(req, method);
}

/* ---- esp_http_server URI handlers --------------------------------------- */

/*
 * Common handler body: read SOAP request, extract method with expat, dispatch.
 */
static esp_err_t onvif_post_handler(httpd_req_t *req, bool is_media_service)
{
  char *body = NULL;
  int body_len = 0;

  esp_err_t err = read_soap_body(req, &body, &body_len);
  if (err != ESP_OK)
  {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "Cannot read SOAP body");
    return err;
  }

  char soap_method[64];
  if (!extract_soap_method(body, body_len, soap_method, sizeof(soap_method)))
  {
    heap_caps_free(body);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "Cannot parse SOAP method");
    return ESP_FAIL;
  }
  heap_caps_free(body);

  ESP_LOGD(TAG, "%s method: %s",
           is_media_service ? "Media2" : "Device", soap_method);
  return dispatch_soap_method(req, soap_method, is_media_service);
}

static esp_err_t device_service_handler(httpd_req_t *req)
{
  return onvif_post_handler(req, false);
}

static esp_err_t media_service_handler(httpd_req_t *req)
{
  return onvif_post_handler(req, true);
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
  if (!p)
  {
    return false;
  }
  p = strchr(p, '>');
  if (!p)
  {
    return false;
  }
  p++;

  const char *end = strchr(p, '<');
  if (!end)
  {
    return false;
  }

  int len = (int)(end - p);
  if (len <= 0 || len >= id_bufsize)
  {
    return false;
  }

  memcpy(id_buf, p, len);
  id_buf[len] = '\0';
  return true;
}

/* ---- WS-Discovery task -------------------------------------------------- */

static void discovery_task(void *arg)
{
  if (!wait_for_ip(30000))
  {
    ESP_LOGW(TAG, "WS-Discovery: no IP after 30 s, aborting");
    vTaskDelete(NULL);
    return;
  }

  char local_ip[32];
  get_local_ip(local_ip, sizeof(local_ip));

  /* Create UDP socket */
  int disc_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (disc_fd < 0)
  {
    ESP_LOGE(TAG, "Discovery socket failed: errno %d", errno);
    vTaskDelete(NULL);
    return;
  }

  int opt = 1;
  setsockopt(disc_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in bind_addr = {
      .sin_family = AF_INET,
      .sin_addr.s_addr = htonl(INADDR_ANY),
      .sin_port = htons(ONVIF_DISC_PORT),
  };

  if (bind(disc_fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0)
  {
    ESP_LOGE(TAG, "Discovery bind failed: errno %d", errno);
    close(disc_fd);
    vTaskDelete(NULL);
    return;
  }

  /* Join WS-Discovery multicast group on the Ethernet interface */
  esp_netif_t *netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
  if (netif)
  {
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK)
    {
      struct ip_mreq mreq;
      mreq.imr_multiaddr.s_addr = inet_addr(ONVIF_MCAST_ADDR);
      mreq.imr_interface.s_addr = ip_info.ip.addr;
      if (setsockopt(disc_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                     &mreq, sizeof(mreq)) < 0)
      {
        ESP_LOGW(TAG, "Multicast join failed: errno %d (continuing)", errno);
      }
    }
  }

  /* Receive timeout so we can refresh the IP periodically */
  struct timeval tv = {.tv_sec = 2, .tv_usec = 0};
  setsockopt(disc_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  char *buf = heap_caps_malloc(ONVIF_DISC_BUF_SIZE, MALLOC_CAP_SPIRAM);
  if (!buf)
  {
    ESP_LOGE(TAG, "Discovery buffer alloc failed");
    close(disc_fd);
    vTaskDelete(NULL);
    return;
  }

  /* Send Hello on the multicast group so scanners see us immediately */
  struct sockaddr_in mcast_addr = {
      .sin_family = AF_INET,
      .sin_port = htons(ONVIF_DISC_PORT),
  };
  mcast_addr.sin_addr.s_addr = inet_addr(ONVIF_MCAST_ADDR);

  int hello_len = build_hello(buf, ONVIF_DISC_BUF_SIZE, local_ip);
  if (hello_len > 0 && hello_len < ONVIF_DISC_BUF_SIZE)
  {
    sendto(disc_fd, buf, hello_len, 0,
           (struct sockaddr *)&mcast_addr, sizeof(mcast_addr));
    ESP_LOGI(TAG, "WS-Discovery Hello sent (UUID=%s)", s_device_uuid);
  }

  ESP_LOGI(TAG, "WS-Discovery listening on %s:%d", ONVIF_MCAST_ADDR, ONVIF_DISC_PORT);

  while (1)
  {
    struct sockaddr_in sender;
    socklen_t sender_len = sizeof(sender);

    int n = recvfrom(disc_fd, buf, ONVIF_DISC_BUF_SIZE - 1, 0,
                     (struct sockaddr *)&sender, &sender_len);
    if (n <= 0)
    {
      /* Refresh local IP on each timeout in case of DHCP renewal */
      get_local_ip(local_ip, sizeof(local_ip));
      continue;
    }
    buf[n] = '\0';

    /* Only respond to Probe messages (ignore ProbeMatches, Hello, etc.) */
    if (!strstr(buf, "Probe") || strstr(buf, "ProbeMatch"))
    {
      continue;
    }

    /* Extract the probe's MessageID for the RelatesTo header */
    char probe_id[128] = "urn:uuid:unknown";
    extract_message_id(buf, probe_id, sizeof(probe_id));

    get_local_ip(local_ip, sizeof(local_ip));

    int match_len = build_probe_match(buf, ONVIF_DISC_BUF_SIZE,
                                      local_ip, probe_id);
    if (match_len > 0 && match_len < ONVIF_DISC_BUF_SIZE)
    {
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

  /* Start WS-Discovery task (waits for IP internally) */
  BaseType_t ret = xTaskCreate(discovery_task, "onvif_disc",
                               ONVIF_DISC_TASK_STACK, NULL,
                               ONVIF_TASK_PRIO, NULL);
  if (ret != pdPASS)
  {
    ESP_LOGE(TAG, "Discovery task create failed");
    return ESP_ERR_NO_MEM;
  }

  /* Start ONVIF HTTP/SOAP server via esp_http_server */
  httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
  http_config.server_port = ONVIF_HTTP_PORT;
  http_config.stack_size = ONVIF_HTTP_TASK_STACK;
  http_config.lru_purge_enable = true;

  httpd_handle_t http_server = NULL;
  esp_err_t err = httpd_start(&http_server, &http_config);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
    return err;
  }

  static const httpd_uri_t device_uri = {
      .uri = "/onvif/device_service",
      .method = HTTP_POST,
      .handler = device_service_handler,
  };
  httpd_register_uri_handler(http_server, &device_uri);

  static const httpd_uri_t media_uri = {
      .uri = "/onvif/media_service",
      .method = HTTP_POST,
      .handler = media_service_handler,
  };
  httpd_register_uri_handler(http_server, &media_uri);

  ESP_LOGI(TAG, "ONVIF HTTP server started (port %d, esp_http_server + expat)",
           ONVIF_HTTP_PORT);

  return ESP_OK;
}
