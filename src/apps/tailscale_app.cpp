#include "tailscale_app.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <SD.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WiFiClient.h>

#include <algorithm>
#include <vector>

#include "../core/ble_manager.h"
#include "../core/board_pins.h"
#include "../core/gateway_client.h"
#include "../core/runtime_config.h"
#include "../core/tailscale_lite_client.h"
#include "../core/wifi_manager.h"
#include "../ui/ui_shell.h"

namespace {

struct RelayTarget {
  String host;
  uint16_t port = 18789;
  String path = "/";
  bool secure = false;
};

struct EnvFileEntry {
  String fullPath;
  String label;
  bool isDirectory = false;
};

struct LiteEnvProfile {
  String authKey;
  String loginServer;
  String nodeIp;
  String privateKey;
  String peerHost;
  uint16_t peerPort = 41641;
  String peerPublicKey;
  String gatewayUrl;
};

bool gSdMountedForTailscale = false;

String boolLabel(bool value) {
  return value ? "Yes" : "No";
}

void markDirty(AppContext &ctx) {
  ctx.configDirty = true;
}

String baseName(const String &path) {
  const int slash = path.lastIndexOf('/');
  if (slash < 0 || slash + 1 >= static_cast<int>(path.length())) {
    return path;
  }
  return path.substring(static_cast<unsigned int>(slash + 1));
}

String parentPath(const String &path) {
  if (path.isEmpty() || path == "/") {
    return "/";
  }
  const int slash = path.lastIndexOf('/');
  if (slash <= 0) {
    return "/";
  }
  return path.substring(0, static_cast<unsigned int>(slash));
}

String buildChildPath(const String &dirPath, const String &name) {
  if (name.startsWith("/")) {
    return name;
  }
  if (dirPath == "/") {
    return "/" + name;
  }
  return dirPath + "/" + name;
}

String trimMiddle(const String &value, size_t maxLength) {
  if (value.length() <= maxLength || maxLength < 6) {
    return value;
  }

  const size_t left = (maxLength - 3) / 2;
  const size_t right = maxLength - 3 - left;
  return value.substring(0, left) + "..." +
         value.substring(value.length() - right);
}

bool parsePortNumber(const String &text, uint16_t &outPort) {
  if (text.isEmpty()) {
    return false;
  }

  const long value = text.toInt();
  if (value < 1 || value > 65535) {
    return false;
  }

  outPort = static_cast<uint16_t>(value);
  return true;
}

bool ensureSdMountedForTailscale(bool forceMount, String *error) {
  if (gSdMountedForTailscale && !forceMount) {
    return true;
  }

  pinMode(boardpins::kTftCs, OUTPUT);
  digitalWrite(boardpins::kTftCs, HIGH);
  pinMode(boardpins::kCc1101Cs, OUTPUT);
  digitalWrite(boardpins::kCc1101Cs, HIGH);
  pinMode(boardpins::kSdCs, OUTPUT);
  digitalWrite(boardpins::kSdCs, HIGH);

  SPIClass *spiBus = &TFT_eSPI::getSPIinstance();
  const bool mounted = SD.begin(boardpins::kSdCs,
                                *spiBus,
                                25000000,
                                "/sd",
                                8,
                                false);
  gSdMountedForTailscale = mounted;
  if (!mounted && error) {
    *error = "SD mount failed";
  }
  return mounted;
}

bool isEnvFileName(const String &nameRaw) {
  String name = nameRaw;
  name.toLowerCase();
  return name == ".env" || name.endsWith(".env");
}

bool listEnvDirectory(const String &path,
                      std::vector<EnvFileEntry> &outEntries,
                      String *error) {
  outEntries.clear();

  File dir = SD.open(path.c_str(), FILE_READ);
  if (!dir || !dir.isDirectory()) {
    if (error) {
      *error = "Directory open failed";
    }
    if (dir) {
      dir.close();
    }
    return false;
  }

  File entry = dir.openNextFile();
  while (entry) {
    const String rawName = String(entry.name());
    if (!rawName.isEmpty()) {
      const bool isDir = entry.isDirectory();
      const String name = baseName(buildChildPath(path, rawName));

      if (isDir || isEnvFileName(name)) {
        EnvFileEntry item;
        item.fullPath = buildChildPath(path, rawName);
        item.isDirectory = isDir;
        item.label = isDir ? "[D] " : "[ENV] ";
        item.label += name;
        outEntries.push_back(item);
      }
    }
    entry.close();
    entry = dir.openNextFile();
  }
  dir.close();

  std::sort(outEntries.begin(),
            outEntries.end(),
            [](const EnvFileEntry &a, const EnvFileEntry &b) {
              if (a.isDirectory != b.isDirectory) {
                return a.isDirectory;
              }
              String lhs = a.fullPath;
              String rhs = b.fullPath;
              lhs.toLowerCase();
              rhs.toLowerCase();
              return lhs < rhs;
            });
  return true;
}

bool selectEnvFileFromSd(AppContext &ctx,
                         String &selectedPathOut,
                         const std::function<void()> &backgroundTick) {
  String err;
  if (!ensureSdMountedForTailscale(false, &err)) {
    ctx.ui->showToast("SD Card",
                      err.isEmpty() ? String("Mount failed") : err,
                      1700,
                      backgroundTick);
    return false;
  }

  String currentPath = "/";
  int selected = 0;

  while (true) {
    std::vector<EnvFileEntry> entries;
    if (!listEnvDirectory(currentPath, entries, &err)) {
      ctx.ui->showToast("Env Select",
                        err.isEmpty() ? String("Read failed") : err,
                        1700,
                        backgroundTick);
      return false;
    }

    std::vector<String> menu;
    if (currentPath != "/") {
      menu.push_back(".. (Up)");
    }
    for (std::vector<EnvFileEntry>::const_iterator it = entries.begin();
         it != entries.end();
         ++it) {
      menu.push_back(it->label);
    }
    menu.push_back("Refresh");
    menu.push_back("Back");

    const String subtitle = "Path: " + trimMiddle(currentPath, 22);
    const int choice = ctx.ui->menuLoop("Select .env",
                                        menu,
                                        selected,
                                        backgroundTick,
                                        "OK Select  BACK Exit",
                                        subtitle);
    if (choice < 0) {
      return false;
    }
    selected = choice;

    int idx = choice;
    if (currentPath != "/") {
      if (idx == 0) {
        currentPath = parentPath(currentPath);
        selected = 0;
        continue;
      }
      idx -= 1;
    }

    const int entryCount = static_cast<int>(entries.size());
    if (idx == entryCount) {
      continue;
    }
    if (idx == entryCount + 1) {
      return false;
    }
    if (idx < 0 || idx >= entryCount) {
      continue;
    }

    const EnvFileEntry picked = entries[static_cast<size_t>(idx)];
    if (picked.isDirectory) {
      currentPath = picked.fullPath;
      selected = 0;
      continue;
    }

    selectedPathOut = picked.fullPath;
    return true;
  }
}

String parseEnvValue(const String &lineIn) {
  String line = lineIn;
  line.trim();
  if (line.startsWith("\"") && line.endsWith("\"") && line.length() >= 2) {
    return line.substring(1, line.length() - 1);
  }
  if (line.startsWith("'") && line.endsWith("'") && line.length() >= 2) {
    return line.substring(1, line.length() - 1);
  }
  return line;
}

bool parseEnvFileForAuth(const String &path,
                         String &authKeyOut,
                         String &loginServerOut,
                         String *error) {
  authKeyOut = "";
  loginServerOut = "";

  File file = SD.open(path.c_str(), FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) {
      file.close();
    }
    if (error) {
      *error = "Failed to open .env";
    }
    return false;
  }

  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.replace("\r", "");
    line.trim();
    if (line.isEmpty() || line.startsWith("#")) {
      continue;
    }
    if (line.startsWith("export ")) {
      line.remove(0, 7);
      line.trim();
    }

