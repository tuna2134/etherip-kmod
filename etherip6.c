// SPDX-License-Identifier: GPL-2.0
#include <linux/etherdevice.h>
#include <linux/if_ether.h>
#include <linux/if_link.h>
#include <linux/if_vlan.h>
#include <linux/in6.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/netlink.h>
#include <linux/percpu.h>
#include <linux/rculist.h>
#include <linux/rtnetlink.h>
#include <linux/skbuff.h>
#include <linux/tcp.h>
#include <linux/unaligned.h>
#include <net/checksum.h>
#include <net/ip.h>
#include <net/ip6_route.h>
#include <net/ipv6.h>
#include <net/net_namespace.h>
#include <net/netns/generic.h>
#include <net/protocol.h>
#include <net/rtnetlink.h>
#include <net/tcp.h>

#include "etherip6_uapi.h"

#ifndef IPPROTO_ETHERIP
#define IPPROTO_ETHERIP 97
#endif

#define ETHERIP6_VERSION 3
#define ETHERIP6_HDR htons(ETHERIP6_VERSION << 12)
#define ETHERIP6_HLEN 2
#define ETHERIP6_DEFAULT_HOP_LIMIT 64
#define ETHERIP6_MAX_MTU 9000
#define ETHERIP6_OUTER_HLEN (sizeof(struct ipv6hdr) + ETHERIP6_HLEN)

struct etherip6_tunnel {
	struct list_head list;
	struct net_device *dev;
	struct in6_addr local;
	struct in6_addr remote;
	u32 link;
	u8 hop_limit;
	struct pcpu_sw_netstats __percpu *stats;
	atomic_long_t tx_dropped;
	atomic_long_t rx_dropped;
};

struct etherip6_net {
	struct list_head tunnels;
	rwlock_t lock;
};

static unsigned int etherip6_net_id;

static const struct nla_policy etherip6_policy[IFLA_ETHERIP6_MAX + 1] = {
	[IFLA_ETHERIP6_LOCAL] = NLA_POLICY_EXACT_LEN(sizeof(struct in6_addr)),
	[IFLA_ETHERIP6_REMOTE] = NLA_POLICY_EXACT_LEN(sizeof(struct in6_addr)),
	[IFLA_ETHERIP6_LINK] = { .type = NLA_U32 },
	[IFLA_ETHERIP6_HOP_LIMIT] = { .type = NLA_U8 },
};

static struct etherip6_net *etherip6_pernet(struct net *net)
{
	return net_generic(net, etherip6_net_id);
}

static bool etherip6_addr_equal(const struct in6_addr *a,
				const struct in6_addr *b)
{
	return ipv6_addr_equal(a, b);
}

static struct etherip6_tunnel *etherip6_lookup(struct net *net,
					       const struct in6_addr *local,
					       const struct in6_addr *remote,
					       int iif, bool running_only)
{
	struct etherip6_net *en = etherip6_pernet(net);
	struct etherip6_tunnel *tun;

	list_for_each_entry(tun, &en->tunnels, list) {
		if (running_only && !netif_running(tun->dev))
			continue;
		if (tun->link && tun->link != iif)
			continue;
		if (!etherip6_addr_equal(&tun->local, local))
			continue;
		if (!etherip6_addr_equal(&tun->remote, remote))
			continue;
		return tun;
	}

	return NULL;
}

static struct etherip6_tunnel *
etherip6_lookup_unique_rx(struct net *net, const struct in6_addr *local,
			  const struct in6_addr *remote, int iif,
			  bool match_local, bool match_remote)
{
	struct etherip6_net *en = etherip6_pernet(net);
	struct etherip6_tunnel *tun, *match = NULL;

	list_for_each_entry(tun, &en->tunnels, list) {
		if (!netif_running(tun->dev))
			continue;
		if (tun->link && tun->link != iif)
			continue;
		if (match_local && !etherip6_addr_equal(&tun->local, local))
			continue;
		if (match_remote && !etherip6_addr_equal(&tun->remote, remote))
			continue;
		if (match)
			return NULL;
		match = tun;
	}

	return match;
}

