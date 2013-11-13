/*  LAN_discovery.c
 *
 *  LAN discovery implementation.
 *
 *  Copyright (C) 2013 Tox project All Rights Reserved.
 *
 *  This file is part of Tox.
 *
 *  Tox is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Tox is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Tox.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "LAN_discovery.h"
#include "util.h"

#define MAX_INTERFACES 16

#ifdef __linux

static int     broadcast_count = -1;
static IP_Port broadcast_ip_port[MAX_INTERFACES];

static void fetch_broadcast_info(uint16_t port)
{
    /* Not sure how many platforms this will run on,
     * so it's wrapped in __linux for now.
     * Definitely won't work like this on Windows...
     */
    broadcast_count = 0;
    sock_t sock = 0;

    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        return;

    /* Configure ifconf for the ioctl call. */
    struct ifreq i_faces[MAX_INTERFACES];
    memset(i_faces, 0, sizeof(struct ifreq) * MAX_INTERFACES);

    struct ifconf ifconf;
    ifconf.ifc_buf = (char *)i_faces;
    ifconf.ifc_len = sizeof(i_faces);

    if (ioctl(sock, SIOCGIFCONF, &ifconf) < 0) {
        close(sock);
        return;
    }

    /* ifconf.ifc_len is set by the ioctl() to the actual length used;
     * on usage of the complete array the call should be repeated with
     * a larger array, not done (640kB and 16 interfaces shall be
     * enough, for everybody!)
     */
    int i, count = ifconf.ifc_len / sizeof(struct ifreq);

    for (i = 0; i < count; i++) {
        /* there are interfaces with are incapable of broadcast */
        if (ioctl(sock, SIOCGIFBRDADDR, &i_faces[i]) < 0)
            continue;

        /* moot check: only AF_INET returned (backwards compat.) */
        if (i_faces[i].ifr_broadaddr.sa_family != AF_INET)
            continue;

        struct sockaddr_in *sock4 = (struct sockaddr_in *)&i_faces[i].ifr_broadaddr;

        if (broadcast_count >= MAX_INTERFACES)
            return;

        IP_Port *ip_port = &broadcast_ip_port[broadcast_count];
        ip_port->ip.family = AF_INET;
        ip_port->ip.ip4.in_addr = sock4->sin_addr;
        ip_port->port = port;
        broadcast_count++;
    }

    close(sock);
}

/* Send packet to all IPv4 broadcast addresses
 *
 *  return 1 if sent to at least one broadcast target.
 *  return 0 on failure to find any valid broadcast target.
 */
static uint32_t send_broadcasts(Networking_Core *net, uint16_t port, uint8_t *data, uint16_t length)
{
    /* fetch only once? on every packet? every X seconds?
     * old: every packet, new: once */
    if (broadcast_count < 0)
        fetch_broadcast_info(port);

    if (!broadcast_count)
        return 0;

    int i;

    for (i = 0; i < broadcast_count; i++)
        sendpacket(net, broadcast_ip_port[i], data, 1 + crypto_box_PUBLICKEYBYTES);

    return 1;
}
#endif /* __linux */

/* Return the broadcast ip. */
static IP broadcast_ip(sa_family_t family_socket, sa_family_t family_broadcast)
{
    IP ip;
    ip_reset(&ip);

    if (family_socket == AF_INET6) {
        if (family_broadcast == AF_INET6) {
            ip.family = AF_INET6;
            /* FF02::1 is - according to RFC 4291 - multicast all-nodes link-local */
            /* FE80::*: MUST be exact, for that we would need to look over all
             * interfaces and check in which status they are */
            ip.ip6.uint8[ 0] = 0xFF;
            ip.ip6.uint8[ 1] = 0x02;
            ip.ip6.uint8[15] = 0x01;
        } else if (family_broadcast == AF_INET) {
            ip.family = AF_INET6;
            ip.ip6.uint32[0] = 0;
            ip.ip6.uint32[1] = 0;
            ip.ip6.uint32[2] = htonl(0xFFFF);
            ip.ip6.uint32[3] = INADDR_BROADCAST;
        }
    } else if (family_socket == AF_INET) {
        if (family_broadcast == AF_INET) {
            ip.family = AF_INET;
            ip.ip4.uint32 = INADDR_BROADCAST;
        }
    }

    return ip;
}

