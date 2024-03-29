/*
 * tipc-config.c: TIPC configuration management tool
 * 
 * Copyright (c) 2004-2005, Ericsson Research Canada
 * Copyright (c) 2004-2006, Ericsson AB
 * Copyright (c) 2005-2008,2010-2011, Wind River Systems
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the names of the copyright holders nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <getopt.h>
#include <unistd.h>
#include <poll.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <linux/tipc.h>
#include <linux/tipc_config.h>
#include <linux/genetlink.h>
#include "build_info.h"

#define VERSION "2.0.2"
/* typedefs */

typedef void (*VOIDFUNCPTR) ();

/* constants */

#define MAX_COMMANDS 8
#define MAX_TLVS_SPACE 33000		/* must be a multiple of 4 bytes */

/* local variables */

static int verbose = 0;
static int interactive = 0;
static __u32 dest = 0;
static __u32 tlv_area[MAX_TLVS_SPACE / sizeof(__u32)];
static __u32 tlv_list_area[MAX_TLVS_SPACE / sizeof(__u32)];

/* forward declarations */

static char usage[];

/* macros */

#define cprintf(fmt, arg...)	do { if (verbose) printf(fmt, ##arg); } while (0)

#define fatal(fmt, arg...)	do { printf(fmt, ##arg); exit(EXIT_FAILURE); } while (0)

#define confirm(fmt, arg...) do { \
		char c; \
		if (interactive) { \
			printf(fmt, ##arg); \
			scanf(" %c", &c); /* leading blank skips whitespace */ \
			if ((c != '\n') && (c != 'Y') && (c != 'y')) { \
				printf("Exiting...\n"); \
				exit(EXIT_SUCCESS); \
			} \
		} \
	} while (0)

/* local variables */

static char *err_string[] = {
	"incorrect message format",
	"must be network administrator to perform operation",
	"must be zone master to perform operation",
	"remote management not enabled on destination node",
	"operation not supported",
	"invalid argument"
};

/******************************************************************************
 *
 * Utility routines used in executing command options
 *
 */

static inline int delimit(int val, int min, int max)
{
	if (val > max)
		return max;
	if (val < min)
		return min;
	return val;
}

static __u32 own_node(void)
{
	struct sockaddr_tipc addr;
	socklen_t sz = sizeof(addr);
	int sd;

	sd = socket(AF_TIPC, SOCK_RDM, 0);
	if (sd < 0)
		fatal("TIPC module not installed\n");
	if (getsockname(sd, (struct sockaddr *)&addr, &sz) < 0)
		fatal("failed to get TIPC socket address\n");
	close(sd);
	return addr.addr.id.node;
}

static const char *addr2str(__u32 addr)
{
	static char addr_area[4][16];	/* allow up to 4 uses in one printf() */
	static int addr_crs = 0;

	addr_crs = (addr_crs + 1) & 3;
	sprintf(&addr_area[addr_crs][0], "<%u.%u.%u>",
		tipc_zone(addr), tipc_cluster(addr), tipc_node(addr));
	return &addr_area[addr_crs][0];
}

static const char *for_dest(void)
{
	static char addr_area[30];

	if (dest == own_node())
		return "";
	sprintf(addr_area, " for node %s", addr2str(dest));
	return addr_area;
}

static const char *for_domain(const char *string, __u32 domain)
{
	static char addr_area[30];

	if (domain == 0)
		return "";
	sprintf(addr_area, "%s%s", string, addr2str(domain));
	return addr_area;
}

static void print_title(const char *main_title, const char *extra_title)
{
	printf(main_title, for_dest(), extra_title);
}

static void print_title_opt(const char *main_title, const char *extra_title)
{
	if ((dest == own_node()) && (extra_title[0] == '\0'))
		return;

	printf(main_title, for_dest(), extra_title);
}

char *get_arg(char **args)
{
	char *ret;
	char *comma;

	ret = *args;
	comma = strchr(ret, ',');
	if (comma) {
		*comma = '\0';
		*args = comma + 1;
	}
	else
		*args = NULL;
	return ret;
}

static __u32 str2addr(char *str)
{
	uint z, c, n;
	char dummy;

	if (sscanf(str, "%u.%u.%u%c", &z, &c, &n, &dummy) != 3)
		fatal("invalid network address, use syntax: Z.C.N\n");
	if ((z != delimit(z, 0, 255)) || 
	    (c != delimit(c, 0, 4095)) ||
	    (n != delimit(n, 0, 4095)))
		fatal("network address field value(s) too large\n");
	return tipc_addr(z, c, n);
}


/******************************************************************************
 *
 * Routines used to exchange messages over Netlink sockets
 *
 */

#define NLA_SIZE(type)	(NLA_HDRLEN + NLA_ALIGN(sizeof(type)))

#define nla_for_each_attr(pos, head, len, rem) \
	for (pos = head, rem = len; nla_ok(pos, rem); pos = nla_next(pos, &(rem)))

static inline void *nla_data(const struct nlattr *nla)
{
	return (char *) nla + NLA_HDRLEN;
}

static inline int nla_ok(const struct nlattr *nla, int remaining)
{
	return remaining >= sizeof(*nla) &&
		nla->nla_len >= sizeof(*nla) &&
		nla->nla_len <= remaining;
}

static inline struct nlattr *nla_next(const struct nlattr *nla, int *remaining)
{
        int totlen = NLA_ALIGN(nla->nla_len);

        *remaining -= totlen;
        return (struct nlattr *) ((char *) nla + totlen);
}

