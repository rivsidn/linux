/*
 * linux/kernel/workqueue.c
 *
 * Generic mechanism for defining kernel helper threads for running
 * arbitrary tasks in process context.
 *
 * Started by Ingo Molnar, Copyright (C) 2002
 *
 * Derived from the taskqueue/keventd code by:
 *
 *   David Woodhouse <dwmw2@infradead.org>
 *   Andrew Morton <andrewm@uow.edu.au>
 *   Kai Petzke <wpp@marie.physik.tu-berlin.de>
 *   Theodore Ts'o <tytso@mit.edu>
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/init.h>
#include <linux/signal.h>
#include <linux/completion.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/cpu.h>
#include <linux/notifier.h>
#include <linux/kthread.h>

/*
 * The per-CPU workqueue (if single thread, we always use cpu 0's).
 *
 * The sequence counters are for flush_scheduled_work().  It wants to wait
 * until until all currently-scheduled works are completed, but it doesn't
 * want to be livelocked by new, incoming ones.  So it waits until
 * remove_sequence is >= the insert_sequence which pertained when
 * flush_scheduled_work() was called.
 */
/*
 * remove_sequence	flush_scheduled_work()中使用，执行该函数的时候，希望
 * insert_sequence	能够将当前加入的work全部执行完，但是执行过程中并不会
 * 			加锁，通过比较计数的方式实现.
 * worklist		需要执行的work_struct{} 添加到worklist 队列中.
 * more_work		执行进程(worker_thread()函数)当没有任务需要执行的时候，
 *			会挂载到队列中等待.
 * work_done		刷新进程等待头
 */
struct cpu_workqueue_struct {

	spinlock_t lock;

	long remove_sequence;	/* Least-recently added (next to run) */
	long insert_sequence;	/* Next to add */

	struct list_head worklist;
	wait_queue_head_t more_work;
	wait_queue_head_t work_done;

	struct workqueue_struct *wq;
	task_t *thread;

	int run_depth;		/* Detect run_workqueue() recursion depth */
} ____cacheline_aligned;

/*
 * The externally visible workqueue abstraction is an array of
 * per-CPU workqueues:
 */
struct workqueue_struct {
	struct cpu_workqueue_struct cpu_wq[NR_CPUS];
	const char *name;
	struct list_head list; 	/* Empty if single thread */
};

/*
 * All the per-cpu workqueues on the system, for hotplug cpu to add/remove
 * threads to each one as cpus come/go.
 */
/* 系统中所有的per-cpu 队列，用于热插拔CPU添加、删除线程 */
static DEFINE_SPINLOCK(workqueue_lock);
static LIST_HEAD(workqueues);

/* If it's single threaded, it isn't in the list of workqueues. */
static inline int is_single_threaded(struct workqueue_struct *wq)
{
	/* 没有加入到链表中 */
	return list_empty(&wq->list);
}

/* Preempt must be disabled. */
static void __queue_work(struct cpu_workqueue_struct *cwq,
			 struct work_struct *work)
{
	unsigned long flags;

	/* 关中断 */
	spin_lock_irqsave(&cwq->lock, flags);
	work->wq_data = cwq;
	list_add_tail(&work->entry, &cwq->worklist);
	cwq->insert_sequence++;
	/* 唤醒等待队列上的进程，起来该干活了 */
	wake_up(&cwq->more_work);
	/* 开中断 */
	spin_unlock_irqrestore(&cwq->lock, flags);
}

/*
 * Queue work on a workqueue. Return non-zero if it was successfully
 * added.
 *
 * We queue the work to the CPU it was submitted, but there is no
 * guarantee that it will be processed by that CPU.
 */
/* 将work放到工作队列中，成功返回非0 值 */
int fastcall queue_work(struct workqueue_struct *wq, struct work_struct *work)
{
	int ret = 0, cpu = get_cpu();

	if (!test_and_set_bit(0, &work->pending)) {
		if (unlikely(is_single_threaded(wq)))
			cpu = 0;
		BUG_ON(!list_empty(&work->entry));
		__queue_work(wq->cpu_wq + cpu, work);
		ret = 1;
	}
	put_cpu();
	return ret;
}

