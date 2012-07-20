// ------------------------------------------------------------------------------------------------
// net/tcp_test.c
// ------------------------------------------------------------------------------------------------

#include "test/test.h"
#include "net/checksum.h"
#include "net/ipv4.h"
#include "net/loopback.h"
#include "net/route.h"
#include "net/swap.h"
#include "net/tcp.h"
#include "stdlib/string.h"
#include "time/time.h"

#include <stdarg.h>
#include <stdio.h>

static Net_Intf* intf;
static IPv4_Addr ip_addr = { { { 127, 0, 0, 1 } } };
static IPv4_Addr subnet_mask = { { { 255, 255, 255, 255 } } };

// ------------------------------------------------------------------------------------------------
// Packets

typedef struct Packet
{
    Link link;
    Checksum_Header phdr;
    u8 data[1500];
    u8* end;
} Packet;

// ------------------------------------------------------------------------------------------------
// Mocked dependencies

u8 net_trace;
u32 pit_ticks;
Link out_packets = { &out_packets, &out_packets };

void rtc_get_time(DateTime* dt)
{
    split_time(dt, 0, 0);
}

void console_print(const char* fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

static void validate_checksum(Packet* pkt);

void ipv4_tx_intf(Net_Intf* intf, const IPv4_Addr* next_addr,
    const IPv4_Addr* dst_addr, u8 protocol, Net_Buf* pkt)
{
    uint len = pkt->end - pkt->start;

    Packet* packet = malloc(sizeof(Packet));
    packet->phdr.src = intf->ip_addr;
    packet->phdr.dst = *dst_addr;
    packet->phdr.reserved = 0;
    packet->phdr.protocol = protocol;
    packet->phdr.len = net_swap16(len);

    memcpy(packet->data, pkt->start, len);
    packet->end = packet->data + len;

    link_before(&out_packets, &packet->link);
}

void* vm_alloc(uint size)
{
    return malloc(size);
}

// ------------------------------------------------------------------------------------------------
static void tcp_input(Net_Buf* pkt)
{
    TCP_Header* tcp_hdr = (TCP_Header*)pkt->start;
    tcp_swap(tcp_hdr);

    // Data
    pkt->end = pkt->start + sizeof(TCP_Header);

    // Pseudo Header
    Checksum_Header* phdr = (Checksum_Header*)(pkt->start - sizeof(Checksum_Header));
    phdr->src = ip_addr;
    phdr->dst = ip_addr;
    phdr->reserved = 0;
    phdr->protocol = IP_PROTOCOL_TCP;
    phdr->len = net_swap16(pkt->end - pkt->start);

    // Checksum
    u16 checksum = net_checksum(pkt->start - sizeof(Checksum_Header), pkt->end);
    tcp_hdr->checksum = net_swap16(checksum);

    // IP Header
    IPv4_Header* ip_hdr = (IPv4_Header*)(pkt->start - sizeof(IPv4_Header));
    ip_hdr->ver_ihl = (4 << 4) | 5;
    ip_hdr->tos = 0;
    ip_hdr->len = net_swap16(pkt->end - pkt->start);
    ip_hdr->id = net_swap16(0);
    ip_hdr->offset = net_swap16(0);
    ip_hdr->ttl = 64;
    ip_hdr->protocol = IP_PROTOCOL_TCP;
    ip_hdr->checksum = 0;
    ip_hdr->src = ip_addr;
    ip_hdr->dst = ip_addr;

    // Receive
    tcp_rx(intf, ip_hdr, pkt);

    net_free_buf(pkt);
}

// ------------------------------------------------------------------------------------------------
static void validate_checksum(Packet* pkt)
{
    u8* phdr_data = (u8*)&pkt->phdr;
    u8* phdr_end = phdr_data + sizeof(Checksum_Header);

    uint sum = 0;
    sum = net_checksum_acc(phdr_data, phdr_end, sum);
    sum = net_checksum_acc(pkt->data, pkt->end, sum);
    u16 checksum = net_checksum_final(sum);

    ASSERT_EQ_UINT(checksum, 0);
}

// ------------------------------------------------------------------------------------------------
static Packet* pop_packet()
{
    ASSERT_TRUE(!list_empty(&out_packets));

    Packet* packet = link_data(out_packets.next, Packet, link);
    link_remove(&packet->link);

    validate_checksum(packet);
    return packet;
}

// ------------------------------------------------------------------------------------------------
static void set_in_hdr(TCP_Conn* conn, TCP_Header* hdr)
{
    hdr->src_port = conn->remote_port;
    hdr->dst_port = conn->local_port;
    hdr->seq = 0;
    hdr->ack = 0;
    hdr->off = 5 << 4;
    hdr->flags = 0;
    hdr->window_size = TCP_WINDOW_SIZE;
    hdr->checksum = 0;
    hdr->urgent = 0;
}

// ------------------------------------------------------------------------------------------------
static void test_case_begin(const char* msg)
{
    printf("-- %s\n", msg);
}

// ------------------------------------------------------------------------------------------------
static void test_case_end()
{
    ASSERT_TRUE(list_empty(&out_packets));
    ASSERT_TRUE(list_empty(&tcp_active_conns));
}

// ------------------------------------------------------------------------------------------------
static void test_setup()
{
    // Create net interface
    intf = net_intf_create();
    intf->eth_addr = null_eth_addr;
    intf->ip_addr = ip_addr;
    intf->name = "test";
    intf->poll = 0;
    intf->tx = 0;
    intf->dev_tx = 0;

    //net_intf_add(intf);

    // Add routing entry
    net_add_route(&ip_addr, &subnet_mask, 0, intf);
}

// ------------------------------------------------------------------------------------------------
static void enter_state(TCP_Conn* conn, uint state)
{
    Packet* out_pkt;
    TCP_Header* out_hdr;

    switch (state)
    {
    case TCP_SYN_SENT:
        ASSERT_TRUE(tcp_connect(conn, &ip_addr, 80));

        out_pkt = pop_packet();
        out_hdr = (TCP_Header*)out_pkt->data;
        tcp_swap(out_hdr);
        ASSERT_TRUE(out_hdr->src_port >= 49152);
        ASSERT_EQ_UINT(out_hdr->dst_port, 80);
        ASSERT_EQ_UINT(out_hdr->seq, conn->iss);
        ASSERT_EQ_UINT(out_hdr->ack, 0);
        ASSERT_EQ_HEX8(out_hdr->flags, TCP_SYN);
        ASSERT_EQ_UINT(out_hdr->window_size, TCP_WINDOW_SIZE);
        ASSERT_EQ_UINT(out_hdr->urgent, 0);
        free(out_pkt);
        break;
    }
}

// ------------------------------------------------------------------------------------------------
static void exit_state(TCP_Conn* conn, uint state)
{
    Net_Buf* in_pkt;
    TCP_Header* in_hdr;
    Packet* out_pkt;
    TCP_Header* out_hdr;

    ASSERT_EQ_UINT(conn->state, state);
    ASSERT_TRUE(list_empty(&out_packets));

    switch (state)
    {
    case TCP_CLOSED:
        tcp_close(conn);
        break;

    case TCP_SYN_SENT:
        tcp_close(conn);
        break;

    case TCP_SYN_RECEIVED:
        in_pkt = net_alloc_buf();
        in_hdr = (TCP_Header*)in_pkt->start;
        set_in_hdr(conn, in_hdr);
        in_hdr->seq = conn->rcv_nxt;
        in_hdr->ack = conn->snd_nxt;
        in_hdr->flags = TCP_ACK;
        tcp_input(in_pkt);

        exit_state(conn, TCP_ESTABLISHED);
        break;

    case TCP_ESTABLISHED:
        in_pkt = net_alloc_buf();
        in_hdr = (TCP_Header*)in_pkt->start;
        set_in_hdr(conn, in_hdr);
        in_hdr->seq = conn->rcv_nxt;
        in_hdr->ack = conn->snd_nxt;
        in_hdr->flags = TCP_FIN | TCP_ACK;
        tcp_input(in_pkt);

        out_pkt = pop_packet();
        out_hdr = (TCP_Header*)out_pkt->data;
        tcp_swap(out_hdr);
        ASSERT_EQ_UINT(out_hdr->src_port, conn->local_port);
        ASSERT_EQ_UINT(out_hdr->dst_port, conn->remote_port);
        ASSERT_EQ_UINT(out_hdr->seq, conn->snd_nxt);
        ASSERT_EQ_UINT(out_hdr->ack, conn->rcv_nxt);
        ASSERT_EQ_HEX8(out_hdr->flags, TCP_ACK);
        free(out_pkt);

        exit_state(conn, TCP_CLOSE_WAIT);
        break;

    case TCP_CLOSE_WAIT:
        tcp_close(conn);

        out_pkt = pop_packet();
        out_hdr = (TCP_Header*)out_pkt->data;
        tcp_swap(out_hdr);
        ASSERT_EQ_UINT(out_hdr->src_port, conn->local_port);
        ASSERT_EQ_UINT(out_hdr->dst_port, conn->remote_port);
        ASSERT_EQ_UINT(out_hdr->seq, conn->snd_nxt - 1);
        ASSERT_EQ_UINT(out_hdr->ack, conn->rcv_nxt);
        ASSERT_EQ_HEX8(out_hdr->flags, TCP_FIN | TCP_ACK);
        free(out_pkt);

        exit_state(conn, TCP_LAST_ACK);
        break;

    case TCP_LAST_ACK:
        in_pkt = net_alloc_buf();
        in_hdr = (TCP_Header*)in_pkt->start;
        set_in_hdr(conn, in_hdr);
        in_hdr->seq = conn->rcv_nxt;
        in_hdr->ack = conn->snd_nxt;
        in_hdr->flags = TCP_ACK;
        tcp_input(in_pkt);
        break;

    default:
        ASSERT_TRUE(0);
        break;
    }
}

// ------------------------------------------------------------------------------------------------
int main(int argc, const char** argv)
{
    // Common variables
    Net_Buf* in_pkt;
    TCP_Header* in_hdr;
    Packet* out_pkt;
    TCP_Header* out_hdr;
    TCP_Conn* conn;

    test_setup();

    // --------------------------------------------------------------------------------------------
    test_case_begin("CLOSED: RST - segment dropped");

    in_pkt = net_alloc_buf();
    in_hdr = (TCP_Header*)in_pkt->start;
    in_hdr->src_port = 100;
    in_hdr->dst_port = 101;
    in_hdr->seq = 1;
    in_hdr->ack = 2;
    in_hdr->off = 5 << 4;
    in_hdr->flags = TCP_RST;
    in_hdr->window_size = TCP_WINDOW_SIZE;
    in_hdr->checksum = 0;
    in_hdr->urgent = 0;
    tcp_input(in_pkt);
    test_case_end();

    // --------------------------------------------------------------------------------------------
    test_case_begin("CLOSED: ACK - RST sent");

    in_pkt = net_alloc_buf();
    in_hdr = (TCP_Header*)in_pkt->start;
    in_hdr->src_port = 100;
    in_hdr->dst_port = 101;
    in_hdr->seq = 1;
    in_hdr->ack = 2;
    in_hdr->off = 5 << 4;
    in_hdr->flags = TCP_ACK;
    in_hdr->window_size = TCP_WINDOW_SIZE;
    in_hdr->checksum = 0;
    in_hdr->urgent = 0;
    tcp_input(in_pkt);

    out_pkt = pop_packet();
    out_hdr = (TCP_Header*)out_pkt->data;
    tcp_swap(out_hdr);
    ASSERT_EQ_UINT(out_hdr->src_port, 101);
    ASSERT_EQ_UINT(out_hdr->dst_port, 100);
    ASSERT_EQ_UINT(out_hdr->seq, 2);
    ASSERT_EQ_UINT(out_hdr->ack, 0);
    ASSERT_EQ_HEX8(out_hdr->flags, TCP_RST);
    free(out_pkt);

    test_case_end();

    // --------------------------------------------------------------------------------------------
    test_case_begin("CLOSED: no ACK - RST/ACK sent");

    in_pkt = net_alloc_buf();
    in_hdr = (TCP_Header*)in_pkt->start;
    in_hdr->src_port = 100;
    in_hdr->dst_port = 101;
    in_hdr->seq = 1;
    in_hdr->ack = 2;
    in_hdr->off = 5 << 4;
    in_hdr->flags = 0;
    in_hdr->window_size = TCP_WINDOW_SIZE;
    in_hdr->checksum = 0;
    in_hdr->urgent = 0;
    tcp_input(in_pkt);

    out_pkt = pop_packet();
    out_hdr = (TCP_Header*)out_pkt->data;
    tcp_swap(out_hdr);
    ASSERT_EQ_UINT(out_hdr->src_port, 101);
    ASSERT_EQ_UINT(out_hdr->dst_port, 100);
    ASSERT_EQ_UINT(out_hdr->seq, 0);
    ASSERT_EQ_UINT(out_hdr->ack, 1);
    ASSERT_EQ_HEX8(out_hdr->flags, TCP_RST | TCP_ACK);
    free(out_pkt);

    test_case_end();

    // --------------------------------------------------------------------------------------------
    test_case_begin("SYN_SENT: Bad ACK, no RST - RST sent");

    conn = tcp_create();
    enter_state(conn, TCP_SYN_SENT);

    in_pkt = net_alloc_buf();
    in_hdr = (TCP_Header*)in_pkt->start;
    set_in_hdr(conn, in_hdr);
    in_hdr->seq = 1000;
    in_hdr->ack = conn->iss;
    in_hdr->flags = TCP_ACK;
    tcp_input(in_pkt);

    out_pkt = pop_packet();
    out_hdr = (TCP_Header*)out_pkt->data;
    tcp_swap(out_hdr);
    ASSERT_EQ_UINT(out_hdr->src_port, conn->local_port);
    ASSERT_EQ_UINT(out_hdr->dst_port, conn->remote_port);
    ASSERT_EQ_UINT(out_hdr->seq, in_hdr->ack);
    ASSERT_EQ_UINT(out_hdr->ack, 0);
    ASSERT_EQ_HEX8(out_hdr->flags, TCP_RST);
    free(out_pkt);

    exit_state(conn, TCP_SYN_SENT);

    test_case_end();

    // --------------------------------------------------------------------------------------------
    test_case_begin("SYN_SENT: Bad ACK, RST - segment dropped");

    conn = tcp_create();
    enter_state(conn, TCP_SYN_SENT);

    in_pkt = net_alloc_buf();
    in_hdr = (TCP_Header*)in_pkt->start;
    set_in_hdr(conn, in_hdr);
    in_hdr->seq = 1000;
    in_hdr->ack = conn->iss;
    in_hdr->flags = TCP_RST | TCP_ACK;
    tcp_input(in_pkt);

    exit_state(conn, TCP_SYN_SENT);

    test_case_end();

    // --------------------------------------------------------------------------------------------
    test_case_begin("SYN_SENT: ACK, RST - connection locally reset");

    conn = tcp_create();
    enter_state(conn, TCP_SYN_SENT);

    in_pkt = net_alloc_buf();
    in_hdr = (TCP_Header*)in_pkt->start;
    set_in_hdr(conn, in_hdr);
    in_hdr->seq = 1000;
    in_hdr->ack = conn->iss + 1;
    in_hdr->flags = TCP_RST | TCP_ACK;
    tcp_input(in_pkt);

    exit_state(conn, TCP_CLOSED);

    test_case_end();

    // --------------------------------------------------------------------------------------------
    test_case_begin("SYN_SENT: no ACK, RST - segment dropped");

    conn = tcp_create();
    enter_state(conn, TCP_SYN_SENT);

    in_pkt = net_alloc_buf();
    in_hdr = (TCP_Header*)in_pkt->start;
    set_in_hdr(conn, in_hdr);
    in_hdr->seq = 1000;
    in_hdr->ack = conn->iss + 1;
    in_hdr->flags = TCP_RST;
    tcp_input(in_pkt);

    exit_state(conn, TCP_SYN_SENT);

    test_case_end();

    // --------------------------------------------------------------------------------------------
    test_case_begin("SYN_SENT: SYN, ACK - transition to ESTABLISHED");

    conn = tcp_create();
    enter_state(conn, TCP_SYN_SENT);

    in_pkt = net_alloc_buf();
    in_hdr = (TCP_Header*)in_pkt->start;
    set_in_hdr(conn, in_hdr);
    in_hdr->seq = 1000;
    in_hdr->ack = conn->iss + 1;
    in_hdr->flags = TCP_SYN | TCP_ACK;
    tcp_input(in_pkt);

    ASSERT_EQ_UINT(conn->irs, 1000);
    ASSERT_EQ_UINT(conn->rcv_nxt, 1001);

    out_pkt = pop_packet();
    out_hdr = (TCP_Header*)out_pkt->data;
    tcp_swap(out_hdr);
    ASSERT_EQ_UINT(out_hdr->src_port, conn->local_port);
    ASSERT_EQ_UINT(out_hdr->dst_port, conn->remote_port);
    ASSERT_EQ_UINT(out_hdr->seq, conn->iss + 1);
    ASSERT_EQ_UINT(out_hdr->ack, 1001);
    ASSERT_EQ_HEX8(out_hdr->flags, TCP_ACK);
    free(out_pkt);

    exit_state(conn, TCP_ESTABLISHED);

    test_case_end();

    // --------------------------------------------------------------------------------------------
    test_case_begin("SYN_SENT: SYN, no ACK - transition to SYN_RECEIVED, resend SYN,ACK");

    conn = tcp_create();
    enter_state(conn, TCP_SYN_SENT);

    in_pkt = net_alloc_buf();
    in_hdr = (TCP_Header*)in_pkt->start;
    set_in_hdr(conn, in_hdr);
    in_hdr->seq = 1000;
    in_hdr->ack = 0;
    in_hdr->flags = TCP_SYN;
    tcp_input(in_pkt);

    ASSERT_EQ_UINT(conn->irs, 1000);
    ASSERT_EQ_UINT(conn->rcv_nxt, 1001);

    out_pkt = pop_packet();
    out_hdr = (TCP_Header*)out_pkt->data;
    tcp_swap(out_hdr);
    ASSERT_EQ_UINT(out_hdr->src_port, conn->local_port);
    ASSERT_EQ_UINT(out_hdr->dst_port, conn->remote_port);
    ASSERT_EQ_UINT(out_hdr->seq, conn->iss);
    ASSERT_EQ_UINT(out_hdr->ack, 1001);
    ASSERT_EQ_HEX8(out_hdr->flags, TCP_SYN | TCP_ACK);
    free(out_pkt);

    exit_state(conn, TCP_SYN_RECEIVED);

    test_case_end();

    return EXIT_SUCCESS;
}
