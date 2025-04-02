/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		IPv4 FIB: lookup engine and maintenance routines.
 *
 * Version:	$Id: fib_hash.c,v 1.13 2001/10/31 21:55:54 davem Exp $
 *
 * Authors:	Alexey Kuznetsov, <kuznet@ms2.inr.ac.ru>
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */

#include <linux/config.h>
#include <asm/uaccess.h>
#include <asm/system.h>
#include <linux/bitops.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/socket.h>
#include <linux/sockios.h>
#include <linux/errno.h>
#include <linux/in.h>
#include <linux/inet.h>
#include <linux/netdevice.h>
#include <linux/if_arp.h>
#include <linux/proc_fs.h>
#include <linux/skbuff.h>
#include <linux/netlink.h>
#include <linux/init.h>

#include <net/ip.h>
#include <net/protocol.h>
#include <net/route.h>
#include <net/tcp.h>
#include <net/sock.h>
#include <net/ip_fib.h>

#include "fib_lookup.h"

static kmem_cache_t *fn_hash_kmem;
static kmem_cache_t *fn_alias_kmem;

/*
 * fn_key: IP地址与掩码相与
 */
struct fib_node {
	struct hlist_node	fn_hash;
	struct list_head	fn_alias;
	u32			fn_key;
};

/*
 * fz_hash: 指向hash表
 * fn_nent: 表项个数
 * fz_divisor: hash桶个数(2^4)
 * fz_hashmask: hash掩码(2^4-1)
 * fz_order: 掩码长度，比如24掩码
 * fz_mask: 24位掩码时，数值为0xffffff00(即255.255.255.0)
 */
struct fn_zone {
	struct fn_zone		*fz_next;	/* Next not empty zone	*/
	struct hlist_head	*fz_hash;	/* Hash table pointer	*/
	int			fz_nent;	/* Number of entries	*/

	int			fz_divisor;	/* Hash divisor		*/
	u32			fz_hashmask;	/* (fz_divisor - 1)	*/
#define FZ_HASHMASK(fz)		((fz)->fz_hashmask)

	int			fz_order;	/* Zone order		*/
	u32			fz_mask;
#define FZ_MASK(fz)		((fz)->fz_mask)
};

/* NOTE. On fast computers evaluation of fz_hashmask and fz_mask
 * can be cheaper than memory lookup, so that FZ_* macros are used.
 */

struct fn_hash {
	struct fn_zone	*fn_zones[33];
	struct fn_zone	*fn_zone_list;
};

static inline u32 fn_hash(u32 key, struct fn_zone *fz)
{
	/*
	 * 仅取key的有效部分计算hash.
	 * 举例说明:
	 * IP 为 172.31.3.138/24
	 * key为 172.31.3.0
	 * 经过计算后，h 仅取 172.31.3 做hash
	 */
	u32 h = ntohl(key)>>(32 - fz->fz_order);
	h ^= (h>>20);
	h ^= (h>>10);
	h ^= (h>>5);
	h &= FZ_HASHMASK(fz);
	return h;
}

static inline u32 fz_key(u32 dst, struct fn_zone *fz)
{
	/* 目的地址与上掩码，假设为24位掩码，即 IP & 255.255.255.0 */
	return dst & FZ_MASK(fz);
}

static DEFINE_RWLOCK(fib_hash_lock);
static unsigned int fib_hash_genid;

/* 限制最大的hash 桶个数 */
#define FZ_MAX_DIVISOR ((PAGE_SIZE<<MAX_ORDER) / sizeof(struct hlist_head))

/* 传入参数为hash 桶个数 */
static struct hlist_head *fz_hash_alloc(int divisor)
{
	unsigned long size = divisor * sizeof(struct hlist_head);

	if (size <= PAGE_SIZE) {
		return kmalloc(size, GFP_KERNEL);
	} else {
		return (struct hlist_head *)
			__get_free_pages(GFP_KERNEL, get_order(size));
	}
}

/* 释放hash表，对应fb_hash_alloc() */
static void fz_hash_free(struct hlist_head *hash, int divisor)
{
	unsigned long size = divisor * sizeof(struct hlist_head);

	if (size <= PAGE_SIZE)
		kfree(hash);
	else
		free_pages((unsigned long)hash, get_order(size));
}

/*
 * 重新构建fn_zone{} hash 表
 *
 * fz: 要重新构建hash表的fn_zone{}
 * old_ht: hash表指针
 * old_divisor: 老hash表的桶个数
 */
