#ifndef _NET_NEIGHBOUR_H
#define _NET_NEIGHBOUR_H

/*
 *	Generic neighbour manipulation
 *
 *	Authors:
 *	Pedro Roque		<roque@di.fc.ul.pt>
 *	Alexey Kuznetsov	<kuznet@ms2.inr.ac.ru>
 *
 * 	Changes:
 *
 *	Harald Welte:		<laforge@gnumonks.org>
 *		- Add neighbour cache statistics like rtstat
 */

/* The following flags & states are exported to user space,
   so that they should be moved to include/linux/ directory.
 */

/*
 *	Neighbor Cache Entry Flags
 *	邻居表标识位
 */

#define NTF_PROXY	0x08	/* == ATF_PUBL */
#define NTF_ROUTER	0x80

/*
 *	Neighbor Cache Entry States.
 *	邻居表状态
 */

/* 发出请求包，没有得到回应 */
#define NUD_INCOMPLETE	0x01
/* 邻居表项可达 */
#define NUD_REACHABLE	0x02
#define NUD_STALE	0x04
#define NUD_DELAY	0x08
#define NUD_PROBE	0x10
/* ARP请求之后没回，进入失败状态 */
#define NUD_FAILED	0x20

/* Dummy states */
#define NUD_NOARP	0x40
#define NUD_PERMANENT	0x80
/* 表项刚被创建，还没有一个有效的状态 */
#define NUD_NONE	0x00

/* NUD_NOARP & NUD_PERMANENT are pseudostates, they never change
   and make no address resolution or NUD.
   NUD_PERMANENT is also cannot be deleted by garbage collectors.
 */

#ifdef __KERNEL__

#include <asm/atomic.h>
#include <linux/skbuff.h>
#include <linux/netdevice.h>
#include <linux/rcupdate.h>
#include <linux/seq_file.h>

#include <linux/err.h>
#include <linux/sysctl.h>

#define NUD_IN_TIMER	(NUD_INCOMPLETE|NUD_REACHABLE|NUD_DELAY|NUD_PROBE)
#define NUD_VALID	(NUD_PERMANENT|NUD_NOARP|NUD_REACHABLE|NUD_PROBE|NUD_STALE|NUD_DELAY)
#define NUD_CONNECTED	(NUD_PERMANENT|NUD_NOARP|NUD_REACHABLE)

struct neighbour;

/*
 * reachable_time:	可达时间
 */
struct neigh_parms
{
	struct neigh_parms *next;
	int	(*neigh_setup)(struct neighbour *);
	struct neigh_table *tbl;
	int	entries;
	void	*priv;

	void	*sysctl_table;

	int	dead;
	atomic_t refcnt;
	struct	rcu_head rcu_head;

	int	base_reachable_time;
	int	retrans_time;
	int	gc_staletime;
	int	reachable_time;
	int	delay_probe_time;

	int	queue_len;
	int	ucast_probes;
	int	app_probes;
	int	mcast_probes;
	int	anycast_delay;
	int	proxy_delay;
	int	proxy_qlen;
	int	locktime;
};

struct neigh_statistics
{
	unsigned long allocs;		/* number of allocated neighs */
	unsigned long destroys;		/* number of destroyed neighs */
	unsigned long hash_grows;	/* number of hash resizes */

	unsigned long res_failed;	/* nomber of failed resolutions */

	unsigned long lookups;		/* number of lookups */
	unsigned long hits;		/* number of hits (among lookups) */

	unsigned long rcv_probes_mcast;	/* number of received mcast ipv6 */
	unsigned long rcv_probes_ucast; /* number of received ucast ipv6 */

	unsigned long periodic_gc_runs;	/* number of periodic GC runs */
	unsigned long forced_gc_runs;	/* number of forced GC runs */
};

#define NEIGH_CACHE_STAT_INC(tbl, field)				\
	do {								\
		preempt_disable();					\
		(per_cpu_ptr((tbl)->stats, smp_processor_id())->field)++; \
		preempt_enable();					\
	} while (0)