    const int eq = line.indexOf('=');
    if (eq <= 0) {
      continue;
    }

    String key = line.substring(0, static_cast<unsigned int>(eq));
    String value = line.substring(static_cast<unsigned int>(eq + 1));
    key.trim();
    value = parseEnvValue(value);

    if (key == "TAILSCALE_AUTH_KEY" ||
        key == "TAILSCALE_AUTHKEY" ||
        key == "TS_AUTHKEY" ||
        key == "tailscale_auth_key" ||
        key == "tailscale_authkey") {
      authKeyOut = value;
    } else if (key == "TAILSCALE_LOGIN_SERVER" ||
               key == "HEADSCALE_URL" ||
               key == "tailscale_login_server" ||
               key == "headscale_url") {
      loginServerOut = value;
    }
  }

  file.close();

  if (authKeyOut.isEmpty()) {
    if (error) {
      *error = "No auth key in .env";
    }
    return false;
  }
  return true;
}

bool parseEnvFileForLite(const String &path,
                         LiteEnvProfile &profileOut,
                         String *error) {
  profileOut = LiteEnvProfile();

  File file = SD.open(path.c_str(), FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) {
      file.close();
    }
    if (error) {
      *error = "Failed to open .env";
    }
    return false;
  }

  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.replace("\r", "");
    line.trim();
    if (line.isEmpty() || line.startsWith("#")) {
      continue;
    }
    if (line.startsWith("export ")) {
      line.remove(0, 7);
      line.trim();
    }

    const int eq = line.indexOf('=');
    if (eq <= 0) {
      continue;
    }

    String key = line.substring(0, static_cast<unsigned int>(eq));
    String value = line.substring(static_cast<unsigned int>(eq + 1));
    key.trim();
    value = parseEnvValue(value);

    if (key == "TAILSCALE_AUTH_KEY" ||
        key == "TAILSCALE_AUTHKEY" ||
        key == "TS_AUTHKEY" ||
        key == "tailscale_auth_key" ||
        key == "tailscale_authkey") {
      profileOut.authKey = value;
    } else if (key == "TAILSCALE_LOGIN_SERVER" ||
               key == "HEADSCALE_URL" ||
               key == "tailscale_login_server" ||
               key == "headscale_url") {
      profileOut.loginServer = value;
    } else if (key == "TAILSCALE_LITE_NODE_IP" ||
        key == "TS_LITE_NODE_IP" ||
        key == "TS_WG_LOCAL_IP" ||
        key == "tailscale_lite_node_ip" ||
        key == "ts_lite_node_ip") {
      profileOut.nodeIp = value;
    } else if (key == "TAILSCALE_LITE_PRIVATE_KEY" ||
               key == "TS_LITE_PRIVATE_KEY" ||
               key == "TS_WG_PRIVATE_KEY" ||
               key == "tailscale_lite_private_key" ||
               key == "ts_lite_private_key") {
      profileOut.privateKey = value;
    } else if (key == "TAILSCALE_LITE_PEER_HOST" ||
               key == "TS_LITE_PEER_HOST" ||
               key == "TS_WG_ENDPOINT" ||
               key == "tailscale_lite_peer_host" ||
               key == "ts_lite_peer_host") {
      profileOut.peerHost = value;
    } else if (key == "TAILSCALE_LITE_PEER_PORT" ||
               key == "TS_LITE_PEER_PORT" ||
               key == "TS_WG_ENDPOINT_PORT" ||
               key == "tailscale_lite_peer_port" ||
               key == "ts_lite_peer_port") {
      uint16_t parsedPort = 0;
      if (parsePortNumber(value, parsedPort)) {
        profileOut.peerPort = parsedPort;
      }
    } else if (key == "TAILSCALE_LITE_PEER_PUBLIC_KEY" ||
               key == "TS_LITE_PEER_PUBLIC_KEY" ||
               key == "TS_WG_PEER_PUBLIC_KEY" ||
               key == "tailscale_lite_peer_public_key" ||
               key == "ts_lite_peer_public_key") {
      profileOut.peerPublicKey = value;
    } else if (key == "OPENCLAW_GATEWAY_URL" ||
               key == "GATEWAY_URL" ||
               key == "openclaw_gateway_url") {
      profileOut.gatewayUrl = value;
    }
  }

  file.close();

  if (profileOut.nodeIp.isEmpty() ||
      profileOut.privateKey.isEmpty() ||
      profileOut.peerHost.isEmpty() ||
      profileOut.peerPublicKey.isEmpty()) {
    if (error) {
      *error = "No lite tunnel profile in .env";
    }
    return false;
  }

  return true;
}