/* The fib hash lock must be held when this is called. */
static inline void fn_rebuild_zone(struct fn_zone *fz,
				   struct hlist_head *old_ht,
				   int old_divisor)
{
	int i;

	for (i = 0; i < old_divisor; i++) {
		struct hlist_node *node, *n;
		struct fib_node *f;

		/* 依次遍历所有的fib_node{}节点，添加到新的hash表中 */
		hlist_for_each_entry_safe(f, node, n, &old_ht[i], fn_hash) {
			struct hlist_head *new_head;

			hlist_del(&f->fn_hash);

			new_head = &fz->fz_hash[fn_hash(f->fn_key, fz)];
			hlist_add_head(&f->fn_hash, new_head);
		}
	}
}

static void fn_rehash_zone(struct fn_zone *fz)
{
	struct hlist_head *ht, *old_ht;
	int old_divisor, new_divisor;
	u32 new_hashmask;
		
	old_divisor = fz->fz_divisor;

	switch (old_divisor) {
	case 16:
		new_divisor = 256;
		break;
	case 256:
		new_divisor = 1024;
		break;
	default:
		if ((old_divisor << 1) > FZ_MAX_DIVISOR) {
			printk(KERN_CRIT "route.c: bad divisor %d!\n", old_divisor);
			return;
		}
		new_divisor = (old_divisor << 1);
		break;
	}

	new_hashmask = (new_divisor - 1);

#if RT_CACHE_DEBUG >= 2
	printk("fn_rehash_zone: hash for zone %d grows from %d\n", fz->fz_order, old_divisor);
#endif

	ht = fz_hash_alloc(new_divisor);

	if (ht)	{
		memset(ht, 0, new_divisor * sizeof(struct hlist_head));

		write_lock_bh(&fib_hash_lock);
		old_ht = fz->fz_hash;
		fz->fz_hash = ht;
		fz->fz_hashmask = new_hashmask;
		fz->fz_divisor = new_divisor;
		fn_rebuild_zone(fz, old_ht, old_divisor);
		fib_hash_genid++;
		write_unlock_bh(&fib_hash_lock);

		/* 释放旧的hash表 */
		fz_hash_free(old_ht, old_divisor);
	}
}

static inline void fn_free_node(struct fib_node * f)
{
	kmem_cache_free(fn_hash_kmem, f);
}

static inline void fn_free_alias(struct fib_alias *fa)
{
	/* 释放对于fib_info{} 的引用计数 */
	fib_release_info(fa->fa_info);
	kmem_cache_free(fn_alias_kmem, fa);
}

static struct fn_zone *
fn_new_zone(struct fn_hash *table, int z)
{
	int i;
	struct fn_zone *fz = kmalloc(sizeof(struct fn_zone), GFP_KERNEL);
	if (!fz)
		return NULL;

	memset(fz, 0, sizeof(struct fn_zone));

	/* z 为 0 时候，hash 表只有一个hash 桶 */
	if (z) {
		fz->fz_divisor = 16;	//2^4
	} else {
		fz->fz_divisor = 1;	//1
	}
	fz->fz_hashmask = (fz->fz_divisor - 1);
	fz->fz_hash = fz_hash_alloc(fz->fz_divisor);
	if (!fz->fz_hash) {
		kfree(fz);
		return NULL;
	}
	memset(fz->fz_hash, 0, fz->fz_divisor * sizeof(struct hlist_head *));
	fz->fz_order = z;
	fz->fz_mask = inet_make_mask(z);

	/* 按掩码长度从长到短顺序插入到fn_zone_list 链表中， */
	/* Find the first not empty zone with more specific mask */
	for (i=z+1; i<=32; i++)
		if (table->fn_zones[i])
			break;
	write_lock_bh(&fib_hash_lock);
	if (i>32) {
		/* No more specific masks, we are the first. */
		fz->fz_next = table->fn_zone_list;
		table->fn_zone_list = fz;
	} else {
		fz->fz_next = table->fn_zones[i]->fz_next;
		table->fn_zones[i]->fz_next = fz;
	}
	table->fn_zones[z] = fz;
	fib_hash_genid++;
	write_unlock_bh(&fib_hash_lock);
	return fz;
}