static void delayed_work_timer_fn(unsigned long __data)
{
	struct work_struct *work = (struct work_struct *)__data;
	struct workqueue_struct *wq = work->wq_data;
	int cpu = smp_processor_id();

	if (unlikely(is_single_threaded(wq)))
		cpu = 0;

	__queue_work(wq->cpu_wq + cpu, work);
}

/*
 * 等待一段时间之后执行，并不着急将work_struct{}加入到worklist中.
 * 先执行定时器，由定时器函数将该work_struct{}加入到队列中.
 */
int fastcall queue_delayed_work(struct workqueue_struct *wq,
			struct work_struct *work, unsigned long delay)
{
	int ret = 0;
	struct timer_list *timer = &work->timer;

	if (!test_and_set_bit(0, &work->pending)) {
		BUG_ON(timer_pending(timer));
		BUG_ON(!list_empty(&work->entry));

		/* This stores wq for the moment, for the timer_fn */
		work->wq_data = wq;
		timer->expires = jiffies + delay;
		timer->data = (unsigned long)work;
		timer->function = delayed_work_timer_fn;
		add_timer(timer);
		ret = 1;
	}
	return ret;
}

static inline void run_workqueue(struct cpu_workqueue_struct *cwq)
{
	unsigned long flags;

	/*
	 * Keep taking off work from the queue until
	 * done.
	 */
	spin_lock_irqsave(&cwq->lock, flags);
	cwq->run_depth++;
	if (cwq->run_depth > 3) {
		/* morton gets to eat his hat */
		printk("%s: recursion depth exceeded: %d\n",
			__FUNCTION__, cwq->run_depth);
		dump_stack();
	}
	while (!list_empty(&cwq->worklist)) {
		struct work_struct *work = list_entry(cwq->worklist.next,
						struct work_struct, entry);
		void (*f) (void *) = work->func;
		void *data = work->data;

		list_del_init(cwq->worklist.next);
		spin_unlock_irqrestore(&cwq->lock, flags);

		BUG_ON(work->wq_data != cwq);
		clear_bit(0, &work->pending);
		f(data);

		spin_lock_irqsave(&cwq->lock, flags);
		cwq->remove_sequence++;
		wake_up(&cwq->work_done);
	}
	cwq->run_depth--;
	spin_unlock_irqrestore(&cwq->lock, flags);
}

/* cpu_workqueue_struct{} 执行线程 */
static int worker_thread(void *__cwq)
{
	struct cpu_workqueue_struct *cwq = __cwq;
	DECLARE_WAITQUEUE(wait, current);
	struct k_sigaction sa;
	sigset_t blocked;

	current->flags |= PF_NOFREEZE;

	set_user_nice(current, -5);

	/* Block and flush all signals */
	sigfillset(&blocked);
	sigprocmask(SIG_BLOCK, &blocked, NULL);
	flush_signals(current);

	/* SIG_IGN makes children autoreap: see do_notify_parent(). */
	sa.sa.sa_handler = SIG_IGN;
	sa.sa.sa_flags = 0;
	siginitset(&sa.sa.sa_mask, sigmask(SIGCHLD));
	do_sigaction(SIGCHLD, &sa, (struct k_sigaction *)0);

	set_current_state(TASK_INTERRUPTIBLE);
	while (!kthread_should_stop()) {
		/* 添加到等待队列中 */
		add_wait_queue(&cwq->more_work, &wait);
		/* 如果此时队列为空，调度先去执行别的进程 */
		if (list_empty(&cwq->worklist))
			schedule();
		else
			__set_current_state(TASK_RUNNING);
		/* 等待队列中删除 */
		remove_wait_queue(&cwq->more_work, &wait);

		if (!list_empty(&cwq->worklist))
			run_workqueue(cwq);
		set_current_state(TASK_INTERRUPTIBLE);
	}
	__set_current_state(TASK_RUNNING);
	return 0;
}