String normalizeApiBasePath(const String &rawPath) {
  String path = rawPath;
  path.trim();
  if (path.isEmpty()) {
    path = "/api/tailscale";
  }
  if (!path.startsWith("/")) {
    path = "/" + path;
  }
  while (path.length() > 1 && path.endsWith("/")) {
    path.remove(path.length() - 1);
  }
  return path;
}

String joinApiPath(const String &basePath, const String &endpoint) {
  String path = normalizeApiBasePath(basePath);
  String suffix = endpoint;
  suffix.trim();

  if (suffix.isEmpty()) {
    return path;
  }
  if (suffix.startsWith("/")) {
    return path + suffix;
  }
  return path + "/" + suffix;
}

bool performRelayApiRequest(const RuntimeConfig &config,
                            const String &endpoint,
                            const String &method,
                            const String &requestBody,
                            int &httpCode,
                            String &responseBody,
                            String &errorOut,
                            String *urlOut = nullptr) {
  if (config.tailscaleRelayApiHost.isEmpty()) {
    errorOut = "Relay API host is empty";
    return false;
  }

  if (WiFi.status() != WL_CONNECTED) {
    errorOut = "Wi-Fi is not connected";
    return false;
  }

  const String path = joinApiPath(config.tailscaleRelayApiBasePath, endpoint);
  const String url = "http://" + config.tailscaleRelayApiHost + ":" +
                     String(config.tailscaleRelayApiPort) + path;
  if (urlOut) {
    *urlOut = url;
  }

  HTTPClient http;
  if (!http.begin(url)) {
    errorOut = "HTTP begin failed";
    return false;
  }

  http.setTimeout(3000);

  int code = -1;
  if (method == "GET") {
    if (!config.tailscaleRelayApiToken.isEmpty()) {
      http.addHeader("X-Relay-Token", config.tailscaleRelayApiToken);
    }
    code = http.GET();
  } else if (method == "POST") {
    http.addHeader("Content-Type", "application/json");
    if (!config.tailscaleRelayApiToken.isEmpty()) {
      http.addHeader("X-Relay-Token", config.tailscaleRelayApiToken);
    }
    code = http.POST(requestBody);
  } else {
    http.end();
    errorOut = "Unsupported HTTP method";
    return false;
  }

  String response;
  if (code > 0) {
    response = http.getString();
  }
  http.end();

  httpCode = code;
  responseBody = response;

  if (code <= 0) {
    errorOut = "HTTP request failed";
    return false;
  }

  return true;
}

void appendWrappedLine(std::vector<String> &lines,
                       const String &line,
                       size_t width = 38) {
  if (line.isEmpty()) {
    lines.push_back(" ");
    return;
  }

  size_t start = 0;
  while (start < line.length()) {
    size_t len = line.length() - start;
    if (len > width) {
      len = width;
    }
    lines.push_back(line.substring(start, start + len));
    start += len;
  }
}

void showRelayApiResponse(AppContext &ctx,
                          const String &title,
                          const String &url,
                          int httpCode,
                          const String &responseBody,
                          const std::function<void()> &backgroundTick) {
  std::vector<String> lines;
  lines.push_back("URL: " + trimMiddle(url, 30));
  lines.push_back("HTTP: " + String(httpCode));
  lines.push_back("Response:");

  if (responseBody.isEmpty()) {
    lines.push_back("(empty)");
  } else {
    const size_t maxChars = 500;
    String body = responseBody;
    if (body.length() > maxChars) {
      body = body.substring(0, maxChars);
      body += "...";
    }

    int cursor = 0;
    while (cursor <= static_cast<int>(body.length())) {
      const int next = body.indexOf('\n', static_cast<unsigned int>(cursor));
      if (next < 0) {
        appendWrappedLine(lines, body.substring(static_cast<unsigned int>(cursor)));
        break;
      }
      appendWrappedLine(lines,
                        body.substring(static_cast<unsigned int>(cursor),
                                       static_cast<unsigned int>(next)));
      cursor = next + 1;
      if (cursor >= static_cast<int>(body.length())) {
        break;
      }
    }
  }

  ctx.ui->showInfo(title, lines, backgroundTick, "OK/BACK Exit");
}

void normalizeTarget(RelayTarget &target) {
  if (target.port == 0) {
    target.port = 18789;
  }
  if (target.path.isEmpty()) {
    target.path = "/";
  }
  if (!target.path.startsWith("/")) {
    target.path = "/" + target.path;
  }
}

bool parseWsUrl(const String &rawUrl, RelayTarget &outTarget) {
  if (rawUrl.isEmpty()) {
    return false;
  }

  RelayTarget parsed;
  String rest;

  if (rawUrl.startsWith("ws://")) {
    parsed.secure = false;
    rest = rawUrl.substring(5);
  } else if (rawUrl.startsWith("wss://")) {
    parsed.secure = true;
    rest = rawUrl.substring(6);
  } else {
    return false;
  }

  const int slash = rest.indexOf('/');
  const String hostPort = slash >= 0 ? rest.substring(0, slash) : rest;
  parsed.path = slash >= 0 ? rest.substring(slash) : "/";

  if (hostPort.isEmpty()) {
    return false;
  }

  parsed.port = parsed.secure ? 443 : 80;

  if (hostPort.startsWith("[")) {
    const int close = hostPort.indexOf(']');
    if (close <= 1) {
      return false;
    }

    parsed.host = hostPort.substring(1, static_cast<unsigned int>(close));
    if (close + 1 < static_cast<int>(hostPort.length()) &&
        hostPort[static_cast<unsigned int>(close + 1)] == ':') {
      const String portText = hostPort.substring(static_cast<unsigned int>(close + 2));
      uint16_t parsedPort = 0;
      if (!parsePortNumber(portText, parsedPort)) {
        return false;
      }
      parsed.port = parsedPort;
    }
  } else {
    const int firstColon = hostPort.indexOf(':');
    const int lastColon = hostPort.lastIndexOf(':');

    if (firstColon > 0 && firstColon == lastColon) {
      parsed.host = hostPort.substring(0, static_cast<unsigned int>(firstColon));
      const String portText = hostPort.substring(static_cast<unsigned int>(firstColon + 1));
      uint16_t parsedPort = 0;
      if (!parsePortNumber(portText, parsedPort)) {
        return false;
      }
      parsed.port = parsedPort;
    } else {
      parsed.host = hostPort;
    }
  }

  if (parsed.host.isEmpty()) {
    return false;
  }

  normalizeTarget(parsed);
  outTarget = parsed;
  return true;
}

