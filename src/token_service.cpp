#include "token_service.h"

#include <WiFi.h>

static bool parseHttpUrl(const String &url, String &host, uint16_t &port, String &path) {
  const String prefix("http://");
  if (!url.startsWith(prefix)) {
    return false;
  }

  String rest = url.substring(prefix.length());
  int slashPos = rest.indexOf('/');
  String hostPort = slashPos >= 0 ? rest.substring(0, slashPos) : rest;
  path = slashPos >= 0 ? rest.substring(slashPos) : String("/");

  int colonPos = hostPort.indexOf(':');
  if (colonPos >= 0) {
    host = hostPort.substring(0, colonPos);
    port = (uint16_t)hostPort.substring(colonPos + 1).toInt();
    if (port == 0) {
      return false;
    }
  } else {
    host = hostPort;
    port = 80;
  }

  return host.length() > 0;
}

static int parseHttpStatusCode(const String &httpResp) {
  int lineEnd = httpResp.indexOf("\r\n");
  if (lineEnd < 0) {
    return -1;
  }
  String statusLine = httpResp.substring(0, lineEnd);
  int sp1 = statusLine.indexOf(' ');
  if (sp1 < 0) {
    return -1;
  }
  int sp2 = statusLine.indexOf(' ', sp1 + 1);
  if (sp2 < 0) {
    sp2 = statusLine.length();
  }
  return statusLine.substring(sp1 + 1, sp2).toInt();
}

static String parseHttpBody(const String &httpResp) {
  int bodyPos = httpResp.indexOf("\r\n\r\n");
  if (bodyPos < 0) {
    return String();
  }
  return httpResp.substring(bodyPos + 4);
}

static String trimTokenText(String s) {
  s.trim();
  if (s.length() >= 2 && s.startsWith("\"") && s.endsWith("\"")) {
    s = s.substring(1, s.length() - 1);
    s.trim();
  }
  return s;
}

static String extractJsonStringValue(const String &json, const char *key) {
  String pattern = String("\"") + key + "\"";
  int keyPos = json.indexOf(pattern);
  if (keyPos < 0) {
    return String();
  }

  int colonPos = json.indexOf(':', keyPos + pattern.length());
  if (colonPos < 0) {
    return String();
  }

  int i = colonPos + 1;
  while (i < json.length() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\r' || json[i] == '\n')) {
    ++i;
  }
  if (i >= json.length()) {
    return String();
  }

  if (json[i] == '"') {
    ++i;
    int end = json.indexOf('"', i);
    if (end < 0) {
      return String();
    }
    return json.substring(i, end);
  }

  int end = i;
  while (end < json.length() && json[end] != ',' && json[end] != '}' && json[end] != '\r' && json[end] != '\n') {
    ++end;
  }
  return trimTokenText(json.substring(i, end));
}

static String extractTokenFromBody(const String &body) {
  String text = trimTokenText(body);
  if (text.length() == 0) {
    return String();
  }

  if (text.startsWith("{")) {
    const char *keys[] = {"token", "access_token", "data", "result"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
      String value = extractJsonStringValue(text, keys[i]);
      if (value.length() > 0 && value != "null") {
        return trimTokenText(value);
      }
    }
    return String();
  }

  // 非 JSON 时按纯文本 token 处理
  return text;
}

bool tokenServiceFetch(const char *url, const TokenFetchOptions *options, TokenFetchResult *outResult) {
  if (outResult) {
    outResult->ok = false;
    outResult->statusCode = -1;
    outResult->token = "";
    outResult->response = "";
  }

  if (!url) {
    if (outResult) {
      outResult->response = "invalid url";
    }
    return false;
  }

  String host;
  String path;
  uint16_t port = 0;
  if (!parseHttpUrl(String(url), host, port, path)) {
    if (outResult) {
      outResult->response = "invalid http url";
    }
    return false;
  }

  WiFiClient client;
  client.setTimeout(12000);
  if (!client.connect(host.c_str(), port)) {
    if (outResult) {
      outResult->response = String("connect failed: ") + host + ":" + String(port);
    }
    return false;
  }

  String hostHeader = port == 80 ? host : host + ":" + String(port);
  client.print("GET " + path + " HTTP/1.1\r\n");
  client.print("Host: " + hostHeader + "\r\n");
  client.print("Connection: close\r\n");
  client.print("User-Agent: esp32-cam\r\n");
  if (options && options->authorizationHeader && strlen(options->authorizationHeader) > 0) {
    client.print("Authorization: ");
    client.print(options->authorizationHeader);
    client.print("\r\n");
  }
  if (options && options->cookieHeader && strlen(options->cookieHeader) > 0) {
    client.print("Cookie: ");
    client.print(options->cookieHeader);
    client.print("\r\n");
  }
  client.print("Accept: application/json,text/plain,*/*\r\n\r\n");

  String rawResp;
  rawResp.reserve(1024);
  unsigned long startMs = millis();
  while (client.connected() || client.available()) {
    while (client.available()) {
      char c = (char)client.read();
      if (rawResp.length() < 4096) {
        rawResp += c;
      }
    }
    if (millis() - startMs > 20000UL) {
      break;
    }
    delay(2);
  }
  client.stop();

  int statusCode = parseHttpStatusCode(rawResp);
  String body = parseHttpBody(rawResp);
  String token = extractTokenFromBody(body);

  if (outResult) {
    outResult->statusCode = statusCode;
    outResult->response = body;
    outResult->token = token;
    outResult->ok = (statusCode >= 200 && statusCode < 300 && token.length() > 0);
  }

  return (statusCode >= 200 && statusCode < 300 && token.length() > 0);
}

bool tokenServiceFetch(const char *url, TokenFetchResult *outResult) {
  return tokenServiceFetch(url, NULL, outResult);
}