static inline int nla_put_string(struct nlattr *nla, int type, const char *str)
{
	int attrlen = strlen(str) + 1;

	nla->nla_len = NLA_HDRLEN + attrlen;
	nla->nla_type = type;
	memcpy(nla_data(nla), str, attrlen);

	return NLA_HDRLEN + NLA_ALIGN(attrlen);
}

static inline __u16 nla_get_u16(struct nlattr *nla)
{
	return *(__u16 *) nla_data(nla);
}

static int write_uninterrupted(int sk, const char *buf, int len)
{
	int c;

	while ((c = write(sk, buf, len)) < len) {
		if (c == -1) {
			if (errno == EINTR)
				continue;
			return -1;
		}

		buf += c;
		len -= c;
	}

	return 0;
}

static int genetlink_call(__u16 family_id, __u8 cmd, void *header, 
		size_t header_len, void *request, size_t request_len, 
		void *reply, size_t reply_len)
{
	struct msg {
		struct nlmsghdr n;
		struct genlmsghdr g;
		char payload[0];
	};
	
	struct msg *request_msg;
	struct msg *reply_msg;
	int request_msg_size;
	int reply_msg_size;

	struct sockaddr_nl local;
	struct pollfd pfd;
	int sndbuf = 32*1024; /* 32k */
	int rcvbuf = 32*1024; /* 32k */
	int len;
	int sk;

	/*
	 * Prepare request/reply messages
	 */
	request_msg_size = NLMSG_LENGTH(GENL_HDRLEN + header_len + request_len);
	request_msg = malloc(request_msg_size);
	if(!request_msg){
		fatal("error alloc memory for request_msg\n");
	}
	request_msg->n.nlmsg_len = request_msg_size;
	request_msg->n.nlmsg_type = family_id;
	request_msg->n.nlmsg_flags = NLM_F_REQUEST;
	request_msg->n.nlmsg_seq = 0;
	request_msg->n.nlmsg_pid = getpid();
	request_msg->g.cmd = cmd;
	request_msg->g.version = 0;
	if (header_len)
		memcpy(&request_msg->payload[0], header, header_len);
	if (request_len)
		memcpy(&request_msg->payload[header_len], request, request_len);

	reply_msg_size = NLMSG_LENGTH(GENL_HDRLEN + header_len + reply_len);
	reply_msg = malloc(reply_msg_size);
	if(!reply_msg){
		fatal("error alloc memory for reply_msg\n");
	}

	/*
	 * Create socket
	 */
	memset(&local, 0, sizeof(local));
	local.nl_family = AF_NETLINK;

	if ((sk = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_GENERIC)) == -1)
		fatal("error creating Netlink socket\n");

	if ((bind(sk, (struct sockaddr*)&local, sizeof(local)) == -1) ||
	    (setsockopt(sk, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) == -1) ||
	    (setsockopt(sk, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) == -1)) {
		fatal("error creating Netlink socket\n");
	}

	/*
	 * Send request
	 */
	if (write_uninterrupted(sk, (char*)request_msg, request_msg_size) < 0)
		fatal("error sending message via Netlink\n");

	/*
	 * Wait for reply
	 */
	pfd.fd = sk;
	pfd.events = ~POLLOUT;
	if ((poll(&pfd, 1, 3000) != 1) || !(pfd.revents & POLLIN))
		fatal("no reply detected from Netlink\n");

	/*
	 * Read reply
	 */
	len = recv(sk, (char*)reply_msg, reply_msg_size, 0);
	if (len < 0)
		fatal("error receiving reply message via Netlink\n");

	close(sk);

	/*
	 * Validate response
	 */
	if (!NLMSG_OK(&reply_msg->n, len))
		fatal("invalid reply message received via Netlink\n");

	if (reply_msg->n.nlmsg_type == NLMSG_ERROR) {
		len = -1;
		goto out;
	}

	if ((request_msg->n.nlmsg_type != reply_msg->n.nlmsg_type) ||
	    (request_msg->n.nlmsg_seq != reply_msg->n.nlmsg_seq))
		fatal("unexpected message received via Netlink\n");

	/*
	 * Copy reply header
	 */
	len -= NLMSG_LENGTH(GENL_HDRLEN);
	if (len < header_len)
		fatal("too small reply message received via Netlink\n");
	if (header_len > 0)
		memcpy(header, &reply_msg->payload[0], header_len);

	/*
	 * Copy reply payload
	 */
	len -= header_len;
	if (len > reply_len)
		fatal("reply message too large to copy\n");
	if (len > 0)
		memcpy(reply, &reply_msg->payload[header_len], len);

 out:
	free(request_msg);
	free(reply_msg);

	return len;
}

static int get_genl_family_id(const char* name)
{
	struct nlattr_family_name {
		char value[GENL_NAMSIZ];
	};

	struct nlattr_family_id {
		__u16 value;
	};

	/*
	 * Create request/reply buffers
	 *
	 * Note that the reply buffer is larger than necessary in case future
	 * versions of Netlink return additional protocol family attributes
	 */
	char request[NLA_SIZE(struct nlattr_family_name)];
	int request_len = nla_put_string((struct nlattr *)request, CTRL_ATTR_FAMILY_NAME, name);

	char reply[256];
	int reply_len = sizeof(reply);

	/*
	 * Call control service
	 */
	int len = genetlink_call(GENL_ID_CTRL, CTRL_CMD_GETFAMILY,
				 0, 0,
				 request, request_len,
				 reply, reply_len);
	
	if (len == -1)
		return -1;

	/*
	 * Parse reply
	 */
        struct nlattr *head = (struct nlattr *) reply;
        struct nlattr *nla;
        int rem;

        nla_for_each_attr(nla, head, len, rem) {
                if (nla->nla_type == CTRL_ATTR_FAMILY_ID)
			return nla_get_u16(nla);
        }

        if (rem > 0)
                fatal("%d bytes leftover after parsing Netlink attributes\n", rem);

	return -1;
}