static int
fn_hash_lookup(struct fib_table *tb, const struct flowi *flp, struct fib_result *res)
{
	int err;
	struct fn_zone *fz;
	struct fn_hash *t = (struct fn_hash*)tb->tb_data;

	read_lock(&fib_hash_lock);
	for (fz = t->fn_zone_list; fz; fz = fz->fz_next) {
		struct hlist_head *head;
		struct hlist_node *node;
		struct fib_node *f;
		/*
		 * 通过目的地址和掩码获取到key，这里的key 即是IP地址与上掩码，
		 * 24位掩码举例，即是 IP & 255.255.255.0
		 */
		u32 k = fz_key(flp->fl4_dst, fz);

		/* 获取到一个hash值 */
		head = &fz->fz_hash[fn_hash(k, fz)];
		hlist_for_each_entry(f, node, head, fn_hash) {
			if (f->fn_key != k)
				continue;

			err = fib_semantic_match(&f->fn_alias,
						 flp, res,
						 f->fn_key, fz->fz_mask,
						 fz->fz_order);
			/*
			 * <0	异常
			 *  0	找到了
			 *
			 * 找到了或者出现了异常，跳转到out
			 */
			if (err <= 0)
				goto out;
		}
	}
	err = 1;
out:
	read_unlock(&fib_hash_lock);
	return err;
}

/*
 * 记录上次使用的默认路由.
 * 当存在多个默认路由时候，上次使用的默认路由会影响默认路由的选择.
 */
static int fn_hash_last_dflt=-1;

/*
 * tb: 路由表
 * flp: 查询条件
 * res: 查询结果
 *
 * 挑选合适的下一跳，核心就是重新设置 res->fi.
 * 通过fn_hash_last_dflt 数值，可以实现多次选择默认路由时，不会重复选择某一个.
 */
static void
fn_hash_select_default(struct fib_table *tb, const struct flowi *flp, struct fib_result *res)
{
	int order, last_idx;
	struct hlist_node *node;
	struct fib_node *f;
	struct fib_info *fi = NULL;
	struct fib_info *last_resort;
	struct fn_hash *t = (struct fn_hash*)tb->tb_data;
	/* 获取对应的fb_zones */
	struct fn_zone *fz = t->fn_zones[0];

	if (fz == NULL)
		return;

	last_idx = -1;
	last_resort = NULL;
	order = -1;

	/* 默认路由只有一个hash 桶 */
	read_lock(&fib_hash_lock);
	/*
	 * 遍历所有的fib_node
	 * 个人理解这里只能有一个，因为掩码长度为 0 导致fn_key 为 0.
	 */
	hlist_for_each_entry(f, node, &fz->fz_hash[0], fn_hash) {
		struct fib_alias *fa;

		/* 遍历fib_alias{} */
		list_for_each_entry(fa, &f->fn_alias, fa_list) {
			struct fib_info *next_fi = fa->fa_info;

			/* 网关路由必定是单播 */
			if (fa->fa_scope != res->scope || fa->fa_type != RTN_UNICAST)
				continue;

			/*
			 * 查找结果需满足优先级关系，如果已经查找到更大优先级了，
			 * 则退出
			 */
			if (next_fi->fib_priority > res->fi->fib_priority)
				break;
			if (!next_fi->fib_nh[0].nh_gw ||
			    next_fi->fib_nh[0].nh_scope != RT_SCOPE_LINK)
				continue;
			fa->fa_state |= FA_S_ACCESSED;

			if (fi == NULL) {
				/* 第一个fib_alias{} */
				if (next_fi != res->fi)
					break;
			} else if (!fib_detect_death(fi, order, &last_resort,
						     &last_idx, &fn_hash_last_dflt)) {
				if (res->fi)
					fib_info_put(res->fi);
				res->fi = fi;
				atomic_inc(&fi->fib_clntref);
				fn_hash_last_dflt = order;
				goto out;
			}
			fi = next_fi;
			order++;
		}
	}

	if (order <= 0 || fi == NULL) {
		fn_hash_last_dflt = -1;
		goto out;
	}

	if (!fib_detect_death(fi, order, &last_resort, &last_idx, &fn_hash_last_dflt)) {
		if (res->fi)
			fib_info_put(res->fi);
		res->fi = fi;
		atomic_inc(&fi->fib_clntref);
		fn_hash_last_dflt = order;
		goto out;
	}

	if (last_idx >= 0) {
		if (res->fi)
			fib_info_put(res->fi);
		res->fi = last_resort;
		if (last_resort)
			atomic_inc(&last_resort->fib_clntref);
	}
	fn_hash_last_dflt = last_idx;
out:
	read_unlock(&fib_hash_lock);
}

/* Insert node F to FZ. */
static inline void fib_insert_node(struct fn_zone *fz, struct fib_node *f)
{
	struct hlist_head *head = &fz->fz_hash[fn_hash(f->fn_key, fz)];

	/* 头插，按照发生先后顺序，顺序与成员内容无关 */
	hlist_add_head(&f->fn_hash, head);
}

/* Return the node in FZ matching KEY. */
static struct fib_node *fib_find_node(struct fn_zone *fz, u32 key)
{
	struct hlist_head *head = &fz->fz_hash[fn_hash(key, fz)];
	struct hlist_node *node;
	struct fib_node *f;

