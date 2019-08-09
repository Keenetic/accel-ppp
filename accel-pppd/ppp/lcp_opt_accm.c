#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include "linux_ppp.h"

#include "ppp.h"
#include "ppp_lcp.h"
#include "log.h"
#include "events.h"

#include "memdebug.h"

static int conf_accm = 0;

static struct lcp_option_t *accm_init(struct ppp_lcp_t *lcp);
static void accm_free(struct ppp_lcp_t *lcp, struct lcp_option_t *opt);
static int accm_send_conf_req(struct ppp_lcp_t *lcp, struct lcp_option_t *opt, uint8_t *ptr);
static int accm_recv_conf_req(struct ppp_lcp_t *lcp, struct lcp_option_t *opt, uint8_t *ptr);
static int accm_recv_conf_rej(struct ppp_lcp_t *lcp, struct lcp_option_t *opt, uint8_t *ptr);
static int accm_recv_conf_nak(struct ppp_lcp_t *lcp, struct lcp_option_t *opt, uint8_t *ptr);
static int accm_recv_conf_ack(struct ppp_lcp_t *lcp, struct lcp_option_t *opt, uint8_t *ptr);
static void accm_print(void (*print)(const char *fmt, ...), struct lcp_option_t *opt, uint8_t *ptr);
static int accm_apply_up(struct ppp_lcp_t *lcp, struct lcp_option_t *opt);

struct accm_option_t
{
	struct lcp_option_t opt;
	uint32_t accm;
	int enabled;
};

static struct lcp_option_handler_t accm_opt_hnd =
{
	.init = accm_init,
	.send_conf_req = accm_send_conf_req,
	.recv_conf_req = accm_recv_conf_req,
	.recv_conf_rej = accm_recv_conf_rej,
	.recv_conf_nak = accm_recv_conf_nak,
	.recv_conf_ack = accm_recv_conf_ack,
	.free = accm_free,
	.print = accm_print,
	.apply_up = accm_apply_up
};

static struct lcp_option_t *accm_init(struct ppp_lcp_t *lcp)
{
	struct accm_option_t *accm_opt = _malloc(sizeof(*accm_opt));

	memset(accm_opt, 0, sizeof(*accm_opt));
	accm_opt->opt.id = CI_ASYNCMAP;
	accm_opt->opt.len = 6;

	return &accm_opt->opt;
}

static void accm_free(struct ppp_lcp_t *lcp, struct lcp_option_t *opt)
{
	struct accm_option_t *accm_opt = container_of(opt, typeof(*accm_opt), opt);

	_free(accm_opt);
}

static int accm_send_conf_req(struct ppp_lcp_t *lcp, struct lcp_option_t *opt, uint8_t *ptr)
{
	return 0;
}

static int accm_apply(int fd, uint32_t accm, int ignore_io, int* err)
{
	if (net->ppp_ioctl(fd, PPPIOCSRASYNCMAP, (caddr_t) &accm) ||
		net->ppp_ioctl(fd, PPPIOCSASYNCMAP, (caddr_t) &accm)) {
		*err = errno;

		if (ignore_io && (*err == EIO || *err == ENOTTY))
			return 0;

		return -1;
	}

	return 0;
}

static int accm_apply_up(struct ppp_lcp_t *lcp, struct lcp_option_t *opt)
{
	struct accm_option_t *accm_opt = container_of(opt, typeof(*accm_opt), opt);
	int err = 0;

	if (accm_opt->enabled) {
		log_ppp_info2("lcp: accm: use RX/TX %08x map\n", accm_opt->accm);

		if (accm_opt->accm != 0xffffffff && accm_opt->accm != 0)
			log_ppp_warn("lcp: accm: strange ACCM map: %08x\n", accm_opt->accm);

		if (accm_apply(lcp->ppp->unit_fd, accm_opt->accm, 1, &err) ||
			accm_apply(lcp->ppp->chan_fd, accm_opt->accm, 1, &err)) {

			log_ppp_error("lcp:accm: failed to set ACCM: %s\n", strerror(err));
			return -1;
		}
	} else
	{
		log_ppp_info2("lcp: accm: disabled\n");
	}

	return 0;
}

static int accm_recv_conf_req(struct ppp_lcp_t *lcp, struct lcp_option_t *opt, uint8_t *ptr)
{
	struct accm_option_t *accm_opt = container_of(opt, typeof(*accm_opt), opt);
	struct lcp_opt32_t *opt32 = (struct lcp_opt32_t *)ptr;

	if (opt32->hdr.len != 6)
		return LCP_OPT_REJ;

	if (!conf_accm)
		return LCP_OPT_REJ;

	accm_opt->accm = ntohl(opt32->val);
	accm_opt->enabled = 1;

	return LCP_OPT_ACK;
}

static int accm_recv_conf_rej(struct ppp_lcp_t *lcp, struct lcp_option_t *opt, uint8_t *ptr)
{
	struct accm_option_t *accm_opt = container_of(opt, typeof(*accm_opt), opt);

	if (accm_opt->enabled)
		accm_opt->enabled = 0;

	return 0;
}

static int accm_recv_conf_nak(struct ppp_lcp_t *lcp, struct lcp_option_t *opt, uint8_t *ptr)
{
	struct accm_option_t *accm_opt = container_of(opt, typeof(*accm_opt), opt);
	struct lcp_opt32_t *opt32 = (struct lcp_opt32_t *)ptr;

	if (opt32->hdr.len != 6)
		return -1;

	if (!conf_accm)
		return -1;

	accm_opt->accm = ntohl(opt32->val);
	accm_opt->enabled = 1;

	return 0;
}

static int accm_recv_conf_ack(struct ppp_lcp_t *lcp, struct lcp_option_t *opt, uint8_t *ptr)
{
	struct accm_option_t *accm_opt = container_of(opt, typeof(*accm_opt), opt);
	struct lcp_opt32_t *opt32 = (struct lcp_opt32_t *)ptr;

	if (opt32->hdr.len != 6)
		return -1;

	if (!conf_accm)
		return -1;

	accm_opt->accm = ntohl(opt32->val);
	accm_opt->enabled = 1;

	return 0;
}

static void accm_print(void (*print)(const char *fmt, ...), struct lcp_option_t *opt, uint8_t *ptr)
{
	struct accm_option_t *accm_opt = container_of(opt, typeof(*accm_opt), opt);
	struct lcp_opt32_t *opt32 = (struct lcp_opt32_t *)ptr;

	if (ptr)
		print("<accm %08x>", ntohl(opt32->val));
	else
	if (accm_opt->enabled)
		print("<accm %08x>", accm_opt->accm);
}

static void load_config(void)
{
	char *opt;

	opt = conf_get_opt("ppp", "accm");
	if (opt) {
		if (!strcmp(opt, "deny"))
			conf_accm = 0;
		else if (!strcmp(opt, "allow"))
			conf_accm = 1;
	}
}

static void accm_opt_init()
{
	lcp_option_register(&accm_opt_hnd);

	load_config();
	triton_event_register_handler(EV_CONFIG_RELOAD, (triton_event_func)load_config);
}

DEFINE_INIT(4, accm_opt_init);