static struct etherip6_tunnel *etherip6_lookup_rx(struct net *net,
						  const struct in6_addr *local,
						  const struct in6_addr *remote,
						  int iif)
{
	struct etherip6_tunnel *tun;

	tun = etherip6_lookup(net, local, remote, iif, true);
	if (tun)
		return tun;

	tun = etherip6_lookup_unique_rx(net, local, remote, iif, false, true);
	if (tun)
		return tun;

	tun = etherip6_lookup_unique_rx(net, local, remote, iif, true, false);
	if (tun)
		return tun;

	return etherip6_lookup_unique_rx(net, local, remote, iif, false, false);
}

/*
 * Reduce an advertised MSS only when the encapsulated SYN would otherwise
 * exceed the smaller of the tunnel MTU and the current outer path MTU.
 * skb_header_pointer() keeps the common linear-skb path allocation-free while
 * still handling cloned and non-linear packets correctly.
 */
static void etherip6_clamp_tcp_mss(struct sk_buff *skb, unsigned int path_mtu)
{
	struct vlan_hdr vlan_buf, *vh;
	struct tcphdr tcp_buf, *th;
	struct ethhdr eth_buf, *eth;
	unsigned int nhoff = ETH_HLEN;
	unsigned int thoff, tcp_hlen;
	unsigned int inner_mtu, ip_hlen;
	unsigned char opt_buf[MAX_TCP_OPTION_SPACE], *opt;
	unsigned int optlen;
	unsigned int mss_offset;
	__be16 proto;
	u16 old_mss, new_mss;
	u8 nexthdr;

	eth = skb_header_pointer(skb, 0, sizeof(eth_buf), &eth_buf);
	if (unlikely(!eth))
		return;
	proto = eth->h_proto;

	while (eth_type_vlan(proto)) {
		vh = skb_header_pointer(skb, nhoff, sizeof(vlan_buf), &vlan_buf);
		if (unlikely(!vh))
			return;
		proto = vh->h_vlan_encapsulated_proto;
		nhoff += sizeof(*vh);
	}

	if (unlikely(path_mtu <= ETHERIP6_OUTER_HLEN + nhoff))
		return;
	inner_mtu = min_t(unsigned int, skb->dev->mtu,
			  path_mtu - ETHERIP6_OUTER_HLEN - nhoff);

	if (proto == htons(ETH_P_IP)) {
		struct iphdr ip_buf, *iph;

		iph = skb_header_pointer(skb, nhoff, sizeof(ip_buf), &ip_buf);
		if (!iph || iph->version != 4 || iph->ihl < 5 ||
		    iph->protocol != IPPROTO_TCP ||
		    (iph->frag_off & htons(IP_MF | IP_OFFSET)))
			return;
		thoff = nhoff + iph->ihl * 4;
		ip_hlen = sizeof(struct iphdr);
	} else if (proto == htons(ETH_P_IPV6)) {
		struct ipv6hdr ip6_buf, *ip6h;
		__be16 frag_off = 0;
		int offset;

		ip6h = skb_header_pointer(skb, nhoff, sizeof(ip6_buf), &ip6_buf);
		if (!ip6h || ip6h->version != 6)
			return;
		nexthdr = ip6h->nexthdr;
		offset = ipv6_skip_exthdr(skb, nhoff + sizeof(*ip6h),
				     &nexthdr, &frag_off);
		if (offset < 0 || nexthdr != IPPROTO_TCP || frag_off)
			return;
		thoff = offset;
		ip_hlen = sizeof(struct ipv6hdr);
	} else {
		return;
	}

	th = skb_header_pointer(skb, thoff, sizeof(tcp_buf), &tcp_buf);
	if (!th || !th->syn || th->doff < sizeof(*th) / 4)
		return;
	tcp_hlen = th->doff * 4;
	if (tcp_hlen > MAX_TCP_HEADER ||
	    inner_mtu <= ip_hlen + sizeof(struct tcphdr))
		return;
	new_mss = min_t(unsigned int, U16_MAX,
			inner_mtu - ip_hlen - sizeof(struct tcphdr));

	optlen = tcp_hlen - sizeof(*th);
	opt = skb_header_pointer(skb, thoff + sizeof(*th), optlen, opt_buf);
	if (!opt)
		return;

	while (optlen) {
		u8 kind = opt[0];
		u8 len;

		if (kind == TCPOPT_EOL)
			return;
		if (kind == TCPOPT_NOP) {
			opt++;
			optlen--;
			continue;
		}
		if (optlen < 2 || (len = opt[1]) < 2 || len > optlen)
			return;
		if (kind == TCPOPT_MSS && len == TCPOLEN_MSS) {
			old_mss = get_unaligned_be16(opt + 2);
			if (old_mss > new_mss) {
				mss_offset = thoff + tcp_hlen - optlen + 2;
				if (skb_ensure_writable(skb, mss_offset + 2))
					return;
				th = (struct tcphdr *)(skb->data + thoff);
				put_unaligned_be16(new_mss,
						     skb->data + mss_offset);
				inet_proto_csum_replace2(&th->check, skb,
						htons(old_mss), htons(new_mss),
						false);
			}
			return;
		}
		opt += len;
		optlen -= len;
	}
}

