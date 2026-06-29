// SPDX-License-Identifier: GPL-2.0
#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "etherip6_uapi.h"

#define BUF_SIZE 8192

static void usage(const char *prog)
{
	fprintf(stderr,
		"usage:\n"
		"  %s add DEV local ADDR remote ADDR [link IFACE] [hoplimit N]\n"
		"  %s del DEV\n",
		prog, prog);
}

static void addattr(struct nlmsghdr *nlh, size_t maxlen, int type,
		    const void *data, size_t len)
{
	struct nlattr *nla;
	size_t nla_len = NLA_HDRLEN + len;
	size_t new_len = NLMSG_ALIGN(nlh->nlmsg_len) + NLA_ALIGN(nla_len);

	if (new_len > maxlen) {
		fprintf(stderr, "netlink message too large\n");
		exit(1);
	}

	nla = (struct nlattr *)((char *)nlh + NLMSG_ALIGN(nlh->nlmsg_len));
	nla->nla_type = type;
	nla->nla_len = nla_len;
	memcpy((char *)nla + NLA_HDRLEN, data, len);
	nlh->nlmsg_len = new_len;
}

static struct nlattr *nest_start(struct nlmsghdr *nlh, size_t maxlen, int type)
{
	struct nlattr *nla;

	addattr(nlh, maxlen, type, NULL, 0);
	nla = (struct nlattr *)((char *)nlh + NLMSG_ALIGN(nlh->nlmsg_len) -
				NLA_ALIGN(NLA_HDRLEN));
	return nla;
}

static void nest_end(struct nlmsghdr *nlh, struct nlattr *nla)
{
	nla->nla_len = (char *)nlh + nlh->nlmsg_len - (char *)nla;
}

static void addattr_string(struct nlmsghdr *nlh, size_t maxlen, int type,
			   const char *str)
{
	addattr(nlh, maxlen, type, str, strlen(str) + 1);
}

static int rtnl_talk(struct nlmsghdr *nlh)
{
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
	char buf[BUF_SIZE];
	struct iovec iov = { .iov_base = nlh, .iov_len = nlh->nlmsg_len };
	struct msghdr msg = { .msg_name = &sa, .msg_namelen = sizeof(sa),
			      .msg_iov = &iov, .msg_iovlen = 1 };
	int fd, ret;

	fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (fd < 0)
		return -1;

	nlh->nlmsg_seq = 1;
	if (sendmsg(fd, &msg, 0) < 0) {
		ret = -1;
		goto out;
	}

	ret = recv(fd, buf, sizeof(buf), 0);
	if (ret < 0)
		goto out;

	for (nlh = (struct nlmsghdr *)buf; NLMSG_OK(nlh, ret);
	     nlh = NLMSG_NEXT(nlh, ret)) {
		if (nlh->nlmsg_type == NLMSG_ERROR) {
			struct nlmsgerr *err = NLMSG_DATA(nlh);

			if (err->error) {
				errno = -err->error;
				ret = -1;
			} else {
				ret = 0;
			}
			goto out;
		}
	}

	ret = 0;
out:
	close(fd);
	return ret;
}

static int cmd_add(int argc, char **argv)
{
	char buf[BUF_SIZE] = {};
	struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
	struct ifinfomsg *ifi;
	struct nlattr *linkinfo, *infodata;
	struct in6_addr local = {}, remote = {};
	uint32_t link = 0;
	uint8_t hoplimit = 64;
	bool have_local = false, have_remote = false;
	const char *dev;
	int i;

	if (argc < 7) {
		usage(argv[0]);
		return 1;
	}

	dev = argv[2];
	for (i = 3; i < argc; i++) {
		if (!strcmp(argv[i], "local") && i + 1 < argc) {
			if (inet_pton(AF_INET6, argv[++i], &local) != 1) {
				fprintf(stderr, "invalid local IPv6 address\n");
				return 1;
			}
			have_local = true;
		} else if (!strcmp(argv[i], "remote") && i + 1 < argc) {
			if (inet_pton(AF_INET6, argv[++i], &remote) != 1) {
				fprintf(stderr, "invalid remote IPv6 address\n");
				return 1;
			}
			have_remote = true;
		} else if (!strcmp(argv[i], "link") && i + 1 < argc) {
			link = if_nametoindex(argv[++i]);
			if (!link) {
				perror("if_nametoindex");
				return 1;
			}
		} else if (!strcmp(argv[i], "hoplimit") && i + 1 < argc) {
			unsigned long v = strtoul(argv[++i], NULL, 0);

			if (!v || v > 255) {
				fprintf(stderr, "hoplimit must be 1..255\n");
				return 1;
			}
			hoplimit = (uint8_t)v;
		} else {
			usage(argv[0]);
			return 1;
		}
	}

	if (!have_local || !have_remote) {
		usage(argv[0]);
		return 1;
	}

	nlh->nlmsg_len = NLMSG_LENGTH(sizeof(*ifi));
	nlh->nlmsg_type = RTM_NEWLINK;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL;
	ifi = NLMSG_DATA(nlh);
	ifi->ifi_family = AF_UNSPEC;

	addattr_string(nlh, sizeof(buf), IFLA_IFNAME, dev);
	linkinfo = nest_start(nlh, sizeof(buf), IFLA_LINKINFO);
	addattr_string(nlh, sizeof(buf), IFLA_INFO_KIND, "etherip6");
	infodata = nest_start(nlh, sizeof(buf), IFLA_INFO_DATA);
	addattr(nlh, sizeof(buf), IFLA_ETHERIP6_LOCAL, &local, sizeof(local));
	addattr(nlh, sizeof(buf), IFLA_ETHERIP6_REMOTE, &remote, sizeof(remote));
	if (link)
		addattr(nlh, sizeof(buf), IFLA_ETHERIP6_LINK, &link, sizeof(link));
	addattr(nlh, sizeof(buf), IFLA_ETHERIP6_HOP_LIMIT, &hoplimit, sizeof(hoplimit));
	nest_end(nlh, infodata);
	nest_end(nlh, linkinfo);

	if (rtnl_talk(nlh) < 0) {
		perror("RTM_NEWLINK");
		return 1;
	}

	return 0;
}

static int cmd_del(int argc, char **argv)
{
	char buf[BUF_SIZE] = {};
	struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
	struct ifinfomsg *ifi;

	if (argc != 3) {
		usage(argv[0]);
		return 1;
	}

	nlh->nlmsg_len = NLMSG_LENGTH(sizeof(*ifi));
	nlh->nlmsg_type = RTM_DELLINK;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	ifi = NLMSG_DATA(nlh);
	ifi->ifi_family = AF_UNSPEC;
	ifi->ifi_index = if_nametoindex(argv[2]);
	if (!ifi->ifi_index) {
		perror("if_nametoindex");
		return 1;
	}

	if (rtnl_talk(nlh) < 0) {
		perror("RTM_DELLINK");
		return 1;
	}

	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		usage(argv[0]);
		return 1;
	}

	if (!strcmp(argv[1], "add"))
		return cmd_add(argc, argv);
	if (!strcmp(argv[1], "del"))
		return cmd_del(argc, argv);

	usage(argv[0]);
	return 1;
}
