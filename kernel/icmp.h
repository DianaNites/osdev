// ------------------------------------------------------------------------------------------------
// icmp.h
// ------------------------------------------------------------------------------------------------

#pragma once

#include "net_intf.h"

void icmp_rx(Net_Intf* intf, const u8* pkt, uint len);
