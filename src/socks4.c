#include <stdlib.h>
#include <lwip/tcp.h>
#include <event2/event.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>

#include "container_of.h"
#include "socks.h"
#include "socks4.h"
#include "pipe.h"

#define SOCKS4_CMD_CONNECT	1
#define SOCKS4_CMD_BIND		2
#define SOCKS4_CMD_RESOLVE	240

#define SOCKS4_RESP_GRANT       90
#define SOCKS4_RESP_REJECT      91

struct socks4_hdr {
	u_char    version;
	u_char    cmd;
	u_int16_t port;
	u_int32_t addr;
} __attribute__((__packed__));

struct socks4_data {
	struct socks_data socks;
	u_char pos;
	u_char cmd;
};

static void
socks4_kill(struct socks_data *sdata)
{
	struct socks4_data *data;
	data = container_of(sdata, struct socks4_data, socks);
	free(data);
}

static void
socks4_response(struct socks_data *sdata, int code, int connected, int die)
{
	struct socks4_hdr hdr = {.version = 0, .cmd = code};
	struct socks4_data *data;
	data = container_of(sdata, struct socks4_data, socks);

	LWIP_DEBUGF(SOCKS_DEBUG, ("%s: %d%s\n", __func__, code,
							die ? " die" : ""));
	if (!die) {
		if (connected && data->cmd == SOCKS4_CMD_BIND) {
			hdr.port = htons(sdata->pcb->remote_port);
			hdr.addr = sdata->pcb->remote_ip.addr;
		} else {
			hdr.port = htons(sdata->pcb->local_port);
			hdr.addr = sdata->pcb->local_ip.addr;
		}
	} else {
		hdr.port = 0;
		hdr.addr = sdata->ipaddr.addr;
	}

	bufferevent_write(sdata->bev, &hdr, sizeof(hdr));

	if (die)
		socks_flush(sdata);
}

static void
socks4_connect_ok(struct socks_data *sdata)
{
	struct socks4_data *data;
	data = container_of(sdata, struct socks4_data, socks);

	socks4_response(sdata, SOCKS4_RESP_GRANT, 1, 0);

	pipe_join(sdata->pcb, sdata->bev);
	free(data);
}

static void
socks4_connect_failed(struct socks_data *sdata)
{
	socks4_response(sdata, SOCKS4_RESP_REJECT, 0, 1);
}

static void
socks4_connect(struct socks_data *sdata)
{
	struct socks4_data *data;
	data = container_of(sdata, struct socks4_data, socks);

	LWIP_DEBUGF(SOCKS_DEBUG, ("%s\n", __func__));
	if (data->cmd == SOCKS4_CMD_RESOLVE)
		socks4_response(sdata, SOCKS4_RESP_GRANT, 0, 1);

	else if (data->cmd == SOCKS4_CMD_CONNECT)
		socks_tcp_connect(sdata);

	else if (socks_tcp_bind(sdata) < 0)
		socks4_response(sdata, SOCKS4_RESP_REJECT, 0, 1);
	else {
		/*
		 * If the user sends any input data at this point, it is an
		 * error
		 */
		socks_request(sdata, 1, socks4_kill);
		socks4_response(sdata, SOCKS4_RESP_GRANT, 0, 0);
	}
}

static void
socks4_host_found(struct host_data *hdata)
{
	struct socks_data *sdata = container_of(hdata, struct socks_data, host);
	LWIP_DEBUGF(SOCKS_DEBUG, ("%s\n", __func__));
	sdata->ipaddr = hdata->ipaddr;
	socks4_connect(sdata);
}

static void
socks4_host_failed(struct host_data *hdata)
{
	struct socks_data *sdata = container_of(hdata, struct socks_data, host);
	LWIP_DEBUGF(SOCKS_DEBUG, ("%s\n", __func__));
	socks4_response(sdata, SOCKS4_RESP_REJECT, 0, 1);
}

static void
socks4_read_fqdn(struct socks_data *sdata)
{
	struct socks4_data *data;
	unsigned char ch;

	data = container_of(sdata, struct socks4_data, socks);

	while (bufferevent_read(sdata->bev, &ch, 1) > 0) {
		sdata->host.fqdn[data->pos] = ch;
		if (!ch) {
			bufferevent_disable(sdata->bev, EV_READ);
			host_lookup(&sdata->host);
			return;
		}
		data->pos++;
		if (data->pos == 255) {
			socks4_response(sdata, SOCKS4_RESP_REJECT, 0, 1);
			return;
		}
	}

	socks_request(sdata, 1, socks4_read_fqdn);
}

static void
socks4_read_user(struct socks_data *sdata)
{
	u_char ch;

	while (bufferevent_read(sdata->bev, &ch, 1) > 0 && ch);

	if (!ch) {
		unsigned long ip = ntohl(sdata->ipaddr.addr);
		LWIP_DEBUGF(SOCKS_DEBUG, ("%s\n", __func__));
		if (ip < 0x100 && ip)
			socks_request(sdata, 1, socks4_read_fqdn);
		else
			socks4_connect(sdata);
	} else
		socks_request(sdata, 1, socks4_read_user);
}

static void
socks4_read_hdr(struct socks_data *sdata)
{
	struct socks4_data *data;
	struct socks4_hdr hdr = {.version = 4};

	data = container_of(sdata, struct socks4_data, socks);

	bufferevent_read(sdata->bev, ((char *) &hdr) + 1, sizeof(hdr) - 1);

	LWIP_DEBUGF(SOCKS_DEBUG, ("%s: cmd %d\n", __func__, hdr.cmd));

	if (hdr.cmd != SOCKS4_CMD_CONNECT && hdr.cmd != SOCKS4_CMD_BIND &&
	    !(hdr.cmd == SOCKS4_CMD_RESOLVE && !hdr.port)) {
		socks4_response(sdata, SOCKS4_RESP_REJECT, 0, 1);
		return;
	}

	data->cmd = hdr.cmd;
	sdata->ipaddr.addr = hdr.addr;
	sdata->port = ntohs(hdr.port);

	socks_request(sdata, 1, socks4_read_user);
}

void
socks4_start(struct bufferevent *bev, int keep_alive)
{
	struct socks4_data *data;
	struct socks_data *sdata;
	data = calloc(1, sizeof(struct socks4_data));
	sdata = &data->socks;
	sdata->host.found = socks4_host_found;
	sdata->host.failed = socks4_host_failed;
	sdata->kill = socks4_kill;
	sdata->connect_ok = socks4_connect_ok;
	sdata->connect_failed = socks4_connect_failed;
	sdata->bev = bev;
	sdata->keep_alive = keep_alive;
	socks_request(sdata, sizeof(struct socks4_hdr) - 1,
							socks4_read_hdr);
}
