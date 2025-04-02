#ifndef _LINUX_PID_H
#define _LINUX_PID_H

/*
 * PID		进程ID
 * TGID		线程组ID
 * PGID		进程组ID
 * 		默认情况下，父进程创建子进程时，子进程与父进程属于
 * 		同一个进程组.
 * 		可以通过setpgid() 设置，启动一个新的进程组.
 * 		通过shell启动一个新任务的时候，会启动一个新的进程组.
 * SID		会话ID
 * 		与进程组一起作为组织进程的一种方式，相同进程组的进程
 * 		必须从属于同一会话.
 */
enum pid_type
{
	PIDTYPE_PID,
	PIDTYPE_TGID,
	PIDTYPE_PGID,
	PIDTYPE_SID,
	PIDTYPE_MAX
};

struct pid
{
	/* Try to keep pid_chain in the same cacheline as nr for find_pid */
	int nr;
	struct hlist_node pid_chain;
	/* list of pids with the same nr, only one of them is in the hash */
	struct list_head pid_list;
};

#define pid_task(elem, type) \
	list_entry(elem, struct task_struct, pids[type].pid_list)

/*
 * attach_pid() and detach_pid() must be called with the tasklist_lock
 * write-held.
 */
extern int FASTCALL(attach_pid(struct task_struct *task, enum pid_type type, int nr));

extern void FASTCALL(detach_pid(struct task_struct *task, enum pid_type));

/*
 * look up a PID in the hash table. Must be called with the tasklist_lock
 * held.
 */
extern struct pid *FASTCALL(find_pid(enum pid_type, int));

extern int alloc_pidmap(void);
extern void FASTCALL(free_pidmap(int));
extern void switch_exec_pids(struct task_struct *leader, struct task_struct *thread);

#define do_each_task_pid(who, type, task)				\
	if ((task = find_task_by_pid_type(type, who))) {		\
		prefetch((task)->pids[type].pid_list.next);		\
		do {

#define while_each_task_pid(who, type, task)				\
		} while (task = pid_task((task)->pids[type].pid_list.next,\
						type),			\
			prefetch((task)->pids[type].pid_list.next),	\
			hlist_unhashed(&(task)->pids[type].pid_chain));	\
	}								\

#endif /* _LINUX_PID_H */