/*  return 0 if ip is a LAN ip.
 *  return -1 if it is not.
 */
int LAN_ip(IP ip)
{
    if (ip.family == AF_INET) {
        IP4 ip4 = ip.ip4;

        /* Loopback. */
        if (ip4.uint8[0] == 127)
            return 0;

        /* 10.0.0.0 to 10.255.255.255 range. */
        if (ip4.uint8[0] == 10)
            return 0;

        /* 172.16.0.0 to 172.31.255.255 range. */
        if (ip4.uint8[0] == 172 && ip4.uint8[1] >= 16 && ip4.uint8[1] <= 31)
            return 0;

        /* 192.168.0.0 to 192.168.255.255 range. */
        if (ip4.uint8[0] == 192 && ip4.uint8[1] == 168)
            return 0;

        /* 169.254.1.0 to 169.254.254.255 range. */
        if (ip4.uint8[0] == 169 && ip4.uint8[1] == 254 && ip4.uint8[2] != 0
                && ip4.uint8[2] != 255)
            return 0;

    } else if (ip.family == AF_INET6) {

        /* autogenerated for each interface: FE80::* (up to FEBF::*)
           FF02::1 is - according to RFC 4291 - multicast all-nodes link-local */
        if (((ip.ip6.uint8[0] == 0xFF) && (ip.ip6.uint8[1] < 3) && (ip.ip6.uint8[15] == 1)) ||
                ((ip.ip6.uint8[0] == 0xFE) && ((ip.ip6.uint8[1] & 0xC0) == 0x80)))
            return 0;

        /* embedded IPv4-in-IPv6 */
        if (IN6_IS_ADDR_V4MAPPED(&ip.ip6.in6_addr)) {
            IP ip4;
            ip4.family = AF_INET;
            ip4.ip4.uint32 = ip.ip6.uint32[3];
            return LAN_ip(ip4);
        }

        /* localhost in IPv6 (::1) */
        if (IN6_IS_ADDR_LOOPBACK(&ip.ip6.in6_addr))
            return 0;
    }

    return -1;
}

static int handle_LANdiscovery(void *object, IP_Port source, uint8_t *packet, uint32_t length)
{
    DHT *dht = object;

    if (LAN_ip(source.ip) == -1)
        return 1;

    if (length != crypto_box_PUBLICKEYBYTES + 1)
        return 1;

    DHT_bootstrap(dht, source, packet + 1);
    return 0;
}


int send_LANdiscovery(uint16_t port, Net_Crypto *c)
{
    uint8_t data[crypto_box_PUBLICKEYBYTES + 1];
    data[0] = NET_PACKET_LAN_DISCOVERY;
    id_copy(data + 1, c->self_public_key);

#ifdef __linux
    send_broadcasts(c->lossless_udp->net, port, data, 1 + crypto_box_PUBLICKEYBYTES);
#endif
    int res = -1;
    IP_Port ip_port;
    ip_port.port = port;

    /* IPv6 multicast */
    if (c->lossless_udp->net->family == AF_INET6) {
        ip_port.ip = broadcast_ip(AF_INET6, AF_INET6);

        if (ip_isset(&ip_port.ip))
            if (sendpacket(c->lossless_udp->net, ip_port, data, 1 + crypto_box_PUBLICKEYBYTES) > 0)
                res = 1;
    }

    /* IPv4 broadcast (has to be IPv4-in-IPv6 mapping if socket is AF_INET6 */
    ip_port.ip = broadcast_ip(c->lossless_udp->net->family, AF_INET);

    if (ip_isset(&ip_port.ip))
        if (sendpacket(c->lossless_udp->net, ip_port, data, 1 + crypto_box_PUBLICKEYBYTES))
            res = 1;

    return res;
}


void LANdiscovery_init(DHT *dht)
{
    networking_registerhandler(dht->c->lossless_udp->net, NET_PACKET_LAN_DISCOVERY, &handle_LANdiscovery, dht);
}