/*
 * dev:		出口设备
 * used:	使用时间
 * confirmed:	设置确认时间
 * 		1. neigh_update()中状态为NUD_CONNECTED 时会更新
 * 		2. 调用dst_confirm()更新刷新时间
 * updated:	更新时间
 * nud_state:	邻居表状态
 * type:	使用了与路由类型相同的宏
 * dead:	表示表项已不可用
 * probes:	当前发送请求的次数
 * ha:		对端硬件地址
 * hh:		硬件缓存链表，因为对应不同协议，所以需要多个
 * output:	对应neigh_ops{}中的发送函数
 * arp_queue:	ARP队列，要发送的报文当邻居表项暂时不可用时，
 * 		需要临时放在队列中
 * timer:	邻居表项定时器
 * ops:		指向对应的neigh_ops{}结构体，由出口设备
 * 		的设备类型决定
 * primary_key: 对于不同的协议来说，key值是不同的，所以放到最后，
 * 		由协议指定私有长度.
 */
struct neighbour
{
	struct neighbour	*next;
	struct neigh_table	*tbl;
	struct neigh_parms	*parms;
	struct net_device	*dev;
	unsigned long		used;
	unsigned long		confirmed;
	unsigned long		updated;
	__u8			flags;
	__u8			nud_state;
	__u8			type;
	__u8			dead;
	atomic_t		probes;
	rwlock_t		lock;
	unsigned char		ha[(MAX_ADDR_LEN+sizeof(unsigned long)-1)&~(sizeof(unsigned long)-1)];
	struct hh_cache		*hh;
	atomic_t		refcnt;
	int			(*output)(struct sk_buff *skb);
	struct sk_buff_head	arp_queue;
	struct timer_list	timer;
	struct neigh_ops	*ops;
	u8			primary_key[0];
};

/*
 * output:		慢速发送
 * connected_output:	快速发送
 * hh_output:		超级快速发送，缓存硬件头
 * queue_xmit:		真正的底层发送函数，慢速、快速发送都会调用
 * 			该函数.
 */
struct neigh_ops
{
	int			family;
	void			(*destructor)(struct neighbour *);
	void			(*solicit)(struct neighbour *, struct sk_buff*);
	void			(*error_report)(struct neighbour *, struct sk_buff*);
	int			(*output)(struct sk_buff*);
	int			(*connected_output)(struct sk_buff*);
	int			(*hh_output)(struct sk_buff*);
	int			(*queue_xmit)(struct sk_buff*);
};

/* 可以做代理的IP地址 */
struct pneigh_entry
{
	struct pneigh_entry	*next;
	struct net_device	*dev;
	u8			key[0];
};

/*
 *	neighbour table manipulation
 */


/*
 * entry_size:	表项长度，sizeof(neighbour)+key长度
 * key_len:	邻居表项的key，IPv4为IP，key_len即为IP地址长度
 * hash:	邻居表项插入到hash表中的hash函数
 *
 * id:		邻居表名称
 *
 * gc_interval:	[已弃用]，查看2.3内核会用该值作为gc定时器时间间隔，
 * 		该版本已不会用到该值.
 * gc_thresh1:	一阶阈值
 * gc_thresh2:	二阶阈值
 * gc_thresh3:	三阶阈值
 *
 * last_flush:	邻居表上次刷新时间
 * entries:	邻居表表项个数
 * lock:	读写锁，保护邻居表项hash表
 * last_rand:	上次计算reachable_time的时间
 * hash_rnd:	随机数，做hash时用作种子
 *
 * kmem_cachep:	邻居表项内存申请
 * hash_buckets:邻居表项hash桶指针
 * hash_mask:	hash掩码
 * hash_rand:	hash种子
 *
 */
