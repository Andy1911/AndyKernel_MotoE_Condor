/*
 * Zenplus I/O scheduler
 * based on zen and sio I/O schedulers.
 *
 * Copyright (C) 2012 Brandon Berhent <bbedward@gmail.com>
 * Copyright (C) 2017 Ryan Andri <ryan.omnia@gmail.com>
 *
 * FCFS, dispatches are back-inserted, async and synchronous for relay
 * on deadlines to ensure fairness. Should work best with devices where
 * there is no travel delay such as SSD.
 */
#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>

static const int sync_read_expire  = HZ / 4;	/* max time before a sync read is submitted. */
static const int sync_write_expire = 2 * HZ;	/* max time before a sync write is submitted. */

static const int async_read_expire  =  4 * HZ;	/* ditto for async, these limits are SOFT! */
static const int async_write_expire = 16 * HZ;	/* ditto for async, these limits are SOFT! */

static const int writes_starved = 4;	   /* max times reads can starve a write */
static const int fifo_batch = 8;	   /* # of sequential requests treated as one
					      by the above parameters. For throughput. */

enum zenplus_data_dir { ASYNC, SYNC };

struct zenplus_data {
	struct list_head fifo_list[2][2];

	unsigned int batching;
	unsigned int starved;

	/* tunables */
	int fifo_expire[2][2];
	int fifo_batch;
	int writes_starved;
};

static inline struct zenplus_data *
zenplus_get_data(struct request_queue *q) {
	return q->elevator->elevator_data;
}

static void zenplus_dispatch(struct zenplus_data *, struct request *);

static void
zenplus_merged_requests(struct request_queue *q, struct request *rq,
		    struct request *next)
{
	/*
	 * if next expires before rq, assign its expire time to arq
	 * and move into next position (next will be deleted) in fifo
	 */
	if (!list_empty(&rq->queuelist) && !list_empty(&next->queuelist)) {
		if (time_before(rq_fifo_time(next), rq_fifo_time(rq))) {
			list_move(&rq->queuelist, &next->queuelist);
			rq_set_fifo_time(rq, rq_fifo_time(next));
		}
	}

	/* next request is gone */
	rq_fifo_clear(next);
}

static void zenplus_add_request(struct request_queue *q, struct request *req)
{
	struct zenplus_data *znp = zenplus_get_data(q);
	const int dir = rq_data_dir(req);
	const int sync = rq_is_sync(req);

	if (znp->fifo_expire[sync][dir]) {
		rq_set_fifo_time(req, jiffies + znp->fifo_expire[sync][dir]);
		list_add_tail(&req->queuelist, &znp->fifo_list[sync][dir]);
	}
}

static void zenplus_dispatch(struct zenplus_data *znp, struct request *req)
{
	/* Remove request from list and dispatch it */
	rq_fifo_clear(req);
	elv_dispatch_add_tail(req->q, req);

	/* Increment # of sequential requests */
	znp->batching++;

	if (rq_data_dir(req))
		znp->starved = 0;
	else
		znp->starved++;
}

static struct request *
zenplus_expired_request(struct zenplus_data *znp, int sync, int data_dir)
{
	struct request *req;

	if (list_empty(&znp->fifo_list[sync][data_dir]))
		return NULL;

	/* Retrieve request */
	req = rq_entry_fifo(znp->fifo_list[sync][data_dir].next);

	/* Request has expired */
	if (time_after(jiffies, rq_fifo_time(req)))
		return req;

	return NULL;
}

