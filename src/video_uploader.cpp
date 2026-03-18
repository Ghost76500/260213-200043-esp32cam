#include "video_uploader.h"

#include <WiFi.h>

#include "FS.h"
#include "SD_MMC.h"

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

bool videoUploaderUploadAviMultipart(
  const char *url,
  const char *deviceId,
  const char *aviPath,
  const VideoUploadOptions *options,
  VideoUploadResult *outResult
) {
  if (outResult) {
    outResult->ok = false;
    outResult->statusCode = -1;
    outResult->response = "";
  }

  if (!url || !deviceId || !aviPath) {
    if (outResult) {
      outResult->response = "invalid args";
    }
    return false;
  }

  if (!SD_MMC.exists(aviPath)) {
    if (outResult) {
      outResult->response = "file not found";
    }
    return false;
  }

  File file = SD_MMC.open(aviPath, FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) {
      file.close();
    }
    if (outResult) {
      outResult->response = "file open failed";
    }
    return false;
  }

  String host;
  String path;
  uint16_t port = 0;
  if (!parseHttpUrl(String(url), host, port, path)) {
    file.close();
    if (outResult) {
      outResult->response = "invalid http url";
    }
    return false;
  }

  String fileName = String(aviPath);
  int slashPos = fileName.lastIndexOf('/');
  if (slashPos >= 0) {
    fileName = fileName.substring(slashPos + 1);
  }

  const String boundary = "----ESP32CAMBoundary7MA4YWxkTrZu0gW";
  String partId;
  partId.reserve(80 + strlen(deviceId));
  partId += "--" + boundary + "\r\n";
  partId += "Content-Disposition: form-data; name=\"id\"\r\n\r\n";
  partId += String(deviceId) + "\r\n";

  String partFile;
  partFile.reserve(180 + fileName.length());
  partFile += "--" + boundary + "\r\n";
  partFile += "Content-Disposition: form-data; name=\"video\"; filename=\"" + fileName + "\"\r\n";
  partFile += "Content-Type: video/x-msvideo\r\n\r\n";

  String tail = "\r\n--" + boundary + "--\r\n";

  size_t contentLength = partId.length() + partFile.length() + file.size() + tail.length();

  WiFiClient client;
  client.setTimeout(15000);
  if (!client.connect(host.c_str(), port)) {
    file.close();
    if (outResult) {
      outResult->response = String("connect failed: ") + host + ":" + String(port);
    }
    return false;
  }

  String hostHeader = port == 80 ? host : host + ":" + String(port);
  client.print("POST " + path + " HTTP/1.1\r\n");
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
  client.print("Accept: */*\r\n");
  client.print("Content-Type: multipart/form-data; boundary=" + boundary + "\r\n");
  client.print("Content-Length: " + String((unsigned long)contentLength) + "\r\n\r\n");

  client.print(partId);
  client.print(partFile);

  uint8_t buf[1024];
  while (file.available()) {
    size_t n = file.read(buf, sizeof(buf));
    if (n == 0) {
      break;
    }
    size_t written = client.write(buf, n);
    if (written != n) {
      file.close();
      client.stop();
      if (outResult) {
        outResult->response = "socket write failed";
      }
      return false;
    }
  }

  client.print(tail);
  file.close();

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

  if (outResult) {
    outResult->statusCode = statusCode;
    outResult->response = body;
    outResult->ok = (statusCode >= 200 && statusCode < 300);
  }

  return statusCode >= 200 && statusCode < 300;
}

bool videoUploaderUploadAviMultipart(const char *url, const char *deviceId, const char *aviPath, VideoUploadResult *outResult) {
  return videoUploaderUploadAviMultipart(url, deviceId, aviPath, NULL, outResult);
}