static int do_command_netlink(__u16 cmd, void *req_tlv, __u32 req_tlv_space,
			      void *rep_tlv, __u32 rep_tlv_space)
{
	struct tipc_genlmsghdr header;
	int family_id;
	int len;

	/*
	 * Request header
	 */
	header.dest = dest;
	header.cmd = cmd;

	/*
	 * Get TIPC family id
	 */
	if ((family_id = get_genl_family_id(TIPC_GENL_NAME)) == -1)
		fatal("no Netlink service registered for %s\n", TIPC_GENL_NAME);

	/*
	 * Call control service
	 */
	len = genetlink_call(family_id, TIPC_GENL_CMD,
			     &header, sizeof(header),
			     req_tlv, req_tlv_space,
			     rep_tlv, rep_tlv_space);

	return len;
}

/******************************************************************************
 *
 * Routines used to exchange messages over TIPC sockets
 *
 */

static int do_command_tipc(__u16 cmd, void *req_tlv, __u32 req_tlv_space,
			   void *rep_tlv, __u32 rep_tlv_space)
{
	struct {
		struct tipc_cfg_msg_hdr hdr;
		char buf[MAX_TLVS_SPACE];
	} req, ans;
	int msg_space;
	int tsd;
	struct sockaddr_tipc tipc_dest;
	int imp = TIPC_CRITICAL_IMPORTANCE;
	struct pollfd pfd;
	int pollres;

	if ((tsd = socket(AF_TIPC, SOCK_RDM, 0)) < 0)
		fatal("TIPC module not installed\n");

	msg_space = TCM_SET(&req.hdr, cmd, TCM_F_REQUEST, 
			    req_tlv, req_tlv_space);

	setsockopt(tsd, SOL_TIPC, TIPC_IMPORTANCE, &imp, sizeof(imp));

	tipc_dest.family = AF_TIPC;
	tipc_dest.addrtype = TIPC_ADDR_NAME;
	tipc_dest.addr.name.name.type = TIPC_CFG_SRV;
	tipc_dest.addr.name.name.instance = dest;
	tipc_dest.addr.name.domain = dest;

	if (sendto(tsd, &req, msg_space, 0,
		   (struct sockaddr *)&tipc_dest, sizeof(tipc_dest)) < 0)
		fatal("unable to send command to node %s\n", addr2str(dest));

	/* Wait for response message */

	pfd.events = 0xffff & ~POLLOUT;
	pfd.fd = tsd;
	pollres = poll(&pfd, 1, 3000);
	if ((pollres < 0) || !(pfd.revents & POLLIN))
		fatal("no reply detected from TIPC\n");
	msg_space = recv(tsd, &ans, sizeof(ans), 0);
	if (msg_space < 0)
		fatal("error receiving reply message via TIPC\n");

	/* Validate response message */

	if ((msg_space < TCM_SPACE(0)) || (ntohl(ans.hdr.tcm_len) > msg_space))
		fatal("invalid reply message received via TIPC\n");
	if ((ntohs(ans.hdr.tcm_type) != cmd) || 
	    (ntohs(ans.hdr.tcm_flags) != 0))
		fatal("unexpected message received via TIPC\n");

	msg_space = ntohl(ans.hdr.tcm_len) - TCM_SPACE(0);
	if (msg_space > rep_tlv_space)
		fatal("reply message too large to copy\n");
	memcpy(rep_tlv, ans.buf, msg_space);
	close(tsd);
	return msg_space;
}


/******************************************************************************
 *
 * Routines used to process commands requested by user
 *
 */

static __u32 do_command(__u16 cmd, void *req_tlv, __u32 req_tlv_space,
			void *rep_tlv, __u32 rep_tlv_space)
{
	int rep_len;

	if (dest == own_node())
		rep_len = do_command_netlink(cmd, req_tlv, req_tlv_space, 
					     rep_tlv, rep_tlv_space);
	else
		rep_len	= do_command_tipc(cmd, req_tlv, req_tlv_space, 
					  rep_tlv, rep_tlv_space);

	if (TLV_CHECK(rep_tlv, rep_len, TIPC_TLV_ERROR_STRING)) {
		char *c = (char *)TLV_DATA(rep_tlv);
		char code = *c;
		char max_code = sizeof(err_string)/sizeof(err_string[0]);

		if (code & 0x80) {
			code &= 0x7F;
			printf((code < max_code) 
			       ? err_string[(int)code] : "unknown error");
			c++;
		}
		fatal("%s\n", c);
	}

	return rep_len;
}

static __u32 do_get_unsigned(__u16 cmd)
{
	int tlv_space;
	__u32 value;

	tlv_space = do_command(cmd, NULL, 0, tlv_area, sizeof(tlv_area));

	if (!TLV_CHECK(tlv_area, tlv_space, TIPC_TLV_UNSIGNED))
		fatal("corrupted reply message\n");

	value = *(__u32 *)TLV_DATA(tlv_area);
	return ntohl(value);
}