static netdev_tx_t etherip6_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct etherip6_tunnel *tun = netdev_priv(dev);
	struct net *net = dev_net(dev);
	struct dst_entry *dst;
	struct flowi6 fl6 = {};
	struct ipv6hdr *ip6h;
	struct pcpu_sw_netstats *stats;
	__be16 *etherip;
	unsigned int payload_len;
	unsigned int inner_len;
	int headroom;
	int err;

	if (skb->len + ETHERIP6_HLEN > IPV6_MAXPLEN)
		goto tx_error;

	fl6.flowi6_proto = IPPROTO_ETHERIP;
	fl6.flowi6_oif = tun->link;
	fl6.daddr = tun->remote;
	fl6.saddr = tun->local;

	dst = ip6_route_output(net, NULL, &fl6);
	if (dst->error) {
		dst_release(dst);
		goto tx_error;
	}

	etherip6_clamp_tcp_mss(skb, dst_mtu(dst));

	headroom = LL_RESERVED_SPACE(dst->dev) + sizeof(*ip6h) + ETHERIP6_HLEN;
	err = skb_cow_head(skb, headroom);
	if (err) {
		dst_release(dst);
		goto tx_error;
	}

	payload_len = skb->len + ETHERIP6_HLEN;
	inner_len = skb->len;

	etherip = skb_push(skb, ETHERIP6_HLEN);
	*etherip = ETHERIP6_HDR;

	skb_push(skb, sizeof(*ip6h));
	skb_reset_network_header(skb);
	ip6h = ipv6_hdr(skb);
	ip6_flow_hdr(ip6h, 0, 0);
	ip6h->payload_len = htons(payload_len);
	ip6h->nexthdr = IPPROTO_ETHERIP;
	ip6h->hop_limit = tun->hop_limit;
	ip6h->saddr = tun->local;
	ip6h->daddr = tun->remote;

	skb->protocol = htons(ETH_P_IPV6);
	skb->dev = dst->dev;
	skb->ignore_df = 1;
	skb_dst_set(skb, dst);

	/*
	 * skb->cb belongs to the protocol currently processing the skb.  The
	 * encapsulated frame may leave bridge, qdisc, or inner IPv6 state in it;
	 * in particular, stale inet6_skb_parm flags or frag_max_size corrupt the
	 * outer IPv6 fragmentation path.  Native IPv6 tunnels clear this state
	 * in ip6tunnel_xmit() before calling ip6_local_out().
	 */
	memset(skb->cb, 0, sizeof(struct inet6_skb_parm));

	err = ip6_local_out(net, NULL, skb);
	if (unlikely(net_xmit_eval(err))) {
		atomic_long_inc(&tun->tx_dropped);
		return NETDEV_TX_OK;
	}

	stats = this_cpu_ptr(tun->stats);
	u64_stats_update_begin(&stats->syncp);
	u64_stats_inc(&stats->tx_packets);
	u64_stats_add(&stats->tx_bytes, inner_len);
	u64_stats_update_end(&stats->syncp);

	return NETDEV_TX_OK;