struct neigh_table
{
	struct neigh_table	*next;
	int			family;
	int			entry_size;
	int			key_len;
	__u32			(*hash)(const void *pkey, const struct net_device *);
	int			(*constructor)(struct neighbour *);
	int			(*pconstructor)(struct pneigh_entry *);
	void			(*pdestructor)(struct pneigh_entry *);
	void			(*proxy_redo)(struct sk_buff *skb);
	char			*id;
	struct neigh_parms	parms;
	/* HACK. gc_* shoul follow parms without a gap! */
	int			gc_interval;
	int			gc_thresh1;
	int			gc_thresh2;
	int			gc_thresh3;
	unsigned long		last_flush;
	struct timer_list 	gc_timer;
	struct timer_list 	proxy_timer;
	struct sk_buff_head	proxy_queue;
	atomic_t		entries;
	rwlock_t		lock;
	unsigned long		last_rand;
	struct neigh_parms	*parms_list;
	kmem_cache_t		*kmem_cachep;
	struct neigh_statistics	*stats;
	struct neighbour	**hash_buckets;
	unsigned int		hash_mask;
	__u32			hash_rnd;
	unsigned int		hash_chain_gc;
	struct pneigh_entry	**phash_buckets;
#ifdef CONFIG_PROC_FS
	struct proc_dir_entry	*pde;
#endif
};

/* flags for neigh_update() */

/* 允许更新已经存在的二层地址 */
#define NEIGH_UPDATE_F_OVERRIDE			0x00000001
/* 质疑已连接的邻居表项 */
#define NEIGH_UPDATE_F_WEAK_OVERRIDE		0x00000002
/* 允许覆盖路由器邻居表项 */
#define NEIGH_UPDATE_F_OVERRIDE_ISROUTER	0x00000004
/* 邻居表项是否是路由器 */
#define NEIGH_UPDATE_F_ISROUTER			0x40000000
/* 修改是用户下发的命令 */
#define NEIGH_UPDATE_F_ADMIN			0x80000000

extern void			neigh_table_init(struct neigh_table *tbl);
extern int			neigh_table_clear(struct neigh_table *tbl);
extern struct neighbour *	neigh_lookup(struct neigh_table *tbl,
					     const void *pkey,
					     struct net_device *dev);
extern struct neighbour *	neigh_lookup_nodev(struct neigh_table *tbl,
						   const void *pkey);
extern struct neighbour *	neigh_create(struct neigh_table *tbl,
					     const void *pkey,
					     struct net_device *dev);
extern void			neigh_destroy(struct neighbour *neigh);
extern int			__neigh_event_send(struct neighbour *neigh, struct sk_buff *skb);
extern int			neigh_update(struct neighbour *neigh, const u8 *lladdr, u8 new, 
					     u32 flags);
extern void			neigh_changeaddr(struct neigh_table *tbl, struct net_device *dev);
extern int			neigh_ifdown(struct neigh_table *tbl, struct net_device *dev);
extern int			neigh_resolve_output(struct sk_buff *skb);
extern int			neigh_connected_output(struct sk_buff *skb);
extern int			neigh_compat_output(struct sk_buff *skb);
extern struct neighbour 	*neigh_event_ns(struct neigh_table *tbl,
						u8 *lladdr, void *saddr,
						struct net_device *dev);

extern struct neigh_parms	*neigh_parms_alloc(struct net_device *dev, struct neigh_table *tbl);
extern void			neigh_parms_release(struct neigh_table *tbl, struct neigh_parms *parms);
extern void			neigh_parms_destroy(struct neigh_parms *parms);
extern unsigned long		neigh_rand_reach_time(unsigned long base);

extern void			pneigh_enqueue(struct neigh_table *tbl, struct neigh_parms *p,
					       struct sk_buff *skb);
extern struct pneigh_entry	*pneigh_lookup(struct neigh_table *tbl, const void *key, struct net_device *dev, int creat);
extern int			pneigh_delete(struct neigh_table *tbl, const void *key, struct net_device *dev);

struct netlink_callback;
struct nlmsghdr;
extern int neigh_dump_info(struct sk_buff *skb, struct netlink_callback *cb);
extern int neigh_add(struct sk_buff *skb, struct nlmsghdr *nlh, void *arg);
extern int neigh_delete(struct sk_buff *skb, struct nlmsghdr *nlh, void *arg);
extern void neigh_app_ns(struct neighbour *n);

