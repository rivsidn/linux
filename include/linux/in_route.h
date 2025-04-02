#ifndef _LINUX_IN_ROUTE_H
#define _LINUX_IN_ROUTE_H

/* IPv4 routing cache flags */

#define RTCF_DEAD	RTNH_F_DEAD
#define RTCF_ONLINK	RTNH_F_ONLINK

/* Obsolete flag. About to be deleted */
#define RTCF_NOPMTUDISC RTM_F_NOPMTUDISC

#define RTCF_NOTIFY	0x00010000	/* 路由改变时，通知用户态 */
#define RTCF_DIRECTDST	0x00020000
#define RTCF_REDIRECTED	0x00040000	/* 重定向生成的路由 */
#define RTCF_TPROXY	0x00080000

#define RTCF_FAST	0x00200000
#define RTCF_MASQ	0x00400000
#define RTCF_SNAT	0x00800000
#define RTCF_DOREDIRECT 0x01000000
#define RTCF_DIRECTSRC	0x04000000	/* 本地设备发出去的包 TODO:存在疑问 */
#define RTCF_DNAT	0x08000000
#define RTCF_BROADCAST	0x10000000
#define RTCF_MULTICAST	0x20000000
#define RTCF_REJECT	0x40000000
#define RTCF_LOCAL	0x80000000	/* 上送本机 */

#define RTCF_NAT	(RTCF_DNAT|RTCF_SNAT)

#define RT_TOS(tos)	((tos)&IPTOS_TOS_MASK)

#endif /* _LINUX_IN_ROUTE_H */