tx_error:
	atomic_long_inc(&tun->tx_dropped);
	dev_kfree_skb(skb);
	return NETDEV_TX_OK;
}

static int etherip6_open(struct net_device *dev)
{
	netif_start_queue(dev);
	return 0;
}

static int etherip6_stop(struct net_device *dev)
{
	netif_stop_queue(dev);
	return 0;
}

static void etherip6_get_stats64(struct net_device *dev,
				 struct rtnl_link_stats64 *stats)
{
	struct etherip6_tunnel *tun = netdev_priv(dev);

	dev_fetch_sw_netstats(stats, tun->stats);
	stats->tx_dropped = atomic_long_read(&tun->tx_dropped);
	stats->rx_dropped = atomic_long_read(&tun->rx_dropped);
}

static void etherip6_uninit(struct net_device *dev);

static const struct net_device_ops etherip6_netdev_ops = {
	.ndo_open = etherip6_open,
	.ndo_stop = etherip6_stop,
	.ndo_start_xmit = etherip6_xmit,
	.ndo_get_stats64 = etherip6_get_stats64,
	.ndo_uninit = etherip6_uninit,
};

static void etherip6_setup(struct net_device *dev)
{
	ether_setup(dev);
	dev->netdev_ops = &etherip6_netdev_ops;
	dev->needs_free_netdev = true;
	dev->type = ARPHRD_ETHER;
	dev->flags &= ~IFF_NOARP;
	dev->features &= ~(NETIF_F_GSO_MASK | NETIF_F_CSUM_MASK);
	dev->hw_features = 0;
	dev->vlan_features = 0;
	dev->min_mtu = ETH_MIN_MTU;
	dev->max_mtu = ETHERIP6_MAX_MTU;
	eth_hw_addr_random(dev);
}

static int etherip6_validate(struct nlattr *tb[], struct nlattr *data[],
			     struct netlink_ext_ack *extack)
{
	if (!data)
		return -EINVAL;
	if (!data[IFLA_ETHERIP6_LOCAL]) {
		NL_SET_ERR_MSG(extack, "local IPv6 address is required");
		return -EINVAL;
	}
	if (!data[IFLA_ETHERIP6_REMOTE]) {
		NL_SET_ERR_MSG(extack, "remote IPv6 address is required");
		return -EINVAL;
	}

	return 0;
}

static int etherip6_newlink(struct net_device *dev,
			    struct rtnl_newlink_params *params,
			    struct netlink_ext_ack *extack)
{
	struct nlattr **data = params->data;
	struct etherip6_tunnel *tun = netdev_priv(dev);
	struct etherip6_net *en = etherip6_pernet(dev_net(dev));
	struct net_device *link_dev;
	struct etherip6_tunnel *old;
	int err;

	nla_memcpy(&tun->local, data[IFLA_ETHERIP6_LOCAL], sizeof(tun->local));
	nla_memcpy(&tun->remote, data[IFLA_ETHERIP6_REMOTE], sizeof(tun->remote));

	if (data[IFLA_ETHERIP6_LINK])
		tun->link = nla_get_u32(data[IFLA_ETHERIP6_LINK]);
	if (data[IFLA_ETHERIP6_HOP_LIMIT])
		tun->hop_limit = nla_get_u8(data[IFLA_ETHERIP6_HOP_LIMIT]);
	else
		tun->hop_limit = ETHERIP6_DEFAULT_HOP_LIMIT;

