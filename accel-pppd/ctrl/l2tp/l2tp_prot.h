#ifndef __L2TP_PROT_H
#define __L2TP_PROT_H

#include <stdint.h>

#define L2TP_PORT 1701

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__

struct l2tp_hdr_t
{
	uint8_t P:1;
	uint8_t O:1;
	uint8_t reserved2:1;
	uint8_t S:1;
	uint8_t reserved1:2;
	uint8_t L:1;
	uint8_t T:1;
	uint8_t ver:4;
	uint8_t reserved3:4;
	uint16_t length;
	union {
		struct {
			uint16_t tid;
			uint16_t sid;
		};
		uint32_t cid;
	};
	uint16_t Ns;
	uint16_t Nr;
} __attribute__((packed));

struct l2tp_avp_t
{
	uint16_t length:10;
	uint16_t reserved:4;
	uint16_t H:1;
	uint16_t M:1;
	uint16_t vendor;
	uint16_t type;
	uint8_t val[0];
} __attribute__((packed));

#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__

struct l2tp_hdr_t
{
	uint8_t T:1;
	uint8_t L:1;
	uint8_t reserved1:2;
	uint8_t S:1;
	uint8_t reserved2:1;
	uint8_t O:1;
	uint8_t P:1;
	uint8_t reserved3:4;
	uint8_t ver:4;
	uint16_t length;
	union {
		struct {
			uint16_t tid;
			uint16_t sid;
		};
		uint32_t cid;
	};
	uint16_t Ns;
	uint16_t Nr;
} __attribute__((packed));

struct l2tp_avp_t
{
	uint16_t M:1;
	uint16_t H:1;
	uint16_t reserved:4;
	uint16_t length:10;
	uint16_t vendor;
	uint16_t type;
	uint8_t val[0];
} __attribute__((packed));

#endif

struct l2tp_avp_result_code
{
	uint16_t result_code;
	uint16_t error_code;
	char error_msg[0];
} __attribute__((packed));

#endif