String buildRelayUrl(const RelayTarget &targetRaw) {
  RelayTarget target = targetRaw;
  normalizeTarget(target);

  String hostPart = target.host;
  if (hostPart.indexOf(':') >= 0 && !hostPart.startsWith("[")) {
    hostPart = "[" + hostPart + "]";
  }

  String url = target.secure ? "wss://" : "ws://";
  url += hostPart;
  url += ":";
  url += String(target.port);
  url += target.path;

  return url;
}

void applyRelayUrlToConfig(AppContext &ctx,
                           const RelayTarget &target,
                           const std::function<void()> &backgroundTick) {
  if (target.host.isEmpty()) {
    ctx.ui->showToast("Tailscale", "Relay host is empty", 1500, backgroundTick);
    return;
  }

  ctx.config.gatewayUrl = buildRelayUrl(target);
  markDirty(ctx);
  ctx.ui->showToast("Tailscale",
                    "Gateway URL staged",
                    1200,
                    backgroundTick);
}

void probeRelay(AppContext &ctx,
                const RelayTarget &target,
                String &lastProbeResult,
                const std::function<void()> &backgroundTick) {
  if (target.host.isEmpty()) {
    ctx.ui->showToast("Relay Probe", "Relay host is empty", 1500, backgroundTick);
    return;
  }

  if (!ctx.wifi->isConnected()) {
    ctx.ui->showToast("Relay Probe", "Wi-Fi is not connected", 1500, backgroundTick);
    return;
  }

  std::vector<String> lines;
  lines.push_back("Target: " + target.host + ":" + String(target.port));

  IPAddress resolved;
  if (WiFi.hostByName(target.host.c_str(), resolved) != 1) {
    lines.push_back("DNS: failed");
    lines.push_back("TCP: skipped");
    lastProbeResult = "DNS fail";
    ctx.ui->showInfo("Relay Probe", lines, backgroundTick, "OK/BACK Exit");
    return;
  }

  lines.push_back("DNS: " + resolved.toString());

  WiFiClient client;
  client.setTimeout(1500);
  const unsigned long startedAt = millis();
  const bool connected = client.connect(target.host.c_str(), target.port);
  const unsigned long elapsedMs = millis() - startedAt;

  if (connected) {
    lines.push_back("TCP: open");
    lines.push_back("Latency: " + String(elapsedMs) + " ms");
    lastProbeResult = "OK " + String(elapsedMs) + "ms";
    client.stop();
  } else {
    lines.push_back("TCP: closed / timeout");
    lines.push_back("Latency: " + String(elapsedMs) + " ms");
    lastProbeResult = "TCP fail";
  }

  ctx.ui->showInfo("Relay Probe", lines, backgroundTick, "OK/BACK Exit");
}

void runRelayLogin(AppContext &ctx,
                   String &lastLoginResult,
                   const std::function<void()> &backgroundTick) {
  if (ctx.config.tailscaleAuthKey.isEmpty()) {
    ctx.ui->showToast("Tailscale Login",
                      "Auth key is empty",
                      1500,
                      backgroundTick);
    return;
  }

  DynamicJsonDocument req(512);
  req["authKey"] = ctx.config.tailscaleAuthKey;
  if (!ctx.config.tailscaleLoginServer.isEmpty()) {
    req["loginServer"] = ctx.config.tailscaleLoginServer;
  }

  String body;
  serializeJson(req, body);

  int code = -1;
  String response;
  String err;
  String url;
  if (!performRelayApiRequest(ctx.config,
                              "/login",
                              "POST",
                              body,
                              code,
                              response,
                              err,
                              &url)) {
    lastLoginResult = err;
    ctx.ui->showToast("Tailscale Login", err, 1800, backgroundTick);
    return;
  }

  lastLoginResult = (code >= 200 && code < 300) ? "Login OK" : "Login fail";
  showRelayApiResponse(ctx,
                       "Tailscale Login",
                       url,
                       code,
                       response,
                       backgroundTick);
}

void runRelayLoginFromEnvFile(AppContext &ctx,
                              String &lastLoginResult,
                              const std::function<void()> &backgroundTick) {
  String envPath;
  if (!selectEnvFileFromSd(ctx, envPath, backgroundTick)) {
    return;
  }

  String authKey;
  String loginServer;
  String err;
  if (!parseEnvFileForAuth(envPath, authKey, loginServer, &err)) {
    lastLoginResult = err;
    ctx.ui->showToast("Tailscale .env", err, 1800, backgroundTick);
    return;
  }

  ctx.config.tailscaleAuthKey = authKey;
  if (!loginServer.isEmpty()) {
    ctx.config.tailscaleLoginServer = loginServer;
  }
  markDirty(ctx);

  String message = "Auth key loaded";
  if (!loginServer.isEmpty()) {
    message += " + login server";
  }
  ctx.ui->showToast("Tailscale .env", message, 1500, backgroundTick);

  runRelayLogin(ctx, lastLoginResult, backgroundTick);
}

void runRelayLogout(AppContext &ctx,
                    String &lastLoginResult,
                    const std::function<void()> &backgroundTick) {
  int code = -1;
  String response;
  String err;
  String url;
  if (!performRelayApiRequest(ctx.config,
                              "/logout",
                              "POST",
                              "{}",
                              code,
                              response,
                              err,
                              &url)) {
    lastLoginResult = err;
    ctx.ui->showToast("Tailscale Logout", err, 1800, backgroundTick);
    return;
  }

  lastLoginResult = (code >= 200 && code < 300) ? "Logout OK" : "Logout fail";
  showRelayApiResponse(ctx,
                       "Tailscale Logout",
                       url,
                       code,
                       response,
                       backgroundTick);
}