static void flush_cpu_workqueue(struct cpu_workqueue_struct *cwq)
{
	if (cwq->thread == current) {
		/*
		 * Probably keventd trying to flush its own queue. So simply run
		 * it by hand rather than deadlocking.
		 */
		run_workqueue(cwq);
	} else {
		DEFINE_WAIT(wait);
		long sequence_needed;

		spin_lock_irq(&cwq->lock);
		sequence_needed = cwq->insert_sequence;

		/* 循环执行，直到remove_sequence >= insert_sequence */
		while (sequence_needed - cwq->remove_sequence > 0) {
			/* 挂载到队列中 */
			prepare_to_wait(&cwq->work_done, &wait,
					TASK_UNINTERRUPTIBLE);
			spin_unlock_irq(&cwq->lock);
			/*
			 * 调度出去执行，执行到worker_thread()->run_workqueue()
			 * 会唤醒该进程.
			 * 唤醒之后重新检查，如果仍然不符合条件，继续等待.
			 */
			schedule();
			spin_lock_irq(&cwq->lock);
		}
		/* 结束等待 */
		finish_wait(&cwq->work_done, &wait);
		spin_unlock_irq(&cwq->lock);
	}
}

/*
 * flush_workqueue - ensure that any scheduled work has run to completion.
 *
 * Forces execution of the workqueue and blocks until its completion.
 * This is typically used in driver shutdown handlers.
 *
 * This function will sample each workqueue's current insert_sequence number and
 * will sleep until the head sequence is greater than or equal to that.  This
 * means that we sleep until all works which were queued on entry have been
 * handled, but we are not livelocked by new incoming ones.
 *
 * This function used to run the workqueues itself.  Now we just wait for the
 * helper threads to do it.
 */
/*
 * 刷新动作，确认之前加入的work_struct{}结束.
 * 如果此时一直处理新加入的work_struct{}可能会导致一直在该函数运行(livelocked)，
 * 所以此处仅仅会处理在调用之前就已经加入的work_struct{}.
 *
 * 也就是，无法保证加入的work_struct{}都会被执行.
 */
void fastcall flush_workqueue(struct workqueue_struct *wq)
{
	/* 可能会引发进程调度 */
	might_sleep();

	if (is_single_threaded(wq)) {
		/* Always use cpu 0's area. */
		flush_cpu_workqueue(wq->cpu_wq + 0);
	} else {
		int cpu;

		lock_cpu_hotplug();
		for_each_online_cpu(cpu)
			flush_cpu_workqueue(wq->cpu_wq + cpu);
		unlock_cpu_hotplug();
	}
}

static struct task_struct *create_workqueue_thread(struct workqueue_struct *wq,
						   int cpu)
{
	struct cpu_workqueue_struct *cwq = wq->cpu_wq + cpu;
	struct task_struct *p;

	spin_lock_init(&cwq->lock);
	cwq->wq = wq;
	cwq->thread = NULL;
	cwq->insert_sequence = 0;
	cwq->remove_sequence = 0;
	INIT_LIST_HEAD(&cwq->worklist);
	init_waitqueue_head(&cwq->more_work);
	init_waitqueue_head(&cwq->work_done);

	if (is_single_threaded(wq))
		p = kthread_create(worker_thread, cwq, "%s", wq->name);
	else
		p = kthread_create(worker_thread, cwq, "%s/%d", wq->name, cpu);
	if (IS_ERR(p))
		return NULL;
	cwq->thread = p;
	return p;
}

