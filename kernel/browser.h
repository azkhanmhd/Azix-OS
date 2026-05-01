#pragma once
#include <stdint.h>

/*
 * browser_get — HTTP GET "http://host/path", display stripped HTML.
 * Resolves hostname if needed, opens TCP connection to port 80,
 * sends GET request, receives response, strips HTML tags, word-wraps
 * and paginates output.
 *
 * url  — e.g. "example.com", "example.com/page", "http://example.com/page"
 */
void browser_get(const char *url);