void runRelayStatus(AppContext &ctx,
                    String &lastLoginResult,
                    const std::function<void()> &backgroundTick) {
  int code = -1;
  String response;
  String err;
  String url;
  if (!performRelayApiRequest(ctx.config,
                              "/status",
                              "GET",
                              "",
                              code,
                              response,
                              err,
                              &url)) {
    lastLoginResult = err;
    ctx.ui->showToast("Tailscale Status", err, 1800, backgroundTick);
    return;
  }

  lastLoginResult = (code >= 200 && code < 300) ? "Status OK" : "Status fail";
  showRelayApiResponse(ctx,
                       "Tailscale Status API",
                       url,
                       code,
                       response,
                       backgroundTick);
}

void showTailscaleStatus(AppContext &ctx,
                         const RelayTarget &target,
                         const String &lastProbeResult,
                         const String &lastLoginResult,
                         const std::function<void()> &backgroundTick) {
  const GatewayStatus gatewayStatus = ctx.gateway->status();
  const TailscaleLiteStatus liteStatus =
      ctx.tailscaleLite ? ctx.tailscaleLite->status() : TailscaleLiteStatus();

  std::vector<String> lines;
  lines.push_back("Tailscale mode: Relay API + Lite direct");
  lines.push_back("Wi-Fi Connected: " + boolLabel(ctx.wifi->isConnected()));
  lines.push_back("Wi-Fi SSID: " +
                  (ctx.wifi->ssid().isEmpty() ? String("(empty)") : ctx.wifi->ssid()));
  lines.push_back("Wi-Fi IP: " +
                  (ctx.wifi->ip().isEmpty() ? String("-") : ctx.wifi->ip()));

  if (target.host.isEmpty()) {
    lines.push_back("Relay Target: (not set)");
  } else {
    lines.push_back("Relay Target: " + target.host + ":" + String(target.port));
    lines.push_back("Relay URL: " + buildRelayUrl(target));
  }

  lines.push_back("Gateway URL: " +
                  (ctx.config.gatewayUrl.isEmpty() ? String("(empty)")
                                                   : ctx.config.gatewayUrl));
  lines.push_back("Auth Mode: " + String(gatewayAuthModeName(ctx.config.gatewayAuthMode)));
  lines.push_back("Credential Set: " + boolLabel(hasGatewayCredentials(ctx.config)));
  lines.push_back("Probe: " + lastProbeResult);

  lines.push_back("Login Server: " +
                  (ctx.config.tailscaleLoginServer.isEmpty()
                       ? String("(default tailscale)")
                       : trimMiddle(ctx.config.tailscaleLoginServer, 26)));
  lines.push_back("Auth Key Set: " + boolLabel(!ctx.config.tailscaleAuthKey.isEmpty()));
  lines.push_back("Relay API: " +
                  (ctx.config.tailscaleRelayApiHost.isEmpty()
                       ? String("(not set)")
                       : ctx.config.tailscaleRelayApiHost + ":" +
                             String(ctx.config.tailscaleRelayApiPort)));
  lines.push_back("Relay API Path: " +
                  normalizeApiBasePath(ctx.config.tailscaleRelayApiBasePath));
  lines.push_back("Relay API Token: " +
                  boolLabel(!ctx.config.tailscaleRelayApiToken.isEmpty()));
  lines.push_back("Login API: " + lastLoginResult);
  lines.push_back("Lite Enabled: " + boolLabel(liteStatus.enabled));
  lines.push_back("Lite Tunnel: " + boolLabel(liteStatus.tunnelUp));
  lines.push_back("Lite Node IP: " +
                  (ctx.config.tailscaleLiteNodeIp.isEmpty()
                       ? String("(empty)")
                       : ctx.config.tailscaleLiteNodeIp));
  lines.push_back("Lite Peer: " +
                  (ctx.config.tailscaleLitePeerHost.isEmpty()
                       ? String("(empty)")
                       : ctx.config.tailscaleLitePeerHost + ":" +
                             String(ctx.config.tailscaleLitePeerPort)));
  lines.push_back("Lite Peer Key: " +
                  boolLabel(!ctx.config.tailscaleLitePeerPublicKey.isEmpty()));
  lines.push_back("Lite Error: " +
                  (liteStatus.lastError.isEmpty() ? String("-") : liteStatus.lastError));

  lines.push_back("WS Connected: " + boolLabel(gatewayStatus.wsConnected));
  lines.push_back("Gateway Ready: " + boolLabel(gatewayStatus.gatewayReady));

  if (!gatewayStatus.lastError.isEmpty()) {
    lines.push_back("Last Error: " + gatewayStatus.lastError);
  }

  ctx.ui->showInfo("Tailscale Status", lines, backgroundTick, "OK/BACK Exit");
}

void saveAndApply(AppContext &ctx,
                  const std::function<void()> &backgroundTick) {
  String validateErr;
  if (!validateConfig(ctx.config, &validateErr)) {
    ctx.ui->showToast("Validation", validateErr, 1800, backgroundTick);
    return;
  }

  String saveErr;
  if (!saveConfig(ctx.config, &saveErr)) {
    String message = saveErr.isEmpty() ? String("Failed to save config") : saveErr;
    message += " / previous config kept";
    ctx.ui->showToast("Save Error", message, 1900, backgroundTick);
    return;
  }

  ctx.configDirty = false;

  ctx.wifi->configure(ctx.config);
  ctx.gateway->configure(ctx.config);
  ctx.ble->configure(ctx.config);
  if (ctx.tailscaleLite) {
    ctx.tailscaleLite->configure(ctx.config);
    if (ctx.config.tailscaleLiteEnabled) {
      String liteErr;
      if (!ctx.tailscaleLite->connectNow(&liteErr) && !liteErr.isEmpty()) {
        ctx.ui->showToast("Tailscale Lite", liteErr, 1600, backgroundTick);
      }
    } else {
      ctx.tailscaleLite->disconnectNow();
    }
  }

  if (!ctx.config.gatewayUrl.isEmpty() && hasGatewayCredentials(ctx.config)) {
    ctx.gateway->reconnectNow();
  } else {
    ctx.gateway->disconnectNow();
  }

  if (ctx.config.bleDeviceAddress.isEmpty()) {
    ctx.ble->disconnectNow();
  } else if (ctx.config.bleAutoConnect) {
    String bleErr;
    if (!ctx.ble->connectToDevice(ctx.config.bleDeviceAddress,
                                  ctx.config.bleDeviceName,
                                  &bleErr)) {
      ctx.ui->showToast("BLE", bleErr, 1500, backgroundTick);
    }
  }

  ctx.ui->showToast("Tailscale", "Saved and applied", 1400, backgroundTick);
}