struct workqueue_struct *__create_workqueue(const char *name,
					    int singlethread)
{
	int cpu, destroy = 0;
	struct workqueue_struct *wq;
	struct task_struct *p;

	BUG_ON(strlen(name) > 10);

	wq = kmalloc(sizeof(*wq), GFP_KERNEL);
	if (!wq)
		return NULL;
	memset(wq, 0, sizeof(*wq));

	/* 设置名称 */
	wq->name = name;
	/* We don't need the distraction of CPUs appearing and vanishing. */
	lock_cpu_hotplug();
	/* 是否是单线程 */
	if (singlethread) {
		INIT_LIST_HEAD(&wq->list);
		p = create_workqueue_thread(wq, 0);
		if (!p)
			destroy = 1;
		else
			wake_up_process(p);
	} else {
		spin_lock(&workqueue_lock);
		/* 不是单线程需要加入到队列中 */
		list_add(&wq->list, &workqueues);
		spin_unlock(&workqueue_lock);
		for_each_online_cpu(cpu) {
			p = create_workqueue_thread(wq, cpu);
			if (p) {
				kthread_bind(p, cpu);
				wake_up_process(p);
			} else
				destroy = 1;
		}
	}
	unlock_cpu_hotplug();

	/*
	 * Was there any error during startup? If yes then clean up:
	 */
	if (destroy) {
		destroy_workqueue(wq);
		wq = NULL;
	}
	return wq;
}

static void cleanup_workqueue_thread(struct workqueue_struct *wq, int cpu)
{
	struct cpu_workqueue_struct *cwq;
	unsigned long flags;
	struct task_struct *p;

	cwq = wq->cpu_wq + cpu;
	spin_lock_irqsave(&cwq->lock, flags);
	p = cwq->thread;
	cwq->thread = NULL;
	spin_unlock_irqrestore(&cwq->lock, flags);
	/* 结束进程 */
	if (p)
		kthread_stop(p);
}

void destroy_workqueue(struct workqueue_struct *wq)
{
	int cpu;

	flush_workqueue(wq);

	/* We don't need the distraction of CPUs appearing and vanishing. */
	lock_cpu_hotplug();
	if (is_single_threaded(wq))
		cleanup_workqueue_thread(wq, 0);
	else {
		for_each_online_cpu(cpu)
			cleanup_workqueue_thread(wq, cpu);
		spin_lock(&workqueue_lock);
		list_del(&wq->list);
		spin_unlock(&workqueue_lock);
	}
	unlock_cpu_hotplug();
	kfree(wq);
}

static struct workqueue_struct *keventd_wq;

int fastcall schedule_work(struct work_struct *work)
{
	return queue_work(keventd_wq, work);
}

int fastcall schedule_delayed_work(struct work_struct *work, unsigned long delay)
{
	return queue_delayed_work(keventd_wq, work, delay);
}

/* 将work_struct{} 放到特定核上执行 */
int schedule_delayed_work_on(int cpu,
			struct work_struct *work, unsigned long delay)
{
	int ret = 0;
	struct timer_list *timer = &work->timer;

	if (!test_and_set_bit(0, &work->pending)) {
		BUG_ON(timer_pending(timer));
		BUG_ON(!list_empty(&work->entry));
		/* This stores keventd_wq for the moment, for the timer_fn */
		work->wq_data = keventd_wq;
		timer->expires = jiffies + delay;
		timer->data = (unsigned long)work;
		timer->function = delayed_work_timer_fn;
		add_timer_on(timer, cpu);
		ret = 1;
	}
	return ret;
}

void flush_scheduled_work(void)
{
	flush_workqueue(keventd_wq);
}

/**
 * cancel_rearming_delayed_workqueue - reliably kill off a delayed
 *			work whose handler rearms the delayed work.
 * @wq:   the controlling workqueue structure
 * @work: the delayed work struct
 */
void cancel_rearming_delayed_workqueue(struct workqueue_struct *wq,
				       struct work_struct *work)
{
	while (!cancel_delayed_work(work))
		flush_workqueue(wq);
}
EXPORT_SYMBOL(cancel_rearming_delayed_workqueue);

