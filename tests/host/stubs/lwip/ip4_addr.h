#pragma once
#include <arpa/inet.h>
#include <stdint.h>
typedef struct { uint32_t addr; } ip4_addr_t;
#define lwip_ntohl ntohl
int ip4addr_aton(const char *s, ip4_addr_t *addr);