static void do_set_unsigned(char *args, __u16 cmd, char *attr_name, 
			    char *attr_warn)
{
	__u32 attr_val;
	__u32 attr_val_net;
	int tlv_space;
	char dummy;

	if (sscanf(args, "%u%c", &attr_val, &dummy) != 1)
		fatal("invalid numeric argument for %s\n", attr_name);

	confirm("set %s to %u%s?%s [Y/n]\n", attr_name, attr_val, 
		for_dest(), attr_warn);

	attr_val_net = htonl(attr_val);
	tlv_space = TLV_SET(tlv_area, TIPC_TLV_UNSIGNED, 
			    &attr_val_net, sizeof(attr_val_net));
	do_command(cmd, tlv_area, tlv_space, tlv_area, sizeof(tlv_area));

	cprintf("%s%s now set to %u\n", attr_name, for_dest(), attr_val);
}

static void set_node_addr(char *args)
{
	__u32 new_addr;
	__u32 new_addr_net;
	int tlv_space;

	if (!*args) {
		do_command(TIPC_CMD_NOOP, NULL, 0, tlv_area, sizeof(tlv_area));
		printf("node address: %s\n", addr2str(dest));
		return;
	}

	new_addr = str2addr(args);

	confirm("change node address%s to %s? "
		"(this will delete all links) [Y/n]\n", 
		for_dest(), addr2str(new_addr));

	new_addr_net = htonl(new_addr);
	tlv_space = TLV_SET(tlv_area, TIPC_TLV_NET_ADDR, 
			    &new_addr_net, sizeof(new_addr_net));
	do_command(TIPC_CMD_SET_NODE_ADDR, tlv_area, tlv_space, 
		   tlv_area, sizeof(tlv_area));

	cprintf("node address%s now set to %s\n", 
		for_dest(), addr2str(new_addr));
	dest = new_addr;
}

static void set_remote_mng(char *args)
{
	__u32 attr_val;
	__u32 attr_val_net;
	int tlv_space;

	if (!*args) {
		printf("remote management%s: %s\n", for_dest(),
		       do_get_unsigned(TIPC_CMD_GET_REMOTE_MNG) ? 
		       "enabled" : "disabled");
		return;
	}

	if (!strcmp(args, "enable"))
		attr_val = 1;
	else if (!strcmp(args, "disable"))
		attr_val = 0;
	else
		fatal("invalid argument for remote management\n");

	confirm("%s remote management%s? [Y/n]\n", 
		attr_val ? "enable" : "disable", for_dest());

	attr_val_net = htonl(attr_val);
	tlv_space = TLV_SET(tlv_area, TIPC_TLV_UNSIGNED, 
			    &attr_val_net, sizeof(attr_val_net));
	do_command(TIPC_CMD_SET_REMOTE_MNG, tlv_area, tlv_space, 
		   tlv_area, sizeof(tlv_area));

	cprintf("remote management%s %s\n", for_dest(),
		attr_val ? "enabled" : "disabled");
}

static void set_max_ports(char *args)
{
	if (!*args)
		printf("maximum allowed ports%s: %u\n", for_dest(),
		       do_get_unsigned(TIPC_CMD_GET_MAX_PORTS));
	else
		do_set_unsigned(args, TIPC_CMD_SET_MAX_PORTS,
				"max ports", "");
}

static void set_max_publ(char *args)
{
	if (!*args)
		printf("maximum allowed publications%s: %u\n", for_dest(),
		       do_get_unsigned(TIPC_CMD_GET_MAX_PUBL));
	else
		do_set_unsigned(args, TIPC_CMD_SET_MAX_PUBL, 
				"max publications", "");
}

static void set_max_subscr(char *args)
{
	if (!*args)
		printf("maximum allowed subscriptions%s: %u\n", for_dest(),
		       do_get_unsigned(TIPC_CMD_GET_MAX_SUBSCR));
	else
		do_set_unsigned(args, TIPC_CMD_SET_MAX_SUBSCR, 
				"max subscriptions", "");
}

static void set_netid(char *args)
{
	if (!*args)
		printf("current network id%s: %u\n", for_dest(),
		       do_get_unsigned(TIPC_CMD_GET_NETID));
	else
		do_set_unsigned(args, TIPC_CMD_SET_NETID,
				"network identity", "");
}

static void get_nodes(char *args)
{
	int tlv_space;
	__u32 domain;
	__u32 domain_net;
	struct tlv_list_desc tlv_list;
	struct tipc_node_info *node_info;

	domain = (*args != 0) ? str2addr(args) : 0;
	domain_net = htonl(domain);
	tlv_space = TLV_SET(tlv_area, TIPC_TLV_NET_ADDR,
			    &domain_net, sizeof(domain_net));
	tlv_space = do_command(TIPC_CMD_GET_NODES, tlv_area, tlv_space,
			       tlv_area, sizeof(tlv_area));

	print_title("Neighbors%s%s:\n", for_domain(" within domain ", domain));
	if (!tlv_space) {
		printf("No nodes found\n");
		return;
	}

	TLV_LIST_INIT(&tlv_list, tlv_area, tlv_space);
	while (!TLV_LIST_EMPTY(&tlv_list)) {
		if (!TLV_LIST_CHECK(&tlv_list, TIPC_TLV_NODE_INFO))
			fatal("corrupted reply message\n");
		node_info = (struct tipc_node_info *)TLV_LIST_DATA(&tlv_list);
		printf("%s: %s\n", addr2str(ntohl(node_info->addr)),
		       ntohl(node_info->up) ? "up" : "down");
		TLV_LIST_STEP(&tlv_list);
	}
}