/**
 * cancel_rearming_delayed_work - reliably kill off a delayed keventd
 *			work whose handler rearms the delayed work.
 * @work: the delayed work struct
 */
void cancel_rearming_delayed_work(struct work_struct *work)
{
	cancel_rearming_delayed_workqueue(keventd_wq, work);
}
EXPORT_SYMBOL(cancel_rearming_delayed_work);

int keventd_up(void)
{
	return keventd_wq != NULL;
}

int current_is_keventd(void)
{
	struct cpu_workqueue_struct *cwq;
	int cpu = smp_processor_id();	/* preempt-safe: keventd is per-cpu */
	int ret = 0;

	BUG_ON(!keventd_wq);

	cwq = keventd_wq->cpu_wq + cpu;
	if (current == cwq->thread)
		ret = 1;

	return ret;

}

#ifdef CONFIG_HOTPLUG_CPU
/* Take the work from this (downed) CPU. */
static void take_over_work(struct workqueue_struct *wq, unsigned int cpu)
{
	struct cpu_workqueue_struct *cwq = wq->cpu_wq + cpu;
	LIST_HEAD(list);
	struct work_struct *work;

	spin_lock_irq(&cwq->lock);
	list_splice_init(&cwq->worklist, &list);

	while (!list_empty(&list)) {
		printk("Taking work for %s\n", wq->name);
		work = list_entry(list.next,struct work_struct,entry);
		list_del(&work->entry);
		__queue_work(wq->cpu_wq + smp_processor_id(), work);
	}
	spin_unlock_irq(&cwq->lock);
}

/* We're holding the cpucontrol mutex here */
static int __devinit workqueue_cpu_callback(struct notifier_block *nfb,
				  unsigned long action,
				  void *hcpu)
{
	unsigned int hotcpu = (unsigned long)hcpu;
	struct workqueue_struct *wq;

	switch (action) {
	case CPU_UP_PREPARE:
		/* Create a new workqueue thread for it. */
		list_for_each_entry(wq, &workqueues, list) {
			if (create_workqueue_thread(wq, hotcpu) < 0) {
				printk("workqueue for %i failed\n", hotcpu);
				return NOTIFY_BAD;
			}
		}
		break;

	case CPU_ONLINE:
		/* Kick off worker threads. */
		list_for_each_entry(wq, &workqueues, list) {
			kthread_bind(wq->cpu_wq[hotcpu].thread, hotcpu);
			wake_up_process(wq->cpu_wq[hotcpu].thread);
		}
		break;

	case CPU_UP_CANCELED:
		list_for_each_entry(wq, &workqueues, list) {
			/* Unbind so it can run. */
			kthread_bind(wq->cpu_wq[hotcpu].thread,
				     smp_processor_id());
			cleanup_workqueue_thread(wq, hotcpu);
		}
		break;

	case CPU_DEAD:
		list_for_each_entry(wq, &workqueues, list)
			cleanup_workqueue_thread(wq, hotcpu);
		list_for_each_entry(wq, &workqueues, list)
			take_over_work(wq, hotcpu);
		break;
	}

	return NOTIFY_OK;
}
#endif

void init_workqueues(void)
{
	hotcpu_notifier(workqueue_cpu_callback, 0);
	keventd_wq = create_workqueue("events");
	BUG_ON(!keventd_wq);
}

EXPORT_SYMBOL_GPL(__create_workqueue);
EXPORT_SYMBOL_GPL(queue_work);
EXPORT_SYMBOL_GPL(queue_delayed_work);
EXPORT_SYMBOL_GPL(flush_workqueue);
EXPORT_SYMBOL_GPL(destroy_workqueue);

EXPORT_SYMBOL(schedule_work);
EXPORT_SYMBOL(schedule_delayed_work);
EXPORT_SYMBOL(schedule_delayed_work_on);
EXPORT_SYMBOL(flush_scheduled_work);
