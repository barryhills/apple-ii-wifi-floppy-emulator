#pragma once

// Minimal HTTP/1.0 server on lwIP's raw TCP API, port 80.
// Call once with the lwIP lock held (cyw43_arch_lwip_begin/end).
void http_init(void);
