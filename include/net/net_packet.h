/**
 * \file net_packet.h
 * \date 07.08.09
 * \author sikmir
 */
#ifndef NET_PACKET_H_
#define NET_PACKET_H_

#define ETHERNET_V2_FRAME_SIZE 1518

typedef struct _net_packet { /* = struct sk_buff in Linux */
        struct _net_device      *netdev;
        void                    *ifdev;
        struct sock             *sk;
        unsigned short          protocol;
        unsigned int            len;
        union {
                //tcphdr        *th;
                struct _udphdr  *uh;
                struct _icmphdr *icmph;
                //igmphdr       *igmph;
                //iphdr         *ipiph;
                //ipv6hdr       *ipv6h;
    	        unsigned char   *raw;
        } h;
        union {
                struct _iphdr   *iph;
        	//ipv6hdr       *ipv6h;
                struct _arphdr  *arph;
                unsigned char   *raw;
        } nh;
        union {
                struct _ethhdr  *ethh;
                unsigned char   *raw;
        } mac;
        unsigned char data[ETHERNET_V2_FRAME_SIZE];
}net_packet;

#endif /* NET_PACKET_H_ */