/**
 * do_these_links - perform operation on specified set of links 
 * @funcToRun: operation to be performed on link
 * @domain: network domain of interest (0.0.0 if not used)
 * @str: link name pattern of interest (NULL if not used)
 * @vname: name of the parameter being set (optional arg to 'funcToRun')
 * @cmd: command to execute (optional arg to 'funcToRun')
 * @val: new value to be set (optional arg to 'funcToRun')
 *
 * This routine first retrieves the names of all links in the specified 
 * network domain, eliminates those that don't match the specified search
 * pattern, and then performs the requestion operation on each remaining link.
 */

static void do_these_links(VOIDFUNCPTR funcToRun, __u32 domain, const char *str,
			   const char *vname, int cmd, int val)
{
	int tlv_space;
	int numLinks = 0;
	__u32 domain_net;
	struct tlv_list_desc tlv_list;
	struct tipc_link_info *local_link_info;

	domain_net = htonl(domain);
	tlv_space = TLV_SET(tlv_list_area, TIPC_TLV_NET_ADDR,
			    &domain_net, sizeof(domain_net));
	tlv_space = do_command(TIPC_CMD_GET_LINKS, tlv_list_area, tlv_space,
			       tlv_list_area, sizeof(tlv_list_area));

	TLV_LIST_INIT(&tlv_list, tlv_list_area, tlv_space);

	while (!TLV_LIST_EMPTY(&tlv_list)) {
		if (!TLV_LIST_CHECK(&tlv_list, TIPC_TLV_LINK_INFO))
			fatal("corrupted reply message\n");
		local_link_info = (struct tipc_link_info *)TLV_LIST_DATA(&tlv_list);
		if ((str == NULL) ||
		    (strstr(local_link_info->str, str) != NULL)) {
			funcToRun(local_link_info->str, local_link_info->up, 
				  vname, cmd, val);
			numLinks++;
		}
		TLV_LIST_STEP(&tlv_list);
	}

	if (numLinks == 0) {
		if (str == NULL)
			printf("No links found\n");
		else
			printf("No links found matching pattern '%s'\n", str);
	}
}

static void get_link(char *linkName, __u32 up)
{
	printf("%s: %s\n", linkName, ntohl(up) ? "up" : "down");
}

static void get_linkset(char *args)
{
	char *strp = NULL;			/* list all links by default */
	__u32 domain = 0;

	if (*args != 0) {
		if (args[0] == '?')
			strp = args + 1;   	/* list links matching pattern */
		else
			domain = str2addr(args);/* list links in domain */
	}

	print_title("Links%s%s:\n", for_domain(" within domain ", domain));

	do_these_links(get_link, domain, strp, "", 0, 0);
}

static void show_link_stats(char *linkName)
{
	int tlv_space;

	tlv_space = TLV_SET(tlv_area, TIPC_TLV_LINK_NAME, 
			    linkName, TIPC_MAX_LINK_NAME);
	tlv_space = do_command(TIPC_CMD_SHOW_LINK_STATS, tlv_area, tlv_space,
			       tlv_area, sizeof(tlv_area));

	if (!TLV_CHECK(tlv_area, tlv_space, TIPC_TLV_ULTRA_STRING))
		fatal("corrupted reply message\n");

	printf("%s\n", (char *)TLV_DATA(tlv_area));
}

static void show_linkset_stats(char *args)
{
	print_title("Link statistics%s:\n", NULL);

	if (*args == 0)			/* show for all links */
		do_these_links(show_link_stats, 0, NULL, NULL, 0, 0);
	else if (args[0] == '?') 	/* show for all links matching pattern */
		do_these_links(show_link_stats, 0, args+1, NULL, 0, 0);
	else	 			/* show for specified link */
		show_link_stats(args);
}

static void reset_link_stats(char *linkName)
{
	int tlv_space;

	tlv_space = TLV_SET(tlv_area, TIPC_TLV_LINK_NAME, 
			    linkName, TIPC_MAX_LINK_NAME);
	tlv_space = do_command(TIPC_CMD_RESET_LINK_STATS, tlv_area, tlv_space,
			       tlv_area, sizeof(tlv_area));

	cprintf("Link %s statistics reset\n", linkName);
}

static void reset_linkset_stats(char *args)
{
	if (args[0] == '?')
		do_these_links(reset_link_stats, 0, args+1, NULL, 0, 0);
	else
		reset_link_stats(args);
}

static void show_name_table(char *args)
{
	int tlv_space;
	__u32 depth;
	__u32 type;
	__u32 lowbound;
	__u32 upbound;
	char dummy;
	struct tipc_name_table_query query_info;

	/* process (optional) depth argument */

	if (!*args)
		depth = 0;
	else if (args[0] == 'a')
		depth = 4;
	else if (args[0] == 'p')
	      depth = 3;
	else if (args[0] == 'n')
	      depth = 2;
	else if (args[0] == 't')
		depth = 1;
	else
		depth = 0;

	if (depth > 0) {
		args += strcspn(args, ",");
		if (*args)
			args++;   /* skip over comma */
	} else {
		depth = 4;
	}

	/* process (optional) type arguments */

	if (!*args) {
		depth |= TIPC_NTQ_ALLTYPES;
		type = lowbound = upbound = 0;
	} else if (sscanf(args, "%u,%u,%u%c", &type, &lowbound, &upbound,
			  &dummy) == 3) {
		/* do nothing more */
	} else if (sscanf(args, "%u,%u%c", &type, &lowbound, &dummy) == 2) {
		upbound = lowbound;
	} else if (sscanf(args, "%u%c", &type, &dummy) == 1) {
		lowbound = 0;
		upbound = ~0;
	} else
		fatal(usage);

	/* issue query & process response */

	query_info.depth = htonl(depth);
	query_info.type = htonl(type);
	query_info.lowbound = htonl(lowbound);
	query_info.upbound = htonl(upbound);

	tlv_space = TLV_SET(tlv_area, TIPC_TLV_NAME_TBL_QUERY, 
			    &query_info, sizeof(query_info));
	tlv_space = do_command(TIPC_CMD_SHOW_NAME_TABLE, tlv_area, tlv_space,
			       tlv_area, sizeof(tlv_area));

	if (!TLV_CHECK(tlv_area, tlv_space, TIPC_TLV_ULTRA_STRING))
		fatal("corrupted reply message\n");

	print_title_opt("Names%s:\n", "");
	printf("%s", (char *)TLV_DATA(tlv_area));
}

