#pragma once

#include <Arduino.h>

struct TokenFetchResult {
  bool ok;
  int statusCode;
  String token;
  String response;
};

struct TokenFetchOptions {
  const char *authorizationHeader;
  const char *cookieHeader;
};

bool tokenServiceFetch(const char *url, const TokenFetchOptions *options, TokenFetchResult *outResult);
bool tokenServiceFetch(const char *url, TokenFetchResult *outResult);