	if (ipv6_addr_any(&tun->local) || ipv6_addr_any(&tun->remote)) {
		NL_SET_ERR_MSG(extack, "local and remote must be non-zero IPv6 addresses");
		return -EINVAL;
	}
	if (ipv6_addr_is_multicast(&tun->remote)) {
		NL_SET_ERR_MSG(extack, "remote must not be multicast");
		return -EINVAL;
	}
	if (!tun->hop_limit) {
		NL_SET_ERR_MSG(extack, "hop limit must be non-zero");
		return -EINVAL;
	}
	if (tun->link) {
		link_dev = __dev_get_by_index(dev_net(dev), tun->link);
		if (!link_dev) {
			NL_SET_ERR_MSG(extack, "link interface does not exist");
			return -ENODEV;
		}
	}
	if ((ipv6_addr_type(&tun->local) & IPV6_ADDR_LINKLOCAL ||
	     ipv6_addr_type(&tun->remote) & IPV6_ADDR_LINKLOCAL) && !tun->link) {
		NL_SET_ERR_MSG(extack, "link-local endpoint requires link");
		return -EINVAL;
	}

	tun->dev = dev;
	atomic_long_set(&tun->tx_dropped, 0);
	atomic_long_set(&tun->rx_dropped, 0);
	tun->stats = netdev_alloc_pcpu_stats(struct pcpu_sw_netstats);
	if (!tun->stats)
		return -ENOMEM;

	write_lock_bh(&en->lock);
	old = etherip6_lookup(dev_net(dev), &tun->local, &tun->remote,
			      tun->link, false);
	if (old)
		err = -EEXIST;
	else
		list_add_rcu(&tun->list, &en->tunnels);
	write_unlock_bh(&en->lock);

	if (old) {
		free_percpu(tun->stats);
		NL_SET_ERR_MSG(extack, "duplicate local/remote/link tunnel");
		return err;
	}

	err = register_netdevice(dev);
	if (err) {
		write_lock_bh(&en->lock);
		list_del_rcu(&tun->list);
		write_unlock_bh(&en->lock);
		synchronize_rcu();
		free_percpu(tun->stats);
		return err;
	}

	return 0;
}

static void etherip6_dellink(struct net_device *dev, struct list_head *head)
{
	unregister_netdevice_queue(dev, head);
}

static void etherip6_uninit(struct net_device *dev)
{
	struct etherip6_tunnel *tun = netdev_priv(dev);
	struct etherip6_net *en = etherip6_pernet(dev_net(dev));

	write_lock_bh(&en->lock);
	list_del_rcu(&tun->list);
	write_unlock_bh(&en->lock);
	synchronize_rcu();
	free_percpu(tun->stats);
}

static size_t etherip6_get_size(const struct net_device *dev)
{
	return nla_total_size(sizeof(struct in6_addr)) +
	       nla_total_size(sizeof(struct in6_addr)) +
	       nla_total_size(sizeof(u32)) +
	       nla_total_size(sizeof(u8));
}

static int etherip6_fill_info(struct sk_buff *skb, const struct net_device *dev)
{
	const struct etherip6_tunnel *tun = netdev_priv(dev);

	if (nla_put(skb, IFLA_ETHERIP6_LOCAL, sizeof(tun->local), &tun->local) ||
	    nla_put(skb, IFLA_ETHERIP6_REMOTE, sizeof(tun->remote), &tun->remote) ||
	    nla_put_u32(skb, IFLA_ETHERIP6_LINK, tun->link) ||
	    nla_put_u8(skb, IFLA_ETHERIP6_HOP_LIMIT, tun->hop_limit))
		return -EMSGSIZE;

	return 0;
}

static struct rtnl_link_ops etherip6_link_ops __read_mostly = {
	.kind = "etherip6",
	.priv_size = sizeof(struct etherip6_tunnel),
	.setup = etherip6_setup,
	.maxtype = IFLA_ETHERIP6_MAX,
	.policy = etherip6_policy,
	.validate = etherip6_validate,
	.newlink = etherip6_newlink,
	.dellink = etherip6_dellink,
	.get_size = etherip6_get_size,
	.fill_info = etherip6_fill_info,
};