	/* 匹配key获取到正确的fib_node */
	hlist_for_each_entry(f, node, head, fn_hash) {
		if (f->fn_key == key)
			return f;
	}

	return NULL;
}

static int
fn_hash_insert(struct fib_table *tb, struct rtmsg *r, struct kern_rta *rta,
	       struct nlmsghdr *n, struct netlink_skb_parms *req)
{
	struct fn_hash *table = (struct fn_hash *) tb->tb_data;
	struct fib_node *new_f, *f;
	struct fib_alias *fa, *new_fa;
	struct fn_zone *fz;
	struct fib_info *fi;
	int z = r->rtm_dst_len;
	int type = r->rtm_type;
	u8 tos = r->rtm_tos;
	u32 key;
	int err;

	if (z > 32)
		return -EINVAL;
	fz = table->fn_zones[z];
	if (!fz && !(fz = fn_new_zone(table, z)))
		return -ENOBUFS;

	/* 获取fib_node 值 */
	key = 0;
	if (rta->rta_dst) {
		u32 dst;
		memcpy(&dst, rta->rta_dst, 4);
		if (dst & ~FZ_MASK(fz))
			return -EINVAL;
		key = fz_key(dst, fz);
	}

	if  ((fi = fib_create_info(r, rta, n, &err)) == NULL)
		return err;

	if (fz->fz_nent > (fz->fz_divisor<<1) &&
	    fz->fz_divisor < FZ_MAX_DIVISOR &&
	    (z==32 || (1<<z) > fz->fz_divisor))
		fn_rehash_zone(fz);

	f = fib_find_node(fz, key);

	if (!f)
		fa = NULL;
	else
		fa = fib_find_alias(&f->fn_alias, tos, fi->fib_priority);

	/*
	 * Now fa, if non-NULL, points to the first fib alias
	 * with the same keys [prefix,tos,priority], if such key already
	 * exists or to the node before which we will insert new one.
	 *
	 * If fa is NULL, we will need to allocate a new one and
	 * insert to the head of f.
	 *
	 * If f is NULL, no fib node matched the destination key
	 * and we need to allocate a new one of those as well.
	 */

	if (fa && fa->fa_tos == tos && fa->fa_info->fib_priority == fi->fib_priority) {
		struct fib_alias *fa_orig;

		/* 如果已经存在，则退出 */
		err = -EEXIST;
		if (n->nlmsg_flags & NLM_F_EXCL)
			goto out;

		/* 替换 */
		if (n->nlmsg_flags & NLM_F_REPLACE) {
			struct fib_info *fi_drop;
			u8 state;

			write_lock_bh(&fib_hash_lock);
			fi_drop = fa->fa_info;
			fa->fa_info = fi;
			fa->fa_type = type;
			fa->fa_scope = r->rtm_scope;
			state = fa->fa_state;
			fa->fa_state &= ~FA_S_ACCESSED;
			fib_hash_genid++;
			write_unlock_bh(&fib_hash_lock);

			fib_release_info(fi_drop);
			if (state & FA_S_ACCESSED)
				rt_cache_flush(-1);
			return 0;
		}

		/* Error if we find a perfect match which
		 * uses the same scope, type, and nexthop
		 * information.
		 */
		fa_orig = fa;
		fa = list_entry(fa->fa_list.prev, struct fib_alias, fa_list);
		list_for_each_entry_continue(fa, &f->fn_alias, fa_list) {
			if (fa->fa_tos != tos)
				break;
			if (fa->fa_info->fib_priority != fi->fib_priority)
				break;
			if (fa->fa_type == type &&
			    fa->fa_scope == r->rtm_scope &&
			    fa->fa_info == fi)
				goto out;
		}
		if (!(n->nlmsg_flags & NLM_F_APPEND))
			fa = fa_orig;
	}

	/* 如果不创建新的，返回失败 */
	err = -ENOENT;
	if (!(n->nlmsg_flags&NLM_F_CREATE))
		goto out;

	err = -ENOBUFS;
	new_fa = kmem_cache_alloc(fn_alias_kmem, SLAB_KERNEL);
	if (new_fa == NULL)
		goto out;

	new_f = NULL;
	if (!f) {
		new_f = kmem_cache_alloc(fn_hash_kmem, SLAB_KERNEL);
		if (new_f == NULL)
			goto out_free_new_fa;

		INIT_HLIST_NODE(&new_f->fn_hash);
		INIT_LIST_HEAD(&new_f->fn_alias);
		new_f->fn_key = key;
		f = new_f;
	}