extern void neigh_for_each(struct neigh_table *tbl, void (*cb)(struct neighbour *, void *), void *cookie);
extern void __neigh_for_each_release(struct neigh_table *tbl, int (*cb)(struct neighbour *));
extern void pneigh_for_each(struct neigh_table *tbl, void (*cb)(struct pneigh_entry *));

struct neigh_seq_state {
	struct neigh_table *tbl;
	void *(*neigh_sub_iter)(struct neigh_seq_state *state,
				struct neighbour *n, loff_t *pos);
	unsigned int bucket;
	unsigned int flags;
#define NEIGH_SEQ_NEIGH_ONLY	0x00000001
#define NEIGH_SEQ_IS_PNEIGH	0x00000002
#define NEIGH_SEQ_SKIP_NOARP	0x00000004
};
extern void *neigh_seq_start(struct seq_file *, loff_t *, struct neigh_table *, unsigned int);
extern void *neigh_seq_next(struct seq_file *, void *, loff_t *);
extern void neigh_seq_stop(struct seq_file *, void *);

extern int			neigh_sysctl_register(struct net_device *dev, 
						      struct neigh_parms *p,
						      int p_id, int pdev_id,
						      char *p_name,
						      proc_handler *proc_handler,
						      ctl_handler *strategy);
extern void			neigh_sysctl_unregister(struct neigh_parms *p);

static inline void __neigh_parms_put(struct neigh_parms *parms)
{
	atomic_dec(&parms->refcnt);
}

static inline void neigh_parms_put(struct neigh_parms *parms)
{
	if (atomic_dec_and_test(&parms->refcnt))
		neigh_parms_destroy(parms);
}

static inline struct neigh_parms *neigh_parms_clone(struct neigh_parms *parms)
{
	atomic_inc(&parms->refcnt);
	return parms;
}

/*
 *	Neighbour references
 */

static inline void neigh_release(struct neighbour *neigh)
{
	if (atomic_dec_and_test(&neigh->refcnt))
		neigh_destroy(neigh);
}

static inline struct neighbour * neigh_clone(struct neighbour *neigh)
{
	if (neigh)
		atomic_inc(&neigh->refcnt);
	return neigh;
}

#define neigh_hold(n)	atomic_inc(&(n)->refcnt)

static inline void neigh_confirm(struct neighbour *neigh)
{
	if (neigh)
		neigh->confirmed = jiffies;
}

static inline int neigh_is_connected(struct neighbour *neigh)
{
	return neigh->nud_state&NUD_CONNECTED;
}

static inline int neigh_is_valid(struct neighbour *neigh)
{
	return neigh->nud_state&NUD_VALID;
}

static inline int neigh_event_send(struct neighbour *neigh, struct sk_buff *skb)
{
	neigh->used = jiffies;
	if (!(neigh->nud_state&(NUD_CONNECTED|NUD_DELAY|NUD_PROBE)))
		return __neigh_event_send(neigh, skb);
	return 0;
}

/*
 * 查找邻居表项.
 *
 * pkey:	IPv4是下一跳IP地址
 */
static inline struct neighbour *
__neigh_lookup(struct neigh_table *tbl, const void *pkey, struct net_device *dev, int creat)
{
	struct neighbour *n = neigh_lookup(tbl, pkey, dev);

	if (n || !creat)
		return n;

	n = neigh_create(tbl, pkey, dev);
	return IS_ERR(n) ? NULL : n;
}

/* 没有邻居表项即创建 */
static inline struct neighbour *
__neigh_lookup_errno(struct neigh_table *tbl, const void *pkey,
  struct net_device *dev)
{
	struct neighbour *n = neigh_lookup(tbl, pkey, dev);

	if (n)
		return n;

	return neigh_create(tbl, pkey, dev);
}

#define LOCALLY_ENQUEUED -2

#endif
#endif


