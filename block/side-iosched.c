/*
 * Side IO scheduler
 * Deadline and Zen IO schedulers.
 *
 * Copyright (C) 2017 Ryan Andri <ryan.omnia@gmail.com>
 *
 * The simple version of deadline I/O Scheduler
 */
#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>

static const int read_expire = HZ / 4;  /* max time before a read is submitted. */
static const int write_expire = 2 * HZ; /* ditto for writes, these limits are SOFT! */
static const int fifo_batch = 16;

struct side_data {
	/* Runtime Data */
	/* Requests are only present on fifo_list */
	struct list_head fifo_list[2];

	unsigned int batching;		/* number of sequential requests made */

	/* tunables */
	int fifo_expire[2];
	int fifo_batch;
};

static inline struct side_data *
side_get_data(struct request_queue *q) {
	return q->elevator->elevator_data;
}

static void side_dispatch(struct side_data *, struct request *);

static void
side_merged_requests(struct request_queue *q, struct request *rq,
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

static void side_add_request(struct request_queue *q, struct request *rq)
{
	struct side_data *sdata = side_get_data(q);
	const int dir = rq_data_dir(rq);

	rq_set_fifo_time(rq, jiffies + sdata->fifo_expire[dir]);
	list_add_tail(&rq->queuelist, &sdata->fifo_list[dir]);
}

static void side_dispatch(struct side_data *sdata, struct request *rq)
{
	/* Remove request from list and dispatch it */
	rq_fifo_clear(rq);
	elv_dispatch_add_tail(rq->q, rq);

	/* Increment # of sequential requests */
	sdata->batching++;
}

/*
 * get the first expired request in direction ddir
 */
static struct request *
side_expired_request(struct side_data *sdata, int ddir)
{
	struct request *rq;

	if (list_empty(&sdata->fifo_list[ddir]))
		return NULL;

	rq = rq_entry_fifo(sdata->fifo_list[ddir].next);
	if (time_after(jiffies, rq_fifo_time(rq)))
		return rq;

	return NULL;
}

/*
 * side_check_fifo returns 0 if there are no expired requests on the fifo,
 * otherwise it returns the next expired request
 */
static struct request *
side_check_fifo(struct side_data *sdata)
{
	struct request *rq_read = side_expired_request(sdata, READ);
	struct request *rq_write = side_expired_request(sdata, WRITE);

	if (rq_read && rq_write) {
		if (time_after(rq_fifo_time(rq_write), rq_fifo_time(rq_read)))
			return rq_read;
	}
	else if (rq_read)
	{
		return rq_read;
	}
	else if (rq_write)
	{
		return rq_write;
	}

	return 0;
}

static struct request *
side_choose_request(struct side_data *sdata)
{
	/*
	 * Retrieve request from available fifo list.
	 * Synchronous requests have priority over asynchronous.
	 */
	if (!list_empty(&sdata->fifo_list[READ]))
		return rq_entry_fifo(sdata->fifo_list[READ].next);
	if (!list_empty(&sdata->fifo_list[WRITE]))
		return rq_entry_fifo(sdata->fifo_list[WRITE].next);

	return NULL;
}

static int side_dispatch_requests(struct request_queue *q, int force)
{
	struct side_data *sdata = side_get_data(q);
	struct request *rq = NULL;

	/* Check for and issue expired requests */
	if (sdata->batching > sdata->fifo_batch) {
		sdata->batching = 0;
		rq = side_check_fifo(sdata);
	}

	if (!rq) {
		rq = side_choose_request(sdata);
		if (!rq)
			return 0;
	}

	side_dispatch(sdata, rq);

	return 1;
}

static struct request * side_former_request(struct request_queue *q, struct request *rq)
{
	struct side_data *sdata = side_get_data(q);
	const int ddir = rq_data_dir(rq);

	if (rq->queuelist.prev == &sdata->fifo_list[ddir])
		return NULL;

	/* Return former request */
	return list_entry(rq->queuelist.prev, struct request, queuelist);
}

static struct request * side_latter_request(struct request_queue *q, struct request *rq)
{
	struct side_data *sdata = side_get_data(q);
	const int ddir = rq_data_dir(rq);

	if (rq->queuelist.next == &sdata->fifo_list[ddir])
		return NULL;

	/* Return latter request */
	return list_entry(rq->queuelist.next, struct request, queuelist);
}

static void *side_init_queue(struct request_queue *q)
{
	struct side_data *sdata;

	sdata = kmalloc_node(sizeof(*sdata), GFP_KERNEL, q->node);
	if (!sdata)
		return NULL;

	INIT_LIST_HEAD(&sdata->fifo_list[READ]);
	INIT_LIST_HEAD(&sdata->fifo_list[WRITE]);

	sdata->fifo_expire[READ] = read_expire;
	sdata->fifo_expire[WRITE] = write_expire;
	sdata->fifo_batch = fifo_batch;

	return sdata;
}

static void side_exit_queue(struct elevator_queue *e)
{
	struct side_data *sdata = e->elevator_data;

	BUG_ON(!list_empty(&sdata->fifo_list[READ]));
	BUG_ON(!list_empty(&sdata->fifo_list[WRITE]));
	kfree(sdata);
}

/* Sysfs */
static ssize_t
side_var_show(int var, char *page)
{
	return sprintf(page, "%d\n", var);
}

static ssize_t
side_var_store(int *var, const char *page, size_t count)
{
	*var = simple_strtol(page, NULL, 10);
	return count;
}

#define SHOW_FUNCTION(__FUNC, __VAR, __CONV) \
static ssize_t __FUNC(struct elevator_queue *e, char *page) \
{ \
	struct side_data *sdata = e->elevator_data; \
	int __data = __VAR; \
	if (__CONV) \
		__data = jiffies_to_msecs(__data); \
		return side_var_show(__data, (page)); \
}
SHOW_FUNCTION(side_read_expire_show, sdata->fifo_expire[READ], 1);
SHOW_FUNCTION(side_write_expire_show, sdata->fifo_expire[WRITE], 1);
SHOW_FUNCTION(side_fifo_batch_show, sdata->fifo_batch, 0);
#undef SHOW_FUNCTION

#define STORE_FUNCTION(__FUNC, __PTR, MIN, MAX, __CONV) \
static ssize_t __FUNC(struct elevator_queue *e, const char *page, size_t count) \
{ \
	struct side_data *sdata = e->elevator_data; \
	int __data; \
	int ret = side_var_store(&__data, (page), count); \
	if (__data < (MIN)) \
		__data = (MIN); \
	else if (__data > (MAX)) \
		__data = (MAX); \
	if (__CONV) \
		*(__PTR) = msecs_to_jiffies(__data); \
	else \
		*(__PTR) = __data; \
	return ret; \
}
STORE_FUNCTION(side_read_expire_store, &sdata->fifo_expire[READ], 0, INT_MAX, 1);
STORE_FUNCTION(side_write_expire_store, &sdata->fifo_expire[WRITE], 0, INT_MAX, 1);
STORE_FUNCTION(side_fifo_batch_store, &sdata->fifo_batch, 0, INT_MAX, 0);
#undef STORE_FUNCTION

#define DD_ATTR(name) \
	__ATTR(name, S_IRUGO|S_IWUSR, side_##name##_show, \
				      side_##name##_store)

static struct elv_fs_entry side_attrs[] = {
	DD_ATTR(read_expire),
	DD_ATTR(write_expire),
	DD_ATTR(fifo_batch),
	__ATTR_NULL
};

static struct elevator_type iosched_side = {
	.ops = {
		.elevator_merge_req_fn		= side_merged_requests,
		.elevator_dispatch_fn		= side_dispatch_requests,
		.elevator_add_req_fn		= side_add_request,
		.elevator_former_req_fn		= side_former_request,
		.elevator_latter_req_fn		= side_latter_request,
		.elevator_init_fn		= side_init_queue,
		.elevator_exit_fn		= side_exit_queue,
	},
	.elevator_attrs = side_attrs,
	.elevator_name = "side",
	.elevator_owner = THIS_MODULE,
};

static int __init side_init(void)
{
	elv_register(&iosched_side);

	return 0;
}

static void __exit side_exit(void)
{
	elv_unregister(&iosched_side);
}

module_init(side_init);
module_exit(side_exit);

MODULE_AUTHOR("Ryan Andri");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Side 'Simple Deadline' IO scheduler");
MODULE_VERSION("1.0");