	new_fa->fa_info = fi;
	new_fa->fa_tos = tos;
	new_fa->fa_type = type;
	new_fa->fa_scope = r->rtm_scope;
	new_fa->fa_state = 0;

	/*
	 * Insert new entry to the list.
	 */

	write_lock_bh(&fib_hash_lock);
	/* fib_node 插入到链表中 */
	if (new_f)
		fib_insert_node(fz, new_f);
	/*
	 * 这里的fa 是之前通过fib_find_alias()获取的，所以这里fib_alias
	 * 插入是有一定顺序的，具体可以查看fib_find_alias()注释.
	 */
	list_add_tail(&new_fa->fa_list, (fa ? &fa->fa_list : &f->fn_alias));
	fib_hash_genid++;
	write_unlock_bh(&fib_hash_lock);

	if (new_f)
		fz->fz_nent++;
	rt_cache_flush(-1);

	/* 发送消息 */
	rtmsg_fib(RTM_NEWROUTE, key, new_fa, z, tb->tb_id, n, req);
	return 0;

out_free_new_fa:
	kmem_cache_free(fn_alias_kmem, new_fa);
out:
	fib_release_info(fi);
	return err;
}


static int
fn_hash_delete(struct fib_table *tb, struct rtmsg *r, struct kern_rta *rta,
	       struct nlmsghdr *n, struct netlink_skb_parms *req)
{
	struct fn_hash *table = (struct fn_hash*)tb->tb_data;
	struct fib_node *f;
	struct fib_alias *fa, *fa_to_delete;
	int z = r->rtm_dst_len;
	struct fn_zone *fz;
	u32 key;
	u8 tos = r->rtm_tos;

	if (z > 32)
		return -EINVAL;
	if ((fz  = table->fn_zones[z]) == NULL)
		return -ESRCH;

	key = 0;
	if (rta->rta_dst) {
		u32 dst;
		memcpy(&dst, rta->rta_dst, 4);
		if (dst & ~FZ_MASK(fz))
			return -EINVAL;
		key = fz_key(dst, fz);
	}

	f = fib_find_node(fz, key);

	if (!f)
		fa = NULL;
	else
		fa = fib_find_alias(&f->fn_alias, tos, 0);
	if (!fa)
		return -ESRCH;

	fa_to_delete = NULL;
	fa = list_entry(fa->fa_list.prev, struct fib_alias, fa_list);
	list_for_each_entry_continue(fa, &f->fn_alias, fa_list) {
		struct fib_info *fi = fa->fa_info;

		if (fa->fa_tos != tos)
			break;

		if ((!r->rtm_type || fa->fa_type == r->rtm_type) &&
		    (r->rtm_scope == RT_SCOPE_NOWHERE || fa->fa_scope == r->rtm_scope) &&
		    (!r->rtm_protocol || fi->fib_protocol == r->rtm_protocol) &&
		    fib_nh_match(r, n, rta, fi) == 0) {
			fa_to_delete = fa;
			break;
		}
	}

	if (fa_to_delete) {
		int kill_fn;

		fa = fa_to_delete;
		rtmsg_fib(RTM_DELROUTE, key, fa, z, tb->tb_id, n, req);

		kill_fn = 0;
		write_lock_bh(&fib_hash_lock);
		list_del(&fa->fa_list);
		if (list_empty(&f->fn_alias)) {
			hlist_del(&f->fn_hash);
			kill_fn = 1;
		}
		fib_hash_genid++;
		write_unlock_bh(&fib_hash_lock);

		/* 刷新路由缓存 */
		if (fa->fa_state & FA_S_ACCESSED)
			rt_cache_flush(-1);
		fn_free_alias(fa);
		if (kill_fn) {
			fn_free_node(f);
			fz->fz_nent--;
		}

		return 0;
	}
	/* 返回没查询到 */
	return -ESRCH;
}

static int fn_flush_list(struct fn_zone *fz, int idx)
{
	struct hlist_head *head = &fz->fz_hash[idx];
	struct hlist_node *node, *n;
	struct fib_node *f;
	int found = 0;

	/* 遍历fib_node */
	hlist_for_each_entry_safe(f, node, n, head, fn_hash) {
		struct fib_alias *fa, *fa_node;
		int kill_f;

		kill_f = 0;
		/* 遍历fib_alias */
		list_for_each_entry_safe(fa, fa_node, &f->fn_alias, fa_list) {
			/* 获取fib_info */
			struct fib_info *fi = fa->fa_info;

			/* 刷新是仅仅删除设置了RTNH_F_DEAD 标识位的路由信息 */
			if (fi && (fi->fib_flags&RTNH_F_DEAD)) {
				write_lock_bh(&fib_hash_lock);
				list_del(&fa->fa_list);
				/* 如果fib_node 中为空，删除fib_node */
				if (list_empty(&f->fn_alias)) {
					hlist_del(&f->fn_hash);
					kill_f = 1;
				}
				fib_hash_genid++;
				write_unlock_bh(&fib_hash_lock);

				fn_free_alias(fa);
				found++;
			}
		}
		/* 需要删除fib_node */
		if (kill_f) {
			fn_free_node(f);
			fz->fz_nent--;
		}
	}
	return found;
}

