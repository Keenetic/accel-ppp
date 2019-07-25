#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include "linux_ppp.h"

#include "ppp.h"
#include "ppp_ccp.h"
#include "log.h"
#include "events.h"

#include "memdebug.h"

#define MPPE_H (1 << 24)
#define MPPE_M (1 << 7)
#define MPPE_S (1 << 6)
#define MPPE_L (1 << 5)
#define MPPE_D (1 << 4)
#define MPPE_C (1 << 0)

#define MPPE_PAD 4

static struct ccp_option_t *mppe_init(struct ppp_ccp_t *ccp);
static void mppe_free(struct ppp_ccp_t *ccp, struct ccp_option_t *opt);
static int __mppe_send_conf_req(struct ppp_ccp_t *ccp, struct ccp_option_t *opt, uint8_t *ptr, int setup_key);
static int mppe_send_conf_req(struct ppp_ccp_t *ccp, struct ccp_option_t *opt, uint8_t *ptr);
static int mppe_send_conf_nak(struct ppp_ccp_t *ccp, struct ccp_option_t *opt, uint8_t *ptr);
static int mppe_recv_conf_req(struct ppp_ccp_t *ccp, struct ccp_option_t *opt, uint8_t *ptr);
static int mppe_recv_conf_nak(struct ppp_ccp_t *ccp, struct ccp_option_t *opt, uint8_t *ptr);
static int mppe_recv_conf_ack(struct ppp_ccp_t *ccp, struct ccp_option_t *opt, uint8_t *ptr);
static int mppe_recv_conf_rej(struct ppp_ccp_t *ccp, struct ccp_option_t *opt, uint8_t *ptr);
static void mppe_print(void (*print)(const char *fmt,...),struct ccp_option_t*, uint8_t *ptr);

static int conf_mppe = MPPE_ALLOW;
static int mppe_40 = 1;
static int mppe_128 = 0;

struct mppe_option_t
{
	struct ccp_option_t opt;
	int mppe;
	int enabled;
	uint8_t recv_key[16];
	uint8_t send_key[16];
	int policy; // 1 - allowed, 2 - required
	int mppe_40;
	int mppe_128;
	int retry;
};

static struct ccp_option_handler_t mppe_opt_hnd = {
	.init = mppe_init,
	.send_conf_req = mppe_send_conf_req,
	.send_conf_nak = mppe_send_conf_nak,
	.recv_conf_req = mppe_recv_conf_req,
	.recv_conf_nak = mppe_recv_conf_nak,
	.recv_conf_ack = mppe_recv_conf_ack,
	.recv_conf_rej = mppe_recv_conf_rej,
	.free = mppe_free,
	.print = mppe_print,
};

static void log_mppe_state(struct mppe_option_t *mppe_opt)
{
	log_ppp_debug("mppe: {state m=%x e=%x p=%x 40=%x 128=%x r=%x}\n",
		mppe_opt->mppe,
		mppe_opt->enabled,
		mppe_opt->policy,
		mppe_opt->mppe_40,
		mppe_opt->mppe_128,
		mppe_opt->retry);
}

static struct ccp_option_t *mppe_init(struct ppp_ccp_t *ccp)
{
	struct mppe_option_t *mppe_opt = _malloc(sizeof(*mppe_opt));
	memset(mppe_opt, 0, sizeof(*mppe_opt));
	int mppe;

	if (ccp->ppp->ses.ctrl->mppe == MPPE_UNSET)
		mppe = conf_mppe;
	else
		mppe = ccp->ppp->ses.ctrl->mppe;

	if (mppe != MPPE_ALLOW)
		mppe_opt->policy = mppe;
	else
		mppe_opt->policy = 1;

	if (mppe > 0)
		mppe_opt->mppe = 1;
	else
		mppe_opt->mppe = -1;

	if (mppe == MPPE_REQUIRE || mppe == MPPE_PREFER)
		ccp->ld.passive = 0;

	if (mppe == MPPE_REQUIRE)
		ccp->ld.optional = 0;

	if (mppe == MPPE_REQUIRE)
		ccp->ld.optional = 0;

	mppe_opt->opt.id = CI_MPPE;
	mppe_opt->opt.len = 6;

	mppe_opt->mppe_40 = mppe_40;
	mppe_opt->mppe_128 = mppe_128;

	mppe_opt->retry = 0;