void requestGatewayConnect(AppContext &ctx,
                           const std::function<void()> &backgroundTick) {
  String validateErr;
  if (!validateConfig(ctx.config, &validateErr)) {
    ctx.ui->showToast("Config Error", validateErr, 1800, backgroundTick);
    return;
  }

  if (ctx.config.gatewayUrl.isEmpty()) {
    ctx.ui->showToast("Config Error", "Set gateway URL first", 1600, backgroundTick);
    return;
  }

  ctx.gateway->configure(ctx.config);
  ctx.gateway->connectNow();
  ctx.ui->showToast("Tailscale", "Connect requested", 1200, backgroundTick);
}

void editRelayHost(AppContext &ctx,
                   RelayTarget &target,
                   const std::function<void()> &backgroundTick) {
  String host = target.host;
  if (!ctx.ui->textInput("Relay Host/IP", host, false, backgroundTick)) {
    return;
  }

  host.trim();
  target.host = host;
  ctx.ui->showToast("Tailscale", "Relay host updated", 1200, backgroundTick);
}

void editRelayPort(AppContext &ctx,
                   RelayTarget &target,
                   const std::function<void()> &backgroundTick) {
  String portText = String(target.port);
  if (!ctx.ui->textInput("Relay Port", portText, false, backgroundTick)) {
    return;
  }

  uint16_t parsedPort = 0;
  if (!parsePortNumber(portText, parsedPort)) {
    ctx.ui->showToast("Tailscale", "Port must be 1..65535", 1500, backgroundTick);
    return;
  }

  target.port = parsedPort;
  ctx.ui->showToast("Tailscale", "Relay port updated", 1200, backgroundTick);
}

void editRelayPath(AppContext &ctx,
                   RelayTarget &target,
                   const std::function<void()> &backgroundTick) {
  String path = target.path;
  if (!ctx.ui->textInput("Relay Path", path, false, backgroundTick)) {
    return;
  }

  path.trim();
  target.path = path;
  normalizeTarget(target);
  ctx.ui->showToast("Tailscale", "Relay path updated", 1200, backgroundTick);
}

void editLoginServer(AppContext &ctx,
                     const std::function<void()> &backgroundTick) {
  String value = ctx.config.tailscaleLoginServer;
  if (!ctx.ui->textInput("Login Server URL", value, false, backgroundTick)) {
    return;
  }

  value.trim();
  ctx.config.tailscaleLoginServer = value;
  markDirty(ctx);
  ctx.ui->showToast("Tailscale", "Login server updated", 1200, backgroundTick);
}

void editAuthKey(AppContext &ctx,
                 const std::function<void()> &backgroundTick) {
  String value = ctx.config.tailscaleAuthKey;
  if (!ctx.ui->textInput("Tailscale Auth Key", value, true, backgroundTick)) {
    return;
  }

  value.trim();
  ctx.config.tailscaleAuthKey = value;
  markDirty(ctx);
  ctx.ui->showToast("Tailscale", "Auth key updated", 1200, backgroundTick);
}

void toggleLiteEnabled(AppContext &ctx,
                       const std::function<void()> &backgroundTick) {
  ctx.config.tailscaleLiteEnabled = !ctx.config.tailscaleLiteEnabled;
  markDirty(ctx);
  ctx.ui->showToast("Tailscale Lite",
                    ctx.config.tailscaleLiteEnabled ? "Enabled" : "Disabled",
                    1200,
                    backgroundTick);
}

void editLiteNodeIp(AppContext &ctx,
                    const std::function<void()> &backgroundTick) {
  String value = ctx.config.tailscaleLiteNodeIp;
  if (!ctx.ui->textInput("Lite Node IP", value, false, backgroundTick)) {
    return;
  }

  value.trim();
  ctx.config.tailscaleLiteNodeIp = value;
  markDirty(ctx);
  ctx.ui->showToast("Tailscale Lite", "Node IP updated", 1200, backgroundTick);
}

void editLitePrivateKey(AppContext &ctx,
                        const std::function<void()> &backgroundTick) {
  String value = ctx.config.tailscaleLitePrivateKey;
  if (!ctx.ui->textInput("Lite Private Key", value, true, backgroundTick)) {
    return;
  }

  value.trim();
  ctx.config.tailscaleLitePrivateKey = value;
  markDirty(ctx);
  ctx.ui->showToast("Tailscale Lite", "Private key updated", 1200, backgroundTick);
}

void editLitePeerHost(AppContext &ctx,
                      const std::function<void()> &backgroundTick) {
  String value = ctx.config.tailscaleLitePeerHost;
  if (!ctx.ui->textInput("Lite Peer Host/IP", value, false, backgroundTick)) {
    return;
  }

  value.trim();
  ctx.config.tailscaleLitePeerHost = value;
  markDirty(ctx);
  ctx.ui->showToast("Tailscale Lite", "Peer host updated", 1200, backgroundTick);
}

void editLitePeerPort(AppContext &ctx,
                      const std::function<void()> &backgroundTick) {
  String value = String(ctx.config.tailscaleLitePeerPort);
  if (!ctx.ui->textInput("Lite Peer Port", value, false, backgroundTick)) {
    return;
  }

  uint16_t parsed = 0;
  if (!parsePortNumber(value, parsed)) {
    ctx.ui->showToast("Tailscale Lite", "Port must be 1..65535", 1500, backgroundTick);
    return;
  }

  ctx.config.tailscaleLitePeerPort = parsed;
  markDirty(ctx);
  ctx.ui->showToast("Tailscale Lite", "Peer port updated", 1200, backgroundTick);
}