/* 刷新路由表 */
static int fn_hash_flush(struct fib_table *tb)
{
	struct fn_hash *table = (struct fn_hash *) tb->tb_data;
	struct fn_zone *fz;
	int found = 0;

	for (fz = table->fn_zone_list; fz; fz = fz->fz_next) {
		int i;

		/* fz_divisor 为hash 桶个数，依次遍历哈希桶 */
		for (i = fz->fz_divisor - 1; i >= 0; i--)
			found += fn_flush_list(fz, i);
	}
	return found;
}


static inline int
fn_hash_dump_bucket(struct sk_buff *skb, struct netlink_callback *cb,
		     struct fib_table *tb,
		     struct fn_zone *fz,
		     struct hlist_head *head)
{
	struct hlist_node *node;
	struct fib_node *f;
	int i, s_i;

	i = 0;
	s_i = cb->args[3];
	/* 遍历hash表中的fib_node */
	hlist_for_each_entry(f, node, head, fn_hash) {
		struct fib_alias *fa;

		/* 遍历不同的fib_alias */
		list_for_each_entry(fa, &f->fn_alias, fa_list) {
			if (i < s_i)
				goto next;

			/* 输出fib_info */
			if (fib_dump_info(skb, NETLINK_CB(cb->skb).pid,
					  cb->nlh->nlmsg_seq,
					  RTM_NEWROUTE,
					  tb->tb_id,
					  fa->fa_type,
					  fa->fa_scope,
					  &f->fn_key,
					  fz->fz_order,
					  fa->fa_tos,
					  fa->fa_info) < 0) {
				cb->args[3] = i;
				return -1;
			}
		next:
			i++;
		}
	}
	cb->args[3] = i;
	return skb->len;
}

/*
 * tb: 路由表
 * fz: 对应的fn_zone 结构体
 */
static inline int
fn_hash_dump_zone(struct sk_buff *skb, struct netlink_callback *cb,
		   struct fib_table *tb, struct fn_zone *fz)
{
	int h, s_h;

	s_h = cb->args[2];
	for (h=0; h < fz->fz_divisor; h++) {
		if (h < s_h) continue;
		/* 同下，表示从 0 开始 */
		if (h > s_h)
			memset(&cb->args[3], 0, sizeof(cb->args) - 3*sizeof(cb->args[0]));
		/* hash 表为空则跳转到下一个哈希桶 */
		if (fz->fz_hash == NULL || hlist_empty(&fz->fz_hash[h]))
			continue;
		if (fn_hash_dump_bucket(skb, cb, tb, fz, &fz->fz_hash[h])<0) {
			cb->args[2] = h;
			return -1;
		}
	}
	cb->args[2] = h;
	return skb->len;
}

/*
 * args[] 是一个数组，总共有四个分别表示:
 * args[0]: 路由表信息
 * args[1]: fn_zone{}信息
 * args[2]: fn_zone{}中hash桶
 * args[3]: hash桶中fib_alias{}个数
 */
static int fn_hash_dump(struct fib_table *tb, struct sk_buff *skb, struct netlink_callback *cb)
{
	int m, s_m;
	struct fn_zone *fz;
	struct fn_hash *table = (struct fn_hash*)tb->tb_data;

	s_m = cb->args[1];
	read_lock(&fib_hash_lock);
	for (fz = table->fn_zone_list, m=0; fz; fz = fz->fz_next, m++) {
		if (m < s_m) continue;
		/* 清空 args[2]、args[3]，从 0 开始 */
		if (m > s_m)
			memset(&cb->args[2], 0, sizeof(cb->args) - 2*sizeof(cb->args[0]));
		if (fn_hash_dump_zone(skb, cb, tb, fz) < 0) {
			cb->args[1] = m;
			read_unlock(&fib_hash_lock);
			return -1;
		}
	}
	read_unlock(&fib_hash_lock);
	cb->args[1] = m;
	return skb->len;
}