static void get_media(char *dummy)
{
	int tlv_space;
	struct tlv_list_desc tlv_list;

	tlv_space = do_command(TIPC_CMD_GET_MEDIA_NAMES, NULL, 0,
			       tlv_area, sizeof(tlv_area));

	print_title("Media%s:\n", NULL);
	if (!tlv_space) {
		printf("No registered media\n");
		return;
	}

	TLV_LIST_INIT(&tlv_list, tlv_area, tlv_space);
	while (!TLV_LIST_EMPTY(&tlv_list)) {
		if (!TLV_LIST_CHECK(&tlv_list, TIPC_TLV_MEDIA_NAME))
			fatal("corrupted reply message\n");
		printf("%s\n", (char *)TLV_LIST_DATA(&tlv_list));
		TLV_LIST_STEP(&tlv_list);
	}
}


/**
 * do_these_bearers - perform operation on specified set of bearers 
 * @funcToRun: operation to be performed on bearer
 * @str: bearer name pattern (if NULL, do operation on all bearers)
 */

static void do_these_bearers(VOIDFUNCPTR funcToRun, const char *str)
{
	int numBearers = 0;
	int tlv_space;
	struct tlv_list_desc tlv_list;
	char *bname;

	tlv_space = do_command(TIPC_CMD_GET_BEARER_NAMES, NULL, 0,
			       tlv_list_area, sizeof(tlv_list_area));

	TLV_LIST_INIT(&tlv_list, tlv_list_area, tlv_space);

	while (!TLV_LIST_EMPTY(&tlv_list)) {
		if (!TLV_LIST_CHECK(&tlv_list, TIPC_TLV_BEARER_NAME))
			fatal("corrupted reply message\n");
		bname = (char *)TLV_LIST_DATA(&tlv_list);
		if ((str == NULL) || (strstr(bname, str) != NULL)) {
			funcToRun(bname);
			numBearers++;
		}
		TLV_LIST_STEP(&tlv_list);
	}

	if (numBearers == 0) {
		if (str == NULL)
			printf("No active bearers\n");
		else
			printf("No bearers found matching pattern '%s'\n", str);
	}
}

static void get_bearer(char *bname)
{
	printf("%s\n", bname);
}

static void get_bearerset(char *args)
{
	print_title("Bearers%s:\n", NULL);

	if (*args == 0)
		do_these_bearers(get_bearer, NULL);	/* list all bearers */
	else if (args[0] == '?')
		do_these_bearers(get_bearer, args+1);	/* list matching ones */
	else
		fatal("Invalid argument '%s' \n", args);
}

static void show_ports(char *dummy)
{
	int tlv_space;

	tlv_space = do_command(TIPC_CMD_SHOW_PORTS, NULL, 0,
			       tlv_area, sizeof(tlv_area));

	if (!TLV_CHECK(tlv_area, tlv_space, TIPC_TLV_ULTRA_STRING))
		fatal("corrupted reply message\n");

	print_title("Ports%s:\n", NULL);
	printf("%s", (char *)TLV_DATA(tlv_area));
}

static void set_log_size(char *args)
{
	int tlv_space;

	if (!*args) {
		tlv_space = do_command(TIPC_CMD_DUMP_LOG, NULL, 0,
				       tlv_area, sizeof(tlv_area));

		if (!TLV_CHECK(tlv_area, tlv_space, TIPC_TLV_ULTRA_STRING))
			fatal("corrupted reply message\n");

		printf("Log dump%s:\n%s", for_dest(), (char *)TLV_DATA(tlv_area));
	} else {
		do_set_unsigned(args, TIPC_CMD_SET_LOG_SIZE, "log size",
				" (this will discard current log contents)");
	}
}

static void show_stats(char *args)
{
	__u32 attr_val_net;
	int tlv_space;

	/*
	 * In future, may allow user to control what info is returned;
	 * for now, just hard code 0 as command argument to get default info
	 */

	attr_val_net = htonl(0);
	tlv_space = TLV_SET(tlv_area, TIPC_TLV_UNSIGNED, 
			    &attr_val_net, sizeof(attr_val_net));

	tlv_space = do_command(TIPC_CMD_SHOW_STATS, tlv_area, tlv_space,
			       tlv_area, sizeof(tlv_area));

	if (!TLV_CHECK(tlv_area, tlv_space, TIPC_TLV_ULTRA_STRING))
		fatal("corrupted reply message\n");

	print_title_opt("Status%s:\n", "");
	printf("%s", (char *)TLV_DATA(tlv_area));
}

