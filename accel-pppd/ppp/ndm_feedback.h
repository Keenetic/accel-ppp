/*
 * Copyright (C) NDM Systems Inc.
 */

#ifndef _NDM_FEEDBACK_H_INCLUDED_
#define _NDM_FEEDBACK_H_INCLUDED_

#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>

#define NGX_NDM_BINXML_CMD_NODE			0
#define NGX_NDM_BINXML_CMD_ATTR			1
#define NGX_NDM_BINXML_CMD_SIBLING		2
#define NGX_NDM_BINXML_CMD_END			3

#define NGX_NDM_BINXML_TYPE_DOC			0
#define NGX_NDM_BINXML_TYPE_ELEMENT		1

#define NGX_NDM_BINXML_PUT_COMMAND(p, cmd, type) \
	do { \
		*(p) = (char)((((unsigned char)(cmd) << 6) | ((unsigned char)(type))) & 0xFF); \
		++(p); \
	} while (0);

#define NGX_NDM_BINXML_PUT_STRING(p, strval, strlen) \
	do { \
		*((uint32_t *)(p)) = htonl(strlen); \
		(p) += sizeof(uint32_t); \
		if( (strlen) > 0) { \
			memcpy((p), (strval), (strlen)); \
			(p) += (strlen); \
		} \
	} while (0);

#define NGX_NDM_BINXML_PUT_EMPTY_STRING(p) \
	NGX_NDM_BINXML_PUT_STRING(p, "", 0)

#define NGX_NDM_MESSAGE_BUFFER		200
#define NGX_NDM_FROM_NODE			"/tmp/run/accel-ppp/accel-ppp.fb"
#define NGX_NDM_FROM_NODE_SIZE		31

static void
ndm_send_feedback(const char* addr, const uint16_t port)
{
	char buffer[NGX_NDM_MESSAGE_BUFFER];
	char * p = buffer;
	char addrbuffer[256];
	size_t addrlen = 0;
	size_t len = 0;
	size_t pos = 0;

	memset(addrbuffer, 0, sizeof(addrbuffer));

	while (addr[pos] != '\0' && addr[pos] != ':' && pos < sizeof(addrbuffer) - 1) {
		addrbuffer[pos] = addr[pos];
		pos++;
	}

	addrbuffer[pos] = '\0';
	addrlen = strlen(addrbuffer);

	// NODE + DOCUMENT
	NGX_NDM_BINXML_PUT_COMMAND(p, NGX_NDM_BINXML_CMD_NODE, NGX_NDM_BINXML_TYPE_DOC);

	NGX_NDM_BINXML_PUT_EMPTY_STRING(p);
	NGX_NDM_BINXML_PUT_EMPTY_STRING(p);

	// NODE + ELEMENT
	NGX_NDM_BINXML_PUT_COMMAND(p, NGX_NDM_BINXML_CMD_NODE, NGX_NDM_BINXML_TYPE_ELEMENT);

	NGX_NDM_BINXML_PUT_STRING(p, "feedback", 8);
	NGX_NDM_BINXML_PUT_EMPTY_STRING(p);

	// NODE + ELEMENT
	NGX_NDM_BINXML_PUT_COMMAND(p, NGX_NDM_BINXML_CMD_NODE, NGX_NDM_BINXML_TYPE_ELEMENT);

	NGX_NDM_BINXML_PUT_STRING(p, "from", 4);
	NGX_NDM_BINXML_PUT_STRING(p, NGX_NDM_FROM_NODE, NGX_NDM_FROM_NODE_SIZE);

	// SIBLING + ELEMENT
	NGX_NDM_BINXML_PUT_COMMAND(p, NGX_NDM_BINXML_CMD_SIBLING, NGX_NDM_BINXML_TYPE_ELEMENT);

	NGX_NDM_BINXML_PUT_STRING(p, "input", 5);
	NGX_NDM_BINXML_PUT_EMPTY_STRING(p);

	// NODE + ELEMENT
	NGX_NDM_BINXML_PUT_COMMAND(p, NGX_NDM_BINXML_CMD_NODE, NGX_NDM_BINXML_TYPE_ELEMENT);

	NGX_NDM_BINXML_PUT_STRING(p, "item", 4);
	NGX_NDM_BINXML_PUT_STRING(p, addrbuffer, addrlen);

	// END + DOCUMENT
	NGX_NDM_BINXML_PUT_COMMAND(p, NGX_NDM_BINXML_CMD_END, NGX_NDM_BINXML_TYPE_DOC);

	// SIBLING + ELEMENT
	NGX_NDM_BINXML_PUT_COMMAND(p, NGX_NDM_BINXML_CMD_SIBLING, NGX_NDM_BINXML_TYPE_ELEMENT);

	NGX_NDM_BINXML_PUT_STRING(p, "env", 3);
	NGX_NDM_BINXML_PUT_EMPTY_STRING(p);

	// END + DOCUMENT
	NGX_NDM_BINXML_PUT_COMMAND(p, NGX_NDM_BINXML_CMD_END, NGX_NDM_BINXML_TYPE_DOC);

	// END + DOCUMENT
	NGX_NDM_BINXML_PUT_COMMAND(p, NGX_NDM_BINXML_CMD_END, NGX_NDM_BINXML_TYPE_DOC);

	// END + DOCUMENT
	NGX_NDM_BINXML_PUT_COMMAND(p, NGX_NDM_BINXML_CMD_END, NGX_NDM_BINXML_TYPE_DOC);

	len = (p - buffer);

	{
		int sockfd = socket(PF_INET, SOCK_DGRAM, 0);
		struct sockaddr_in serveraddr;
		int flags = 0;

		if (sockfd < 0) {
			return;
		}

		if ((flags = fcntl(sockfd, F_GETFL, 0)) == -1) {
			close(sockfd);
			return;
		}

		if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) == -1) {
			close(sockfd);
			return;
		}

		memset(&serveraddr, 0, sizeof(struct sockaddr_in));

		serveraddr.sin_family = AF_INET;
		serveraddr.sin_addr.s_addr = htonl(0x7F000001); // loopback
		serveraddr.sin_port = htons(port);

		sendto(sockfd, &buffer, len, 0, (const struct sockaddr*) &serveraddr, sizeof(serveraddr));
		close(sockfd);
	}
}

#endif /* _NDM_FEEDBACK_H_INCLUDED_ */