#ifdef CONFIG_IP_MULTIPLE_TABLES
struct fib_table * fib_hash_init(int id)
#else
struct fib_table * __init fib_hash_init(int id)
#endif
{
	struct fib_table *tb;

	/* 创建内存缓存 */
	if (fn_hash_kmem == NULL)
		fn_hash_kmem = kmem_cache_create("ip_fib_hash",
						 sizeof(struct fib_node),
						 0, SLAB_HWCACHE_ALIGN,
						 NULL, NULL);

	if (fn_alias_kmem == NULL)
		fn_alias_kmem = kmem_cache_create("ip_fib_alias",
						  sizeof(struct fib_alias),
						  0, SLAB_HWCACHE_ALIGN,
						  NULL, NULL);

	tb = kmalloc(sizeof(struct fib_table) + sizeof(struct fn_hash),
		     GFP_KERNEL);
	if (tb == NULL)
		return NULL;

	tb->tb_id = id;
	tb->tb_lookup = fn_hash_lookup;
	tb->tb_insert = fn_hash_insert;
	tb->tb_delete = fn_hash_delete;
	tb->tb_flush = fn_hash_flush;
	tb->tb_select_default = fn_hash_select_default;
	tb->tb_dump = fn_hash_dump;
	memset(tb->tb_data, 0, sizeof(struct fn_hash));
	return tb;
}

/* ------------------------------------------------------------------------ */
#ifdef CONFIG_PROC_FS

struct fib_iter_state {
	struct fn_zone	*zone;
	int		bucket;
	struct hlist_head *hash_head;
	struct fib_node *fn;
	struct fib_alias *fa;
	loff_t pos;
	unsigned int genid;
	int valid;
};

static struct fib_alias *fib_get_first(struct seq_file *seq)
{
	struct fib_iter_state *iter = seq->private;
	struct fn_hash *table = (struct fn_hash *) ip_fib_main_table->tb_data;

	iter->bucket    = 0;
	iter->hash_head = NULL;
	iter->fn        = NULL;
	iter->fa        = NULL;
	iter->pos	= 0;
	iter->genid	= fib_hash_genid;
	iter->valid	= 1;

	for (iter->zone = table->fn_zone_list; iter->zone;
	     iter->zone = iter->zone->fz_next) {
		int maxslot;

		if (!iter->zone->fz_nent)
			continue;

		iter->hash_head = iter->zone->fz_hash;
		maxslot = iter->zone->fz_divisor;

		for (iter->bucket = 0; iter->bucket < maxslot;
		     ++iter->bucket, ++iter->hash_head) {
			struct hlist_node *node;
			struct fib_node *fn;

			hlist_for_each_entry(fn,node,iter->hash_head,fn_hash) {
				struct fib_alias *fa;

				list_for_each_entry(fa,&fn->fn_alias,fa_list) {
					iter->fn = fn;
					iter->fa = fa;
					goto out;
				}
			}
		}
	}
out:
	return iter->fa;
}

static struct fib_alias *fib_get_next(struct seq_file *seq)
{
	struct fib_iter_state *iter = seq->private;
	struct fib_node *fn;
	struct fib_alias *fa;

	/* Advance FA, if any. */
	fn = iter->fn;
	fa = iter->fa;
	if (fa) {
		BUG_ON(!fn);
		list_for_each_entry_continue(fa, &fn->fn_alias, fa_list) {
			iter->fa = fa;
			goto out;
		}
	}

	fa = iter->fa = NULL;

	/* Advance FN. */
	if (fn) {
		struct hlist_node *node = &fn->fn_hash;
		hlist_for_each_entry_continue(fn, node, fn_hash) {
			iter->fn = fn;

			list_for_each_entry(fa, &fn->fn_alias, fa_list) {
				iter->fa = fa;
				goto out;
			}
		}
	}

	fn = iter->fn = NULL;

	/* Advance hash chain. */
	if (!iter->zone)
		goto out;

	for (;;) {
		struct hlist_node *node;
		int maxslot;

		maxslot = iter->zone->fz_divisor;

		while (++iter->bucket < maxslot) {
			iter->hash_head++;

			hlist_for_each_entry(fn, node, iter->hash_head, fn_hash) {
				list_for_each_entry(fa, &fn->fn_alias, fa_list) {
					iter->fn = fn;
					iter->fa = fa;
					goto out;
				}
			}
		}

		iter->zone = iter->zone->fz_next;

		if (!iter->zone)
			goto out;
		
		iter->bucket = 0;
		iter->hash_head = iter->zone->fz_hash;

		hlist_for_each_entry(fn, node, iter->hash_head, fn_hash) {
			list_for_each_entry(fa, &fn->fn_alias, fa_list) {
				iter->fn = fn;
				iter->fa = fa;
				goto out;
			}
		}
	}
out:
	iter->pos++;
	return fa;
}