static void set_link_value(char *linkName, __u32 dummy, const char *vname,
			   int cmd, int val)
{
	struct tipc_link_config req_tlv;
	int tlv_space;

	req_tlv.value = htonl(val);
	strcpy(req_tlv.name, linkName);
	req_tlv.name[TIPC_MAX_LINK_NAME - 1] = '\0';

	confirm("Change %s of link <%s>%s to %u? [Y/n]\n",
		vname, req_tlv.name, for_dest(), val);

	tlv_space = TLV_SET(tlv_area, TIPC_TLV_LINK_CONFIG,
			    &req_tlv, sizeof(req_tlv));
	tlv_space = do_command(cmd, tlv_area, tlv_space,
			       tlv_area, sizeof(tlv_area));

	cprintf("Link <%s>%s changed %s to %u\n", 
		req_tlv.name, for_dest(), vname, val);
}

static void set_linkset_value(char *args, const char *vname, int cmd)
{
	int  val;
	char dummy;
	char *s = strchr(args, '/');

	if (!s)
		fatal("Syntax: tipcConfig -l%c=<link-name>|<pattern>/<%s>\n",
		      vname[0], vname);

	*s++ = 0;

	if (sscanf(s, "%u%c", &val, &dummy) != 1)
		fatal("non-numeric link %s specified\n", vname);

	if (args[0] == '?')
		do_these_links(set_link_value, 0, args+1, vname, cmd, val);
	else
		set_link_value(args, 0, vname, cmd, val);
}

static void set_linkset_tolerance(char *args)
{
	set_linkset_value(args, "tolerance", TIPC_CMD_SET_LINK_TOL);
}

static void set_linkset_priority(char *args)
{
	set_linkset_value(args, "priority", TIPC_CMD_SET_LINK_PRI);
}

static void set_linkset_window(char *args)
{
	set_linkset_value(args, "window", TIPC_CMD_SET_LINK_WINDOW);
}


static void enable_bearer(char *args)
{
	struct tipc_bearer_config req_tlv;
	int tlv_space;
	char *a;
	char dummy;

	while (args) {
		__u32 domain = dest & 0xfffff000; /* defaults to own cluster */
		uint pri = TIPC_MEDIA_LINK_PRI; /* defaults to media priority */
		char *domain_str, *pri_str;

		a = get_arg(&args);
		if ((domain_str = strchr(a, '/'))) {
			*domain_str++ = 0;
			if ((pri_str = strchr(domain_str, '/'))) {
				*pri_str++ = 0;
				if ((*pri_str != 0) &&
				    sscanf(pri_str, "%u%c", &pri, &dummy) != 1)
					fatal("non-numeric bearer priority specified\n");
			}
			if (*domain_str != 0)
				domain = str2addr(domain_str);
		}

		confirm("Enable bearer <%s>%s with detection domain %s and "
			"priority %u? [Y/n]",
			a, for_dest(), addr2str(domain), pri);

		req_tlv.priority = htonl(pri);
#if (!(defined INHAND_IR8 || defined INHAND_IP812))
		req_tlv.disc_domain = htonl(domain);
#else
		req_tlv.detect_scope = htonl(domain);
#endif
		strncpy(req_tlv.name, a, TIPC_MAX_BEARER_NAME - 1);
		req_tlv.name[TIPC_MAX_BEARER_NAME - 1] = '\0';

		tlv_space = TLV_SET(tlv_area, TIPC_TLV_BEARER_CONFIG, 
				    &req_tlv, sizeof(req_tlv));
		tlv_space = do_command(TIPC_CMD_ENABLE_BEARER, tlv_area, tlv_space,
				       tlv_area, sizeof(tlv_area));

		cprintf("Bearer <%s> enabled%s\n", a, for_dest());
	}
}

static void disable_bearer(char *bname)
{
	char bearer_name[TIPC_MAX_BEARER_NAME];
	int tlv_space;

	strncpy(bearer_name, bname, TIPC_MAX_BEARER_NAME - 1);
	bearer_name[TIPC_MAX_BEARER_NAME - 1] = '\0';

	confirm("Disable bearer <%s>%s ? [Y/n]", bearer_name, for_dest());

	tlv_space = TLV_SET(tlv_area, TIPC_TLV_BEARER_NAME, 
			    bearer_name, sizeof(bearer_name));
	tlv_space = do_command(TIPC_CMD_DISABLE_BEARER, tlv_area, tlv_space,
			       tlv_area, sizeof(tlv_area));

	cprintf("Bearer <%s> disabled%s\n", bearer_name, for_dest());
}

static void disable_bearerset(char *args)
{
	if (args[0] == '?')
		do_these_bearers(disable_bearer, args+1); /* name pattern */
	else {
		while (args) {				
			disable_bearer(get_arg(&args)); /* list of names */
		}
	}
}


/******************************************************************************
 *
 * Basic data structures and routines associated with command/option processing
 *
 * Terminology note: The arguments passed to tipc-config are usually referred
 * to as "commands", since most of them are actually requests that are passed
 * on to TIPC rather than directives that are executed by tipc-config itself.
 * However, since tipc-config utilizes Linux's command line library to parse
 * the commands as if they were options, the latter term is also acceptable.
 *
 */

#define OPT_BASE '@'

struct command {
	void (*fcn) (char *args);
	char args[128];
};

/*
 * Help string generated by tipc-config application;
 * command entries are listed alphabetically
 */