static struct request *
zenplus_choose_expired_request(struct zenplus_data *znp)
{
	struct request *sync_req_read = zenplus_expired_request(znp, SYNC, READ);
	struct request *sync_req_write = zenplus_expired_request(znp, SYNC, WRITE);
	struct request *async_req_read = zenplus_expired_request(znp, ASYNC, READ);
	struct request *async_req_write = zenplus_expired_request(znp, ASYNC, WRITE);

	if (async_req_read && sync_req_read)
	{
		if (time_after(rq_fifo_time(async_req_read),
				rq_fifo_time(sync_req_read)))
			return sync_req_read;
		else if (time_after(rq_fifo_time(sync_req_read),
				rq_fifo_time(async_req_read)))
			return async_req_read;
	}
	else if (async_req_write && sync_req_write)
	{
		if (time_after(rq_fifo_time(async_req_write),
				rq_fifo_time(sync_req_write)))
			return sync_req_write;
		else if (time_after(rq_fifo_time(sync_req_write),
				rq_fifo_time(async_req_write)))
			return async_req_write;
	}
	else if (sync_req_read)
	{
		return sync_req_read;
	}
	else if (sync_req_write)
	{
		return sync_req_write;
	}
	else if (async_req_read)
	{
		return async_req_read;
	}
	else if (async_req_write)
	{
		return async_req_write;
	}

	return NULL;
}

static struct request *
zenplus_choose_request(struct zenplus_data *znp, int data_dir)
{
	struct list_head *sync = znp->fifo_list[SYNC];
	struct list_head *async = znp->fifo_list[ASYNC];

	/*
	 * Retrieve request from available fifo list.
	 * Synchronous requests have priority over asynchronous.
	 * Read requests have priority over write.
	 */
	if (!list_empty(&sync[data_dir]))
		return rq_entry_fifo(sync[data_dir].next);
	if (!list_empty(&async[data_dir]))
		return rq_entry_fifo(async[data_dir].next);

	if (!list_empty(&sync[!data_dir]))
		return rq_entry_fifo(sync[!data_dir].next);
	if (!list_empty(&async[!data_dir]))
		return rq_entry_fifo(async[!data_dir].next);

	return NULL;
}

static int zenplus_dispatch_requests(struct request_queue *q, int force)
{
	struct zenplus_data *znp = zenplus_get_data(q);
	struct request *rq = NULL;
	int data_dir = READ;

	/* Check for and issue expired requests */
	if (znp->batching > znp->fifo_batch) {
		znp->batching = 0;
		rq = zenplus_choose_expired_request(znp);
	}

	if (!rq) {
		if (znp->starved > znp->writes_starved)
			data_dir = WRITE;

		rq = zenplus_choose_request(znp, data_dir);
		if (!rq)
			return 0;
	}

	zenplus_dispatch(znp, rq);

	return 1;
}

static void *zenplus_init_queue(struct request_queue *q)
{
	struct zenplus_data *znp;

	znp = kmalloc_node(sizeof(*znp), GFP_KERNEL, q->node);
	if (!znp)
		return NULL;

	INIT_LIST_HEAD(&znp->fifo_list[SYNC][READ]);
	INIT_LIST_HEAD(&znp->fifo_list[SYNC][WRITE]);
	INIT_LIST_HEAD(&znp->fifo_list[ASYNC][READ]);
	INIT_LIST_HEAD(&znp->fifo_list[ASYNC][WRITE]);

	znp->batching = 0;
	znp->fifo_expire[SYNC][READ] = sync_read_expire;
	znp->fifo_expire[SYNC][WRITE] = sync_write_expire;
	znp->fifo_expire[ASYNC][READ] = async_read_expire;
	znp->fifo_expire[ASYNC][WRITE] = async_write_expire;
	znp->fifo_batch = fifo_batch;
	znp->writes_starved = writes_starved;

	return znp;
}

static void zenplus_exit_queue(struct elevator_queue *e)
{
	struct zenplus_data *znp = e->elevator_data;

	BUG_ON(!list_empty(&znp->fifo_list[SYNC][READ]));
	BUG_ON(!list_empty(&znp->fifo_list[SYNC][WRITE]));
	BUG_ON(!list_empty(&znp->fifo_list[ASYNC][READ]));
	BUG_ON(!list_empty(&znp->fifo_list[ASYNC][WRITE]));

	kfree(znp);
}

/* Sysfs */
static ssize_t
zenplus_var_show(int var, char *page)
{
	return sprintf(page, "%d\n", var);
}

static ssize_t
zenplus_var_store(int *var, const char *page, size_t count)
{
	*var = simple_strtol(page, NULL, 10);
	return count;
}