void editLitePeerPublicKey(AppContext &ctx,
                           const std::function<void()> &backgroundTick) {
  String value = ctx.config.tailscaleLitePeerPublicKey;
  if (!ctx.ui->textInput("Lite Peer Public Key", value, true, backgroundTick)) {
    return;
  }

  value.trim();
  ctx.config.tailscaleLitePeerPublicKey = value;
  markDirty(ctx);
  ctx.ui->showToast("Tailscale Lite", "Peer public key updated", 1200, backgroundTick);
}

void runLiteLoadFromEnvFile(AppContext &ctx,
                            const std::function<void()> &backgroundTick) {
  String envPath;
  if (!selectEnvFileFromSd(ctx, envPath, backgroundTick)) {
    return;
  }

  LiteEnvProfile profile;
  String err;
  if (!parseEnvFileForLite(envPath, profile, &err)) {
    ctx.ui->showToast("Tailscale Lite", err, 1800, backgroundTick);
    return;
  }

  ctx.config.tailscaleLiteEnabled = true;
  ctx.config.tailscaleLiteNodeIp = profile.nodeIp;
  ctx.config.tailscaleLitePrivateKey = profile.privateKey;
  ctx.config.tailscaleLitePeerHost = profile.peerHost;
  ctx.config.tailscaleLitePeerPort = profile.peerPort;
  ctx.config.tailscaleLitePeerPublicKey = profile.peerPublicKey;
  if (!profile.authKey.isEmpty()) {
    ctx.config.tailscaleAuthKey = profile.authKey;
  }
  if (!profile.loginServer.isEmpty()) {
    ctx.config.tailscaleLoginServer = profile.loginServer;
  }
  if (!profile.gatewayUrl.isEmpty()) {
    ctx.config.gatewayUrl = profile.gatewayUrl;
  }
  markDirty(ctx);

  String message = "Lite profile loaded";
  if (!profile.authKey.isEmpty()) {
    message += " + auth key";
  }
  if (!profile.gatewayUrl.isEmpty()) {
    message += " + gateway URL";
  }
  ctx.ui->showToast("Tailscale Lite", message, 1600, backgroundTick);
}

void runLiteConnect(AppContext &ctx,
                    const std::function<void()> &backgroundTick) {
  if (!ctx.tailscaleLite) {
    ctx.ui->showToast("Tailscale Lite", "Lite client unavailable", 1500, backgroundTick);
    return;
  }

  String validateErr;
  if (!validateConfig(ctx.config, &validateErr)) {
    ctx.ui->showToast("Validation", validateErr, 1800, backgroundTick);
    return;
  }

  ctx.tailscaleLite->configure(ctx.config);
  String err;
  if (!ctx.tailscaleLite->connectNow(&err)) {
    ctx.ui->showToast("Tailscale Lite",
                      err.isEmpty() ? String("Connect failed") : err,
                      1800,
                      backgroundTick);
    return;
  }

  ctx.ui->showToast("Tailscale Lite", "Tunnel connected", 1200, backgroundTick);
}

void runLiteDisconnect(AppContext &ctx,
                       const std::function<void()> &backgroundTick) {
  if (!ctx.tailscaleLite) {
    ctx.ui->showToast("Tailscale Lite", "Lite client unavailable", 1500, backgroundTick);
    return;
  }

  ctx.tailscaleLite->disconnectNow();
  ctx.ui->showToast("Tailscale Lite", "Tunnel disconnected", 1200, backgroundTick);
}

void editRelayApiHost(AppContext &ctx,
                      const std::function<void()> &backgroundTick) {
  String value = ctx.config.tailscaleRelayApiHost;
  if (!ctx.ui->textInput("Relay API Host/IP", value, false, backgroundTick)) {
    return;
  }

  value.trim();
  ctx.config.tailscaleRelayApiHost = value;
  markDirty(ctx);
  ctx.ui->showToast("Tailscale", "Relay API host updated", 1200, backgroundTick);
}

void editRelayApiPort(AppContext &ctx,
                      const std::function<void()> &backgroundTick) {
  String value = String(ctx.config.tailscaleRelayApiPort);
  if (!ctx.ui->textInput("Relay API Port", value, false, backgroundTick)) {
    return;
  }

  uint16_t parsed = 0;
  if (!parsePortNumber(value, parsed)) {
    ctx.ui->showToast("Tailscale", "Port must be 1..65535", 1500, backgroundTick);
    return;
  }

  ctx.config.tailscaleRelayApiPort = parsed;
  markDirty(ctx);
  ctx.ui->showToast("Tailscale", "Relay API port updated", 1200, backgroundTick);
}

void editRelayApiBasePath(AppContext &ctx,
                          const std::function<void()> &backgroundTick) {
  String value = normalizeApiBasePath(ctx.config.tailscaleRelayApiBasePath);
  if (!ctx.ui->textInput("Relay API Base Path", value, false, backgroundTick)) {
    return;
  }

  ctx.config.tailscaleRelayApiBasePath = normalizeApiBasePath(value);
  markDirty(ctx);
  ctx.ui->showToast("Tailscale", "Relay API path updated", 1200, backgroundTick);
}

void editRelayApiToken(AppContext &ctx,
                       const std::function<void()> &backgroundTick) {
  String value = ctx.config.tailscaleRelayApiToken;
  if (!ctx.ui->textInput("Relay API Token", value, true, backgroundTick)) {
    return;
  }

  value.trim();
  ctx.config.tailscaleRelayApiToken = value;
  markDirty(ctx);
  ctx.ui->showToast("Tailscale", "Relay API token updated", 1200, backgroundTick);
}

}  // namespace