static char usage[] =
"Usage: \n"
"       tipc-config command [command ...]\n"
"  \n"
"  valid commands:\n"
"  -addr [=<addr>]                            Get/set node address\n"
"  -b    [=<bearerpat>]                       Get bearers\n"
"  -bd    =<bearerpat>                        Disable bearer\n"
"  -be    =<bearer>[/<domain>[/<priority>]]]  Enable bearer\n"
"  -dest  =<addr>                             Command destination node\n"
"  -help                                      This usage list\n"
"  -i                                         Interactive set operations\n"
"  -l    [=<domain>|<linkpat>]                Get links to domain\n"
"  -log  [=<size>]                            Dump/resize log\n"
"  -lp    =<linkpat>|<bearer>|<media>/<value> Set link priority\n"
"  -ls   [=<linkpat>]                         Get link statistics\n"
"  -lsr   =<linkpat>                          Reset link statistics\n"
"  -lt    =<linkpat>|<bearer>|<media>/<value> Set link tolerance\n"
"  -lw    =<linkpat>|<bearer>|<media>/<value> Set link window\n"
"  -m                                         Get media\n"
"  -max_ports    [=<value>]                   Get/set max number of ports\n"
"  -max_publ     [=<value>]                   Get/set max publications\n"
"  -max_subscr   [=<value>]                   Get/set max subscriptions\n"
"  -mng  [=enable|disable]                    Get/set remote management\n"
"  -n    [=<domain>]                          Get nodes in domain\n"
"  -netid[=<value>]                           Get/set network id\n"
"  -nt   [=[<depth>,]<type>[,<low>[,<up>]]]   Get name table\n"
"        where <depth> = types|names|ports|all\n"
"  -p                                         Get port info\n"
"  -s                                         Get TIPC status info\n"
"  -v                                         Verbose output\n"
"  -V                                         Get tipc-config version info\n"
; /* end of concatenated string literal */

/*
 * Option structure field usage in tipc-config application:
 *	1) option name
 *	2) argument count
 *		0 if argument is not allowed
 *		1 if argument is required
 *		2 if argument is optional
 *	3) always set to 0 
 *	4) value to return
 *
 * Note 1: Option name field must match the info in "usage" (above).
 * Note 2: Entries need not be stored alphabetically, but "value to return"
 *         field must reflect ordering used in "cmd_array" (below).  
 */

static struct option options[] = {
	{"help",         0, 0, '0'},
	{"v",            0, 0, '1'},
	{"i",            0, 0, '2'},
	{"dest",         1, 0, '3'},
	{"V",            0, 0, '4'},
	{"addr",         2, 0, OPT_BASE + 0},
	{"netid",        2, 0, OPT_BASE + 1},
	{"mng",          2, 0, OPT_BASE + 2},
	{"nt",           2, 0, OPT_BASE + 3},
	{"p",            0, 0, OPT_BASE + 4},
	{"m",            0, 0, OPT_BASE + 5},
	{"b",            2, 0, OPT_BASE + 6},
	{"be",           1, 0, OPT_BASE + 7},
	{"bd",           1, 0, OPT_BASE + 8},
	{"n",            2, 0, OPT_BASE + 9},
	{"l",            2, 0, OPT_BASE + 10},
	{"ls",           2, 0, OPT_BASE + 11},
	{"lsr",          1, 0, OPT_BASE + 12},
	{"lp",           1, 0, OPT_BASE + 13},
	{"lw",           1, 0, OPT_BASE + 14},
	{"lt",           1, 0, OPT_BASE + 15},
	{"max_ports",    2, 0, OPT_BASE + 16},
	{"max_subscr",   2, 0, OPT_BASE + 17},
	{"max_publ",     2, 0, OPT_BASE + 18},
	{"log",          2, 0, OPT_BASE + 19},
	{"s",            0, 0, OPT_BASE + 20},
	{0, 0, 0, 0}
};

void (*cmd_array[])(char *args) = {
	set_node_addr,
	set_netid,
	set_remote_mng,
	show_name_table,
	show_ports,
	get_media,
	get_bearerset,
	enable_bearer,
	disable_bearerset,
	get_nodes,
	get_linkset,
	show_linkset_stats,
	reset_linkset_stats,
	set_linkset_priority,
	set_linkset_window,
	set_linkset_tolerance,
	set_max_ports,
	set_max_subscr,
	set_max_publ,
	set_log_size,
	show_stats,
	NULL
};

/*
 * Mainline parses option list and processes each command.  Most commands are
 * not actually executed until parsing is complete in case they are impacted
 * by commands that appear later in the list. 
 */

//int main(int argc, char *argv[], char *dummy[])
int tipc_config_main(int argc, char *argv[])
{
	struct command commands[MAX_COMMANDS];
	int cno, cno2;
	int c;

	if (argc == 1)
		fatal(usage);

	dest = own_node();

	cno = 0;
	while ((c = getopt_long_only(argc, argv, "", options, NULL)) != EOF) {

		if (c >= OPT_BASE) {
			if (cno >= MAX_COMMANDS)
				fatal("too many commands specified\n");

			commands[cno].fcn = cmd_array[c - OPT_BASE];
			if (optarg)
				strcpy(commands[cno].args, optarg);
			else
				commands[cno].args[0] = '\0';
			cno++;
		} else {
			switch (c) {
			case '0':
				fatal(usage);
				break;
			case '1':
				verbose = 1;
				break;
			case '2':
				interactive = 1;
				break;
			case '3':
				dest = str2addr(optarg);
				break;
			case '4':
				printf("TIPC configuration tool version "
				       VERSION "\n");
				break;
			default:
				/* getopt_long_only() generates the error msg */
				exit(EXIT_FAILURE);
				break;
			}
		}

	}

	if (optind < argc) {
		/* detects arguments that don't start with a '-' sign */
		fatal("unexpected command argument '%s'\n", argv[optind]);
	}

	for (cno2 = 0; cno2 < cno; cno2++) {
		if (!commands[cno2].fcn)
			fatal("command table error\n");
		commands[cno2].fcn(commands[cno2].args);
	}

	return 0;
}