#define SHOW_FUNCTION(__FUNC, __VAR, __CONV)			\
static ssize_t __FUNC(struct elevator_queue *e, char *page)	\
{								\
	struct zenplus_data *znp = e->elevator_data;		\
	int __data = __VAR;					\
	if (__CONV)						\
		__data = jiffies_to_msecs(__data);		\
		return zenplus_var_show(__data, (page));		\
}
SHOW_FUNCTION(zenplus_sync_read_expire_show, znp->fifo_expire[SYNC][READ], 1);
SHOW_FUNCTION(zenplus_sync_write_expire_show, znp->fifo_expire[SYNC][WRITE], 1);
SHOW_FUNCTION(zenplus_async_read_expire_show, znp->fifo_expire[ASYNC][READ], 1);
SHOW_FUNCTION(zenplus_async_write_expire_show, znp->fifo_expire[ASYNC][WRITE], 1);
SHOW_FUNCTION(zenplus_fifo_batch_show, znp->fifo_batch, 0);
SHOW_FUNCTION(zenplus_writes_starved_show, znp->writes_starved, 0);
#undef SHOW_FUNCTION

#define STORE_FUNCTION(__FUNC, __PTR, MIN, MAX, __CONV)				\
static ssize_t __FUNC(struct elevator_queue *e, const char *page, size_t count) \
{										\
	struct zenplus_data *znp = e->elevator_data;				\
	int __data;								\
	int ret = zenplus_var_store(&__data, (page), count);			\
	if (__data < (MIN))							\
		__data = (MIN);							\
	else if (__data > (MAX))						\
		__data = (MAX);							\
	if (__CONV)								\
		*(__PTR) = msecs_to_jiffies(__data);				\
	else									\
		*(__PTR) = __data;						\
	return ret;								\
}
STORE_FUNCTION(zenplus_sync_read_expire_store, &znp->fifo_expire[SYNC][READ], 0, INT_MAX, 1);
STORE_FUNCTION(zenplus_sync_write_expire_store, &znp->fifo_expire[SYNC][WRITE], 0, INT_MAX, 1);
STORE_FUNCTION(zenplus_async_read_expire_store, &znp->fifo_expire[ASYNC][READ], 0, INT_MAX, 1);
STORE_FUNCTION(zenplus_async_write_expire_store, &znp->fifo_expire[ASYNC][WRITE], 0, INT_MAX, 1);
STORE_FUNCTION(zenplus_fifo_batch_store, &znp->fifo_batch, 0, INT_MAX, 0);
STORE_FUNCTION(zenplus_writes_starved_store, &znp->writes_starved, 0, INT_MAX, 0);
#undef STORE_FUNCTION

#define DD_ATTR(name)						\
	__ATTR(name, S_IRUGO|S_IWUSR, zenplus_##name##_show,	\
				      zenplus_##name##_store)

static struct elv_fs_entry zenplus_attrs[] = {
	DD_ATTR(sync_read_expire),
	DD_ATTR(sync_write_expire),
	DD_ATTR(async_read_expire),
	DD_ATTR(async_write_expire),
	DD_ATTR(fifo_batch),
	DD_ATTR(writes_starved),
	__ATTR_NULL
};

static struct elevator_type iosched_zenplus = {
	.ops = {
		.elevator_merge_req_fn		= zenplus_merged_requests,
		.elevator_dispatch_fn		= zenplus_dispatch_requests,
		.elevator_add_req_fn		= zenplus_add_request,
		.elevator_former_req_fn		= elv_rb_former_request,
		.elevator_latter_req_fn		= elv_rb_latter_request,
		.elevator_init_fn		= zenplus_init_queue,
		.elevator_exit_fn		= zenplus_exit_queue,
	},
	.elevator_attrs = zenplus_attrs,
	.elevator_name = "zenplus",
	.elevator_owner = THIS_MODULE,
};

static int __init zenplus_init(void)
{
	elv_register(&iosched_zenplus);

	return 0;
}

static void __exit zenplus_exit(void)
{
	elv_unregister(&iosched_zenplus);
}

module_init(zenplus_init);
module_exit(zenplus_exit);

MODULE_AUTHOR("Brandon Berhent");
MODULE_AUTHOR("Ryan Andri a.k.a Rainforce279");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Zenplus IO scheduler");
MODULE_VERSION("0.1");