	log_ppp_debug("mppe: init\n");
	log_mppe_state(mppe_opt);

	return &mppe_opt->opt;
}

static void mppe_free(struct ppp_ccp_t *ccp, struct ccp_option_t *opt)
{
	struct mppe_option_t *mppe_opt = container_of(opt, typeof(*mppe_opt), opt);

	_free(mppe_opt);
}

static int setup_mppe_key(int fd, int transmit, struct mppe_option_t *mppe_opt, uint8_t *key, int log)
{
	struct ppp_option_data data;
	uint8_t buf[6 + 16];

	log_ppp_debug("mppe: setup key for %s\n", (transmit ? "send" : "recv"));
	log_mppe_state(mppe_opt);

	if (!mppe_opt->mppe_128 && !mppe_opt->mppe_40) {
		log_ppp_warn("mppe: neither 40 nor 128 bit mode was selected\n");
		return -1;
	}

	if (mppe_opt->mppe_128) {
		if (log)
			log_ppp_info1("mppe: using 128 bit stateless mode\n");
		else
			log_ppp_info2("mppe: using 128 bit stateless mode\n");
	} else {
		if (log)
			log_ppp_info1("mppe: using 40 bit stateless mode\n");
		else
			log_ppp_info2("mppe: using 40 bit stateless mode\n");
	}

	memset(buf, 0, sizeof(buf));
	buf[0] = CI_MPPE;
	buf[1] = 6;
	*(uint32_t*)(buf + 2) = htonl((mppe_opt->mppe_128 ? MPPE_S : MPPE_L) | MPPE_H);
	if (key)
		memcpy(buf + 6, key, 16 - (mppe_opt->mppe_128 ? 0 : 8));

	memset(&data, 0, sizeof(data));
	data.ptr = buf;
	data.length = sizeof(buf);
	data.transmit = transmit;

	if (net->ppp_ioctl(fd, PPPIOCSCOMPRESS, &data)) {
		log_ppp_warn("mppe: MPPE requested but not supported by kernel\n");
		return -1;
	}

	return 0;
}

