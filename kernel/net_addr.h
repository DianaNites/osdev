// ------------------------------------------------------------------------------------------------
// net_addr.h
// ------------------------------------------------------------------------------------------------

#pragma once

#include "types.h"

// ------------------------------------------------------------------------------------------------
// Ethernet Address

typedef struct Eth_Addr
{
    u8 n[6];
} PACKED Eth_Addr;

#define ETH_ADDR_STRING_SIZE            18

// ------------------------------------------------------------------------------------------------
// IPv4 Address

typedef struct IPv4_Addr
{
    u8 n[4];
} PACKED IPv4_Addr;

#define IPV4_ADDR_STRING_SIZE           16

// ------------------------------------------------------------------------------------------------
// Functions

void eth_addr_to_str(char* str, size_t size, const Eth_Addr* addr);
void ipv4_addr_to_str(char* str, size_t size, const IPv4_Addr* addr);