static int etherip6_rcv(struct sk_buff *skb)
{
	struct net *net = dev_net(skb->dev);
	const struct ipv6hdr *ip6h = ipv6_hdr(skb);
	struct etherip6_tunnel *tun;
	struct pcpu_sw_netstats *stats;
	__be16 etherip;
	int iif = skb->dev->ifindex;

	if (!pskb_may_pull(skb, ETHERIP6_HLEN))
		goto drop;

	etherip = *(__be16 *)skb->data;
	if (etherip != ETHERIP6_HDR)
		goto drop;

	read_lock_bh(&etherip6_pernet(net)->lock);
	tun = etherip6_lookup_rx(net, &ip6h->daddr, &ip6h->saddr, iif);
	if (tun)
		dev_hold(tun->dev);
	read_unlock_bh(&etherip6_pernet(net)->lock);

	if (!tun)
		goto drop;

	skb_pull(skb, ETHERIP6_HLEN);
	if (!pskb_may_pull(skb, ETH_HLEN)) {
		atomic_long_inc(&tun->rx_dropped);
		dev_put(tun->dev);
		goto drop;
	}

	skb->dev = tun->dev;
	skb_scrub_packet(skb, true);
	skb_reset_mac_header(skb);
	skb->protocol = eth_type_trans(skb, tun->dev);
	skb->pkt_type = PACKET_HOST;
	skb_reset_network_header(skb);

	stats = this_cpu_ptr(tun->stats);
	u64_stats_update_begin(&stats->syncp);
	u64_stats_inc(&stats->rx_packets);
	u64_stats_add(&stats->rx_bytes, skb->len);
	u64_stats_update_end(&stats->syncp);

	netif_rx(skb);
	dev_put(tun->dev);
	return 0;

drop:
	kfree_skb(skb);
	return 0;
}

static const struct inet6_protocol etherip6_protocol = {
	.handler = etherip6_rcv,
	.flags = INET6_PROTO_NOPOLICY | INET6_PROTO_FINAL,
};

static int __net_init etherip6_net_init(struct net *net)
{
	struct etherip6_net *en = etherip6_pernet(net);

	INIT_LIST_HEAD(&en->tunnels);
	rwlock_init(&en->lock);
	return 0;
}

static struct pernet_operations etherip6_net_ops = {
	.init = etherip6_net_init,
	.id = &etherip6_net_id,
	.size = sizeof(struct etherip6_net),
};

static int __init etherip6_init(void)
{
	int err;

	err = register_pernet_subsys(&etherip6_net_ops);
	if (err)
		return err;

	err = inet6_add_protocol(&etherip6_protocol, IPPROTO_ETHERIP);
	if (err)
		goto err_pernet;

	err = rtnl_link_register(&etherip6_link_ops);
	if (err)
		goto err_proto;

	pr_info("etherip6: RFC3378 EtherIP over IPv6 loaded\n");
	return 0;

err_proto:
	inet6_del_protocol(&etherip6_protocol, IPPROTO_ETHERIP);
err_pernet:
	unregister_pernet_subsys(&etherip6_net_ops);
	return err;
}

static void __exit etherip6_exit(void)
{
	rtnl_link_unregister(&etherip6_link_ops);
	inet6_del_protocol(&etherip6_protocol, IPPROTO_ETHERIP);
	unregister_pernet_subsys(&etherip6_net_ops);
	pr_info("etherip6: unloaded\n");
}

module_init(etherip6_init);
module_exit(etherip6_exit);

MODULE_AUTHOR("Masato Kikuchi");
MODULE_DESCRIPTION("RFC3378 EtherIP tunnel endpoint over IPv6");
MODULE_LICENSE("GPL");
MODULE_ALIAS_RTNL_LINK("etherip6");