static int decrease_mtu(struct ppp_t *ppp)
{
	struct ifreq ifr;

	strcpy(ifr.ifr_name, ppp->ses.ifname);

	if (net->sock_ioctl(SIOCGIFMTU, &ifr)) {
		log_ppp_error("mppe: failed to get MTU: %s\n", strerror(errno));
		return -1;
	}

	ifr.ifr_mtu -= MPPE_PAD;

	if (net->sock_ioctl(SIOCSIFMTU, &ifr)) {
		log_ppp_error("mppe: failed to set MTU: %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

static int __mppe_send_conf_req(struct ppp_ccp_t *ccp, struct ccp_option_t *opt, uint8_t *ptr, int setup_key)
{
	struct mppe_option_t *mppe_opt = container_of(opt,typeof(*mppe_opt),opt);
	struct ccp_opt32_t *opt32 = (struct ccp_opt32_t*)ptr;

	if (mppe_opt->mppe != -1) {
		opt32->hdr.id = CI_MPPE;
		opt32->hdr.len = 6;
		opt32->val = mppe_opt->mppe ? htonl((mppe_opt->mppe_128 ? MPPE_S : (mppe_opt->mppe_40 ? MPPE_L : 0)) | MPPE_H) : 0;

		if (setup_key && mppe_opt->mppe && setup_mppe_key(ccp->ppp->unit_fd, 0, mppe_opt, mppe_opt->recv_key, 0))
			return 0;

		return 6;
	}
	return 0;
}

static int mppe_send_conf_req(struct ppp_ccp_t *ccp, struct ccp_option_t *opt, uint8_t *ptr)
{
	struct mppe_option_t *mppe_opt = container_of(opt, typeof(*mppe_opt), opt);

	log_ppp_debug("mppe: sent [ConfReq id=%x]\n", opt->id);
	log_mppe_state(mppe_opt);

	return __mppe_send_conf_req(ccp, opt, ptr, 1);
}

static int mppe_send_conf_nak(struct ppp_ccp_t *ccp, struct ccp_option_t *opt, uint8_t *ptr)
{
	struct mppe_option_t *mppe_opt = container_of(opt, typeof(*mppe_opt), opt);

	log_ppp_debug("mppe: sent [ConfNak id=%x]\n", opt->id);
	log_mppe_state(mppe_opt);

	return __mppe_send_conf_req(ccp, opt, ptr, 0);
}

static int mppe_recv_conf_req(struct ppp_ccp_t *ccp, struct ccp_option_t *opt, uint8_t *ptr)
{
	struct mppe_option_t *mppe_opt = container_of(opt, typeof(*mppe_opt), opt);
	struct ccp_opt32_t *opt32 = (struct ccp_opt32_t *)ptr;
	int mppe;
	long bits = 0;
	int changed = 0;

	log_ppp_debug("mppe: recv [ConfReq id=%x]\n", opt->id);

	if (ccp->ppp->ses.ctrl->mppe == MPPE_UNSET)
		mppe = conf_mppe;
	else
		mppe = ccp->ppp->ses.ctrl->mppe;

	if (!ptr) {
		log_ppp_debug("mppe: no MPPE/MPPC option found\n");

		if (mppe_opt->policy == 2)
			return CCP_OPT_NAK;
		return CCP_OPT_ACK;
	}

	if (opt32->hdr.len != 6)
		return CCP_OPT_REJ;

	log_mppe_state(mppe_opt);

	bits = ntohl(opt32->val);
	changed =
		((bits & (MPPE_H | MPPE_L | MPPE_M | MPPE_S | MPPE_C)) !=
		 (MPPE_H | (mppe_opt->mppe_40 ? MPPE_L : 0) | (mppe_opt->mppe_128 ? MPPE_S : 0)));
	mppe_opt->mppe_40 = mppe_opt->mppe_40 && (bits & MPPE_L);
	mppe_opt->mppe_128 = mppe_opt->mppe_128 && (bits & MPPE_S);

	log_mppe_state(mppe_opt);

	if (changed)
		log_ppp_debug("mppe: state changed\n");

	if (mppe_opt->policy == 2) {
		if ((!mppe_opt->mppe_40 && !mppe_opt->mppe_128) ||
				!(bits & MPPE_H)) {
			if (!mppe_opt->retry) {
				log_ppp_debug("mppe: retry to enable encryption\n");
				++mppe_opt->retry;
				mppe_opt->mppe_40 = mppe_40;
				mppe_opt->mppe_128 = mppe_128;

				return CCP_OPT_NAK;
			}

			log_ppp_info1("mppe: unencrypted connections are prohibited\n");

			return CCP_OPT_REJ;
		} else
		if (changed) {
			log_ppp_debug("mppe: options changed, sent NAK\n");
			return CCP_OPT_NAK;
		}
	} else if (mppe_opt->policy == 1) {
		if ((bits & MPPE_H) && (mppe_opt->mppe_40 || mppe_opt->mppe_128)) {
			log_ppp_debug("mppe: encryption negotiated\n");
			mppe_opt->mppe = 1;
			if (changed) {
				log_ppp_debug("mppe: options changed, sent NAK\n");
				return CCP_OPT_NAK;
			}
		} else if (opt32->val || mppe) {
			if (!mppe_opt->retry) {
				log_ppp_debug("mppe: invalid options, retry to enable\n");
				++mppe_opt->retry;
				mppe_opt->mppe_40 = mppe_40;
				mppe_opt->mppe_128 = mppe_128;
			} else {
				mppe_opt->mppe = 0;
				log_ppp_debug("mppe: allow unencrypted connection, sent NAK\n");
			}

			return CCP_OPT_NAK;
		} else {
			mppe_opt->mppe = 0;
			log_ppp_debug("mppe: allow unencrypted connection\n");
		}
	} else {
		log_ppp_debug("mppe: reject connection\n");
		return CCP_OPT_REJ;
	}

	if (bits & MPPE_C) {
		log_ppp_debug("mppc requested, send NAK\n");
		return CCP_OPT_NAK;
	}

	if (mppe_opt->mppe) {
		if (setup_mppe_key(ccp->ppp->unit_fd, 1, mppe_opt, mppe_opt->send_key, 1))
			return CCP_OPT_REJ;

		if (!mppe_opt->enabled) {
			decrease_mtu(ccp->ppp);
			mppe_opt->enabled = 1;
		}

		log_ppp_debug(" (mppe enabled)");
		ccp->ppp->ses.comp = "mppe";
	} else
		ccp->ppp->ses.comp = NULL;

	return CCP_OPT_ACK;
}

static int mppe_recv_conf_rej(struct ppp_ccp_t *ccp, struct ccp_option_t *opt, uint8_t *ptr)
{
	struct mppe_option_t *mppe_opt = container_of(opt, typeof(*mppe_opt), opt);
	struct ccp_opt32_t *opt32 = (struct ccp_opt32_t *)ptr;
	long bits = 0;

	log_ppp_debug("mppe: recv [ConfRej id=%x len=%x]\n", opt->id, opt32->hdr.len);
	log_mppe_state(mppe_opt);

	if (mppe_opt->policy != 2) {
		if (opt32->hdr.len != 6) {
			mppe_opt->mppe = -1;

			log_ppp_debug("mppe: fallback to default\n");

			return 0;
		}

		bits = ntohl(opt32->val);

		if ((mppe_40 && (bits & MPPE_L)) || (mppe_128 && (bits & MPPE_S))) {
			log_ppp_info1("mppe: encryption rejected, proceed\n");
			mppe_opt->mppe = -1;
		}

		log_mppe_state(mppe_opt);

		if (bits & MPPE_C) {
			log_ppp_info1("mppe: mppc required, terminate\n");
			return -1;
		}

		return 0;
	}

	if (opt32->hdr.len != 6)
		return -1;

	bits = ntohl(opt32->val);

	if ((mppe_40 && (bits & MPPE_L)) || (mppe_128 && (bits & MPPE_S))) {
		log_ppp_info1("mppe: encryption required, but rejected, terminate\n");
		return -1;
	}

	if (bits & MPPE_C) {
		log_ppp_info1("mppe:mppc required, terminate\n");
		return -1;
	}

	return 0;
}

static int mppe_recv_conf_ack(struct ppp_ccp_t *ccp, struct ccp_option_t *opt, uint8_t *ptr)
{
	struct mppe_option_t *mppe_opt = container_of(opt, typeof(*mppe_opt), opt);
	struct ccp_opt32_t *opt32 = (struct ccp_opt32_t *)ptr;
	long bits = 0;
	int has_mppe = 0;

	log_ppp_debug("mppe: [ConfAck id=%x len=%d]\n", opt->id, opt32->hdr.len);
	log_mppe_state(mppe_opt);

	if (opt32->hdr.len != 6)
		return -1;

	bits = ntohl(opt32->val);
	mppe_opt->mppe_40 = mppe_40 && (bits & MPPE_L);
	mppe_opt->mppe_128 = mppe_128 && (bits & MPPE_S);
	has_mppe = (mppe_opt->mppe_40 || mppe_opt->mppe_128) && (bits & MPPE_H);

	log_mppe_state(mppe_opt);

	if (has_mppe)
		log_ppp_debug("mppe: mppe acknowleged\n");

	if (bits & MPPE_C) {
		log_ppp_info1("mppe: mppc required, terminate\n");
		return -1;
	}

	if (mppe_opt->policy == 2) {
		if (!has_mppe) {
			log_ppp_info1("mppe: encryption required, but rejected, terminate\n");
			return -1;
		}
	} else if (mppe_opt->policy == 1) {
		log_ppp_debug("mppe: proceed with new state\n");
		mppe_opt->mppe = has_mppe;
	} else {
		if (opt32->val == 0) {
			log_ppp_debug("mppe: invalid options in ACK\n");
			return -1;
		}
	}

	return 0;
}

static int mppe_recv_conf_nak(struct ppp_ccp_t *ccp, struct ccp_option_t *opt, uint8_t *ptr)
{
	struct mppe_option_t *mppe_opt = container_of(opt, typeof(*mppe_opt), opt);
	struct ccp_opt32_t *opt32 = (struct ccp_opt32_t *)ptr;
	long bits = 0;
	int has_mppe = 0;

	log_ppp_debug("mppe: [ConfNak id=%x len=%d]\n", opt->id, opt32->hdr.len);
	log_mppe_state(mppe_opt);

	if (opt32->hdr.len != 6)
		return -1;

	bits = ntohl(opt32->val);
	mppe_opt->mppe_40 = mppe_opt->mppe_40 && (bits & MPPE_L);
	mppe_opt->mppe_128 = mppe_opt->mppe_128 && (bits & MPPE_S);
	has_mppe = (mppe_opt->mppe_40 || mppe_opt->mppe_128) && (bits & MPPE_H);

	log_mppe_state(mppe_opt);

	if (has_mppe)
		log_ppp_debug("mppe: mppe acknowleged\n");

	if (mppe_opt->policy == 2) {
		if (!has_mppe) {
			log_ppp_info1("mppe: encryption required, but rejected, terminate\n");
			return -1;
		}
	} else if (mppe_opt->policy == 1) {
		log_ppp_debug("mppe: proceed with new state\n");
		mppe_opt->mppe = has_mppe;
	} else {
		if (opt32->val == 0) {
			log_ppp_debug("mppe: invalid options in NAK\n");
			return -1;
		}
	}

	return 0;
}

static void mppe_print(void (*print)(const char *fmt,...),struct ccp_option_t *opt, uint8_t *ptr)
{
	struct mppe_option_t *mppe_opt = container_of(opt, typeof(*mppe_opt), opt);
	struct ccp_opt32_t *opt32 = (struct ccp_opt32_t *)ptr;
	uint32_t bits;

	if (ptr)
		bits = ntohl(opt32->val);
	else
		if (mppe_opt->mppe)
			bits = (mppe_40 ? MPPE_L : 0) | (mppe_128 ? MPPE_S : 0) | MPPE_H;
		else
			bits = 0;

	print("<mppe %sH %sM %sS %sL %sD %sC>",
		bits & MPPE_H ? "+" : "-",
		bits & MPPE_M ? "+" : "-",
		bits & MPPE_S ? "+" : "-",
		bits & MPPE_L ? "+" : "-",
		bits & MPPE_D ? "+" : "-",
		bits & MPPE_C ? "+" : "-"
	);
}

static void ev_mppe_keys(struct ev_mppe_keys_t *ev)
{
	struct ppp_ccp_t *ccp = ccp_find_layer_data(ev->ppp);
	struct mppe_option_t *mppe_opt = container_of(ccp_find_option(ev->ppp, &mppe_opt_hnd), typeof(*mppe_opt), opt);
	int mppe;

	memcpy(mppe_opt->recv_key, ev->recv_key, 16);
	memcpy(mppe_opt->send_key, ev->send_key, 16);

	if (ev->policy == -1)
		return;

	if ((ev->type & 0x04) == 0) {
		log_ppp_warn("mppe: 128-bit session keys not allowed, disabling mppe ...\n");
		mppe_opt->mppe = 0;
		return;
	}

	if (ccp->ppp->ses.ctrl->mppe == MPPE_UNSET)
		mppe = conf_mppe;
	else
		mppe = ev->ppp->ses.ctrl->mppe;

	if (ev->ppp->ses.ctrl->mppe == MPPE_UNSET) {
		mppe_opt->policy = ev->policy;

		if (ev->policy == 2) {
			mppe_opt->mppe = 1;
			ccp->ld.passive = 0;
		} else if (ev->policy == 1) {
			if (mppe == 1)
				mppe_opt->mppe = 1;
			else
				mppe_opt->mppe = -1;

			if (mppe == 2)
				ccp->ld.passive = 1;
		}
	}
}

static void load_config(void)
{
	const char *opt;

	opt = conf_get_opt("ppp", "mppe");
	if (opt) {
		if (!strcmp(opt,"require"))
			conf_mppe = MPPE_REQUIRE;
		else if (!strcmp(opt,"prefer") || !strcmp(opt,"prefere"))
			conf_mppe = MPPE_PREFER;
		else if (!strcmp(opt,"deny"))
			conf_mppe = MPPE_DENY;
	} else
		conf_mppe = MPPE_ALLOW;

	opt = conf_get_opt("ppp", "mppe-128");

	if (opt) {
		if (!strcmp(opt, "1"))
			mppe_128 = 1;
		else
			mppe_128 = 0;
	}

	opt = conf_get_opt("ppp", "mppe-40");

	if (opt) {
		if (!strcmp(opt, "1"))
			mppe_40 = 1;
		else
			mppe_40 = 0;
	}
}

static void mppe_opt_init()
{
	ccp_option_register(&mppe_opt_hnd);
	triton_event_register_handler(EV_MPPE_KEYS, (triton_event_func)ev_mppe_keys);

	load_config();
	triton_event_register_handler(EV_CONFIG_RELOAD, (triton_event_func)load_config);
}

DEFINE_INIT(4, mppe_opt_init);