static struct fib_alias *fib_get_idx(struct seq_file *seq, loff_t pos)
{
	struct fib_iter_state *iter = seq->private;
	struct fib_alias *fa;
	
	if (iter->valid && pos >= iter->pos && iter->genid == fib_hash_genid) {
		fa   = iter->fa;
		pos -= iter->pos;
	} else
		fa = fib_get_first(seq);

	if (fa)
		while (pos && (fa = fib_get_next(seq)))
			--pos;
	return pos ? NULL : fa;
}

static void *fib_seq_start(struct seq_file *seq, loff_t *pos)
{
	void *v = NULL;

	read_lock(&fib_hash_lock);
	if (ip_fib_main_table)
		v = *pos ? fib_get_idx(seq, *pos - 1) : SEQ_START_TOKEN;
	return v;
}

static void *fib_seq_next(struct seq_file *seq, void *v, loff_t *pos)
{
	++*pos;
	return v == SEQ_START_TOKEN ? fib_get_first(seq) : fib_get_next(seq);
}

static void fib_seq_stop(struct seq_file *seq, void *v)
{
	read_unlock(&fib_hash_lock);
}

static unsigned fib_flag_trans(int type, u32 mask, struct fib_info *fi)
{
	static unsigned type2flags[RTN_MAX + 1] = {
		[7] = RTF_REJECT, [8] = RTF_REJECT,
	};
	unsigned flags = type2flags[type];

	if (fi && fi->fib_nh->nh_gw)
		flags |= RTF_GATEWAY;
	if (mask == 0xFFFFFFFF)
		flags |= RTF_HOST;
	flags |= RTF_UP;
	return flags;
}

/* 
 *	This outputs /proc/net/route.
 *
 *	It always works in backward compatibility mode.
 *	The format of the file is not supposed to be changed.
 */
static int fib_seq_show(struct seq_file *seq, void *v)
{
	struct fib_iter_state *iter;
	char bf[128];
	u32 prefix, mask;
	unsigned flags;
	struct fib_node *f;
	struct fib_alias *fa;
	struct fib_info *fi;

	if (v == SEQ_START_TOKEN) {
		seq_printf(seq, "%-127s\n", "Iface\tDestination\tGateway "
			   "\tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU"
			   "\tWindow\tIRTT");
		goto out;
	}

	iter	= seq->private;
	f	= iter->fn;
	fa	= iter->fa;
	fi	= fa->fa_info;
	prefix	= f->fn_key;
	mask	= FZ_MASK(iter->zone);
	flags	= fib_flag_trans(fa->fa_type, mask, fi);
	if (fi)
		snprintf(bf, sizeof(bf),
			 "%s\t%08X\t%08X\t%04X\t%d\t%u\t%d\t%08X\t%d\t%u\t%u",
			 fi->fib_dev ? fi->fib_dev->name : "*", prefix,
			 fi->fib_nh->nh_gw, flags, 0, 0, fi->fib_priority,
			 mask, (fi->fib_advmss ? fi->fib_advmss + 40 : 0),
			 fi->fib_window,
			 fi->fib_rtt >> 3);
	else
		snprintf(bf, sizeof(bf),
			 "*\t%08X\t%08X\t%04X\t%d\t%u\t%d\t%08X\t%d\t%u\t%u",
			 prefix, 0, flags, 0, 0, 0, mask, 0, 0, 0);
	seq_printf(seq, "%-127s\n", bf);
out:
	return 0;
}

static struct seq_operations fib_seq_ops = {
	.start  = fib_seq_start,
	.next   = fib_seq_next,
	.stop   = fib_seq_stop,
	.show   = fib_seq_show,
};

static int fib_seq_open(struct inode *inode, struct file *file)
{
	struct seq_file *seq;
	int rc = -ENOMEM;
	struct fib_iter_state *s = kmalloc(sizeof(*s), GFP_KERNEL);
       
	if (!s)
		goto out;

	rc = seq_open(file, &fib_seq_ops);
	if (rc)
		goto out_kfree;

	seq	     = file->private_data;
	seq->private = s;
	memset(s, 0, sizeof(*s));
out:
	return rc;
out_kfree:
	kfree(s);
	goto out;
}

static struct file_operations fib_seq_fops = {
	.owner		= THIS_MODULE,
	.open           = fib_seq_open,
	.read           = seq_read,
	.llseek         = seq_lseek,
	.release	= seq_release_private,
};

int __init fib_proc_init(void)
{
	if (!proc_net_fops_create("route", S_IRUGO, &fib_seq_fops))
		return -ENOMEM;
	return 0;
}

void __init fib_proc_exit(void)
{
	proc_net_remove("route");
}
#endif /* CONFIG_PROC_FS */