void runTailscaleApp(AppContext &ctx,
                     const std::function<void()> &backgroundTick) {
  RelayTarget target;
  if (!parseWsUrl(ctx.config.gatewayUrl, target)) {
    target.host = "";
    target.port = 18789;
    target.path = "/";
    target.secure = false;
  }

  if (ctx.config.tailscaleRelayApiPort == 0) {
    ctx.config.tailscaleRelayApiPort = 9080;
  }
  if (ctx.config.tailscaleRelayApiBasePath.isEmpty()) {
    ctx.config.tailscaleRelayApiBasePath = "/api/tailscale";
  }
  if (ctx.config.tailscaleLitePeerPort == 0) {
    ctx.config.tailscaleLitePeerPort = 41641;
  }

  String lastProbeResult = "Not run";
  String lastLoginResult = "Not run";
  int selected = 0;

  while (true) {
    std::vector<String> menu;
    menu.push_back("Status");
    menu.push_back("Relay Host/IP");
    menu.push_back("Relay Port");
    menu.push_back("Relay Path");
    menu.push_back(String("Scheme: ") + (target.secure ? "wss://" : "ws://"));
    menu.push_back("Apply URL to OpenClaw");
    menu.push_back("Probe Relay TCP");
    menu.push_back("Login Server URL");
    menu.push_back("Auth Key");
    menu.push_back("Login from SD .env");
    menu.push_back(String("Lite Enabled: ") +
                   (ctx.config.tailscaleLiteEnabled ? "Yes" : "No"));
    menu.push_back("Lite Node IP");
    menu.push_back("Lite Private Key");
    menu.push_back("Lite Peer Host/IP");
    menu.push_back("Lite Peer Port");
    menu.push_back("Lite Peer Public Key");
    menu.push_back("Lite Load from SD .env");
    menu.push_back("Lite Connect");
    menu.push_back("Lite Disconnect");
    menu.push_back("Relay API Host/IP");
    menu.push_back("Relay API Port");
    menu.push_back("Relay API Base Path");
    menu.push_back("Relay API Token");
    menu.push_back("Relay Login");
    menu.push_back("Relay Logout");
    menu.push_back("Relay Status");
    menu.push_back("Save & Apply");
    menu.push_back("Connect");
    menu.push_back("Disconnect");
    menu.push_back("Back");

    String subtitle;
    if (target.host.isEmpty()) {
      subtitle = "Relay optional (Lite direct)";
    } else {
      subtitle = trimMiddle(target.host, 16) + ":" + String(target.port);
    }
    subtitle += " / Lite:";
    subtitle += (ctx.tailscaleLite && ctx.tailscaleLite->isConnected())
                    ? "UP"
                    : (ctx.config.tailscaleLiteEnabled ? "CFG" : "OFF");
    subtitle += " / API:";
    subtitle += lastLoginResult;

    if (ctx.configDirty) {
      subtitle += " *DIRTY";
    }

    const int choice = ctx.ui->menuLoop("Tailscale",
                                        menu,
                                        selected,
                                        backgroundTick,
                                        "OK Select  BACK Exit",
                                        subtitle);

    if (choice < 0 || choice == 29) {
      return;
    }

    selected = choice;

    if (choice == 0) {
      showTailscaleStatus(ctx, target, lastProbeResult, lastLoginResult, backgroundTick);
      continue;
    }

    if (choice == 1) {
      editRelayHost(ctx, target, backgroundTick);
      continue;
    }

    if (choice == 2) {
      editRelayPort(ctx, target, backgroundTick);
      continue;
    }

    if (choice == 3) {
      editRelayPath(ctx, target, backgroundTick);
      continue;
    }

    if (choice == 4) {
      target.secure = !target.secure;
      ctx.ui->showToast("Tailscale",
                        target.secure ? "Scheme set to wss://" : "Scheme set to ws://",
                        1300,
                        backgroundTick);
      continue;
    }

    if (choice == 5) {
      applyRelayUrlToConfig(ctx, target, backgroundTick);
      continue;
    }

    if (choice == 6) {
      probeRelay(ctx, target, lastProbeResult, backgroundTick);
      continue;
    }

    if (choice == 7) {
      editLoginServer(ctx, backgroundTick);
      continue;
    }

    if (choice == 8) {
      editAuthKey(ctx, backgroundTick);
      continue;
    }

    if (choice == 9) {
      runRelayLoginFromEnvFile(ctx, lastLoginResult, backgroundTick);
      continue;
    }

    if (choice == 10) {
      toggleLiteEnabled(ctx, backgroundTick);
      continue;
    }

    if (choice == 11) {
      editLiteNodeIp(ctx, backgroundTick);
      continue;
    }

    if (choice == 12) {
      editLitePrivateKey(ctx, backgroundTick);
      continue;
    }

    if (choice == 13) {
      editLitePeerHost(ctx, backgroundTick);
      continue;
    }

    if (choice == 14) {
      editLitePeerPort(ctx, backgroundTick);
      continue;
    }

    if (choice == 15) {
      editLitePeerPublicKey(ctx, backgroundTick);
      continue;
    }

    if (choice == 16) {
      runLiteLoadFromEnvFile(ctx, backgroundTick);
      continue;
    }

    if (choice == 17) {
      runLiteConnect(ctx, backgroundTick);
      continue;
    }

    if (choice == 18) {
      runLiteDisconnect(ctx, backgroundTick);
      continue;
    }

    if (choice == 19) {
      editRelayApiHost(ctx, backgroundTick);
      continue;
    }

    if (choice == 20) {
      editRelayApiPort(ctx, backgroundTick);
      continue;
    }

    if (choice == 21) {
      editRelayApiBasePath(ctx, backgroundTick);
      continue;
    }

    if (choice == 22) {
      editRelayApiToken(ctx, backgroundTick);
      continue;
    }

    if (choice == 23) {
      runRelayLogin(ctx, lastLoginResult, backgroundTick);
      continue;
    }

    if (choice == 24) {
      runRelayLogout(ctx, lastLoginResult, backgroundTick);
      continue;
    }

    if (choice == 25) {
      runRelayStatus(ctx, lastLoginResult, backgroundTick);
      continue;
    }

    if (choice == 26) {
      saveAndApply(ctx, backgroundTick);
      continue;
    }

    if (choice == 27) {
      requestGatewayConnect(ctx, backgroundTick);
      continue;
    }

    if (choice == 28) {
      ctx.gateway->disconnectNow();
      ctx.ui->showToast("Tailscale", "Disconnected", 1200, backgroundTick);
      continue;
    }
  }
}
