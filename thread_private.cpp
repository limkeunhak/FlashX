#include <sys/time.h>
#include <sys/types.h>
#include <sys/syscall.h>
#define gettid() syscall(__NR_gettid)

#include "thread_private.h"

#define BULK_SIZE 1000

#define NUM_PAGES (40960 * nthreads)

extern bool verify_read_content;

void check_read_content(char *buf, int size, off_t off)
{
	// I assume the space in the buffer is larger than 8 bytes.
	off_t aligned_off = off & (~(sizeof(off_t) - 1));
	long data[2];
	data[0] = aligned_off / sizeof(off_t);
	data[1] = aligned_off / sizeof(off_t) + 1;
	long expected = 0;
	int copy_size = size < (int) sizeof(off_t) ? size : (int) sizeof(off_t);
	memcpy(&expected, ((char *) data) + (off - aligned_off), copy_size);
	long read_value = 0;
	memcpy(&read_value, buf, copy_size);
	if(read_value != expected)
		printf("%ld %ld\n", read_value, expected);
	assert(read_value == expected);
}

class cleanup_callback: public callback
{
	rand_buf *buf;
	ssize_t read_bytes;
	int thread_id;
public:
	cleanup_callback(rand_buf *buf, int idx) {
		this->buf = buf;
		read_bytes = 0;
		this->thread_id = idx;
	}

	int invoke(io_request *rq) {
		extern bool verify_read_content;
		if (rq->get_access_method() == READ && verify_read_content) {
			check_read_content(rq->get_buf(), rq->get_size(), rq->get_offset());
		}
		buf->free_entry(rq->get_buf());
		read_bytes += rq->get_size();
		return 0;
	}

	ssize_t get_size() {
		return read_bytes;
	}
};

ssize_t thread_private::get_read_bytes() {
	if (cb)
		return cb->get_size();
	else
		return read_bytes;
}

int thread_private::thread_init() {
	attach2cpu();
	io->init();

	rand_buf *buf = new rand_buf(NUM_PAGES / (nthreads
				// TODO maybe I should set the right entry size for a buffer.
				// If each access size is irregular, I'll break each access
				// into pages so each access is no larger than a page, so it
				// should workl fine.
				/ NUM_NODES) * PAGE_SIZE, PAGE_SIZE);
	this->buf = buf;
	if (io->support_aio()) {
		cb = new cleanup_callback(buf, idx);
		io->set_callback(cb);
	}
	return 0;
}

int thread_private::run()
{
	ssize_t ret = -1;
	gettimeofday(&start_time, NULL);
	int reqs_capacity = BULK_SIZE * 2;
	io_request *reqs = (io_request *) malloc(sizeof(io_request) * reqs_capacity);
	while (gen->has_next()) {
		if (io->support_aio()) {
			int i;
//			io_request *reqs = gc->allocate_obj(BULK_SIZE);
			for (i = 0; i < BULK_SIZE && gen->has_next(); ) {
//				printf("thread %d: allocate %p\n", idx, p);
				// TODO right now it only support read.
				workload_t workload = gen->next();
				// TODO let's read data first;
				int access_method = workload.read ? READ : WRITE;
				off_t off = workload.off;
				int size = workload.size;
				while (size > 0) {
					off_t next_off = ROUNDUP_PAGE(off + 1);
					if (next_off > off + size)
						next_off = off + size;
					// This is a very hacking way to handle the case that one access
					// is broken into multiple requests. It works because the array for
					// storing requests are twice as large as needed.
					assert (i < reqs_capacity);
					char *p = buf->next_entry();
					reqs[i].init(p, off, next_off - off, access_method, io);
					size -= next_off - off;
					off = next_off;
					i++;
				}
			}
			ret = io->access(reqs, i);
			if (ret < 0) {
				perror("access_vector");
				exit(1);
			}
		}
		else {
			workload_t workload = gen->next();
			off_t off = workload.off;
			// TODO let's just read data first.
			int access_method = workload.read ? READ : WRITE;
			int entry_size = workload.size;

			while (entry_size > 0) {
				char *entry = buf->next_entry();
				/*
				 * generate the data for writing the file,
				 * so the data in the file isn't changed.
				 */
				if (access_method == WRITE) {
					unsigned long *p = (unsigned long *) entry;
					long start = off / sizeof(long);
					for (unsigned int i = 0; i < entry_size / sizeof(*p); i++)
						p[i] = start++;
				}
				// There is at least one byte we need to access in the page.
				// By adding 1 and rounding up the offset, we'll get the next page
				// behind the current offset.
				off_t next_off = ROUNDUP_PAGE(off + 1);
				if (next_off > off + entry_size)
					next_off = off + entry_size;
				ret = io->access(entry, off, next_off - off, access_method);
				if (ret > 0) {
					if (access_method == READ && verify_read_content) {
						check_read_content(entry, next_off - off, off);
					}
					read_bytes += ret;
				}
				buf->free_entry(entry);
				if (ret < 0) {
					perror("access");
					exit(1);
				}
				entry_size -= next_off - off;
				off = next_off;
			}
		}
	}
	io->cleanup();
	printf("thread %d exits\n", idx);
	gettimeofday(&end_time, NULL);
	free(reqs);
	return 0;
}

int thread_private::attach2cpu()
{
#if NCPUS > 0
	cpu_set_t cpuset;
	pthread_t thread = pthread_self();
	CPU_ZERO(&cpuset);
	int cpu_num = idx % NCPUS;
	CPU_SET(cpu_num, &cpuset);
	int ret = pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
	if (ret != 0) {
		perror("pthread_setaffinity_np");
		exit(1);
	}
	printf("attach thread %d to CPU %d\n", idx, cpu_num);
	return ret;
#else
	return -1;
#endif
}

static void *rand_read(void *arg)
{
	thread_private *priv = (thread_private *) arg;

	printf("rand_read: pid: %d, tid: %ld\n", getpid(), gettid());
	priv->thread_init();
	priv->run();
	return NULL;
}

#ifdef USE_PROCESS
static int process_create(pid_t *pid, void (*func)(void *), void *priv)
{
	pid_t id = fork();

	if (id < 0)
		return -1;

	if (id == 0) {	// child
		func(priv);
		exit(0);
	}

	if (id > 0)
		*pid = id;
	return 0;
}

static int process_join(pid_t pid)
{
	int status;
	pid_t ret = waitpid(pid, &status, 0);
	return ret < 0 ? ret : 0;
}
#endif

int thread_private::start_thread()
{
	int ret;
#ifdef USE_PROCESS
	ret = process_create(&id, rand_read, (void *) this);
#else
	ret = pthread_create(&id, NULL, rand_read, (void *) this);
#endif
	return ret;
}

int thread_private::wait_thread_end()
{
	int ret;
#ifdef USE_PROCESS
	ret = process_join(id);
#else
	ret = pthread_join(id, NULL);
#endif
	return ret;
}
