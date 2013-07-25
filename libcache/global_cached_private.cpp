#include <tr1/unordered_map>
#include <vector>

#include "global_cached_private.h"
#include "container.cpp"
#include "flush_thread.h"

// TODO I assume the block size of the RAID array is 16 pages.
const int RAID_BLOCK_SIZE = 16 * PAGE_SIZE;

#define ENABLE_LARGE_WRITE
//#define TEST_HIT_RATE

/**
 * This file implements global cache.
 * There are three types of requests:
 * original request: it is a copy of the request passed from access() and 
 *		is allocated in the heap.
 * partial request: it represents part of a request, and it should have 
 *		a point to the original request. This exists in two cases: when
 *		writing an old dirty page; when reading a page with a pending IO.
 * underlying request: it is a request sent to the underlying IO and is
 *		allocated on the stack. It has a point to the original request for
 *		a read request and the partial request for a write request. Only
 *		this type of requests can be multi-buf requests, and it should have
 *		a point to a page if it's a single-buf request.
 *
 * When a request is issued to the cache with access(), we first make a copy
 * of the request as there is no guarantee that the original request from
 * the invoker will be available after access() returns.
 *
 * However, it's often that we need to break a request into smaller ones for
 * different reasons when issuing them to the underlying IO.
 *
 * In some case, a request tries to access a page that another request has
 * been issued to the underlying IO for the page, the request will be added
 * to the page.
 */

/**
 * Extract a request from the input request.
 * The extract request is within the range [off, off + npages * PAGE_SIZE),
 * where off is aligned with PAGE_SIZE.
 */
static void extract_pages(const io_request &req, off_t off, int npages,
		io_request &extracted)
{
	off_t req_off;
	char *req_buf;
	ssize_t req_size;
	assert(req.get_num_bufs() == 1);
	assert((off & (PAGE_SIZE - 1)) == 0);
	bool check = (off >= req.get_offset() && off < req.get_offset() + req.get_size())
		|| (off + PAGE_SIZE >= req.get_offset()
				&& off + PAGE_SIZE < req.get_offset() + req.get_size())
		|| (off <= req.get_offset()
				&& off + PAGE_SIZE >= req.get_offset() + req.get_size());
	if (!check)
		fprintf(stderr, "req %lx, size: %lx, page off: %lx\n",
				req.get_offset(), req.get_size(), off);
	assert(check);
	// this is the first page in the request.
	if (off == ROUND_PAGE(req.get_offset())) {
		req_off = req.get_offset();
		req_buf = req.get_buf();
		// the remaining size in the page.
		req_size = PAGE_SIZE * npages - (req_off - off);
		if (req_size > req.get_size())
			req_size = req.get_size();
	}
	else {
		req_off = off;
		/* 
		 * We can't be sure if the request buffer is aligned
		 * with the page size.
		 */
		req_buf = req.get_buf() + (off - req.get_offset());
		ssize_t remaining = req.get_size() - (off - req.get_offset());
		req_size = remaining > PAGE_SIZE * npages ? PAGE_SIZE
			* npages : remaining;
	}
	extracted.init(req_buf, req_off, req_size, req.get_access_method(),
			req.get_io(), req.get_node_id());
}

static thread_safe_page *generic_complete_req(io_request *req,
		thread_safe_page *p, bool lock)
{
	int page_off;
	thread_safe_page *ret = NULL;
	char *req_buf;
	int req_size;

	if (req->within_1page()) {
		page_off = req->get_offset() - ROUND_PAGE(req->get_offset());
		req_buf = req->get_buf();
		req_size = req->get_size();
	}
	else {
		io_request extracted;
		extract_pages(*req, p->get_offset(), 1, extracted);
		page_off = extracted.get_offset() - ROUND_PAGE(extracted.get_offset());
		req_buf = extracted.get_buf();
		req_size = extracted.get_size();
	}

	if (lock)
		p->lock();
	if (req->get_access_method() == WRITE) {
		memcpy((char *) p->get_data() + page_off, req_buf, req_size);
		if (!p->set_dirty(true))
			ret = p;
	}
	else 
		/* I assume the data I read never crosses the page boundary */
		memcpy(req_buf, (char *) p->get_data() + page_off, req_size);
	if (lock)
		p->unlock();
	// TODO this is a bug. If the page is returned, we shouldn't
	// dereference it here.
	p->dec_ref();
	return ret;
}

/**
 * It returns the page that is dirtied by the function for the first time.
 */
static inline thread_safe_page *__complete_req(io_request *orig,
		thread_safe_page *p)
{
	return generic_complete_req(orig, p, true);
}

static inline thread_safe_page *__complete_req_unlocked(io_request *orig,
		thread_safe_page *p)
{
	return generic_complete_req(orig, p, false);
}

class access_page_callback: public callback
{
	global_cached_io *cached_io;
public:
	access_page_callback(global_cached_io *io) {
		cached_io = io;
	}
	int invoke(io_request *requests[], int);
	int multibuf_invoke(io_request *request);
};

void global_cached_io::notify_completion(io_request *req)
{
	io_interface *io = req->get_io();
	if (io->get_callback())
		io->get_callback()->invoke(&req, 1);
}

void global_cached_io::notify_completion(io_request *reqs[], int num)
{
	// If we just deliver notification to threads on the local processor.
	// we can notify them for each request.
	for (int i = 0; i < num; i++)
		notify_completion(reqs[i]);
}

void global_cached_io::finalize_partial_request(io_request &partial,
		io_request *orig)
{
	orig->inc_complete_count();
	if (orig->complete_size(partial.get_size())) {
		// It's important to notify the IO interface that issues the request.
		// In the case of parted global cache, the IO interface that processes
		// the reqeust isn't the IO interface that issue the request.
		// The request may be handled differently.
		global_cached_io *io = (global_cached_io *) orig->get_io();
		if (orig->is_sync())
			io->wakeup_on_req(orig, IO_OK);
		else
			io->notify_completion(orig);
		orig->dec_complete_count();
		orig->wait4unref();
		// Now we can delete it.
		req_allocator.free(orig);
	}
	else
		orig->dec_complete_count();
}

/**
 * This method is to finalize the request. The processing of the request
 * ends here.
 */
void global_cached_io::finalize_request(io_request &req)
{
	// It's possible that the request is just a partial request.
	if (req.is_partial()) {
		io_request *original = req.get_orig();
		assert(original);
		assert(original->get_orig() == NULL);
		original->inc_complete_count();
		if (original->complete_size(req.get_size())) {
			global_cached_io *io = (global_cached_io *) original->get_io();
			if (original->is_sync())
				io->wakeup_on_req(original, IO_OK);
			else
				io->notify_completion(original);
			original->dec_complete_count();
			original->wait4unref();
			req_allocator.free(original);
		}
		else
			original->dec_complete_count();
	}
	else {
		assert(req.get_orig() == NULL);
		global_cached_io *io = (global_cached_io *) req.get_io();
		if (req.is_sync())
			io->wakeup_on_req(&req, IO_OK);
		else
			io->notify_completion(&req);
	}
}

int access_page_callback::multibuf_invoke(io_request *request)
{
	page_cache *cache = cached_io->get_global_cache();
	io_request *orig = request->get_orig();
	assert(orig->get_num_bufs() == 1);
	/*
	 * Right now the global cache only support normal access().
	 */
	io_request *pending_reqs[request->get_num_bufs()];
	thread_safe_page *pages[request->get_num_bufs()];
	// The pages that are set dirty for the first time.
	thread_safe_page *dirty_pages[request->get_num_bufs()];
	int num_dirty_pages = 0;
	off_t off = request->get_offset();
	for (int i = 0; i < request->get_num_bufs(); i++) {
		thread_safe_page *p = request->get_page(i);
		/*
		 * The pages in the buffer of the request are sorted according
		 * to their offsets.
		 */
		assert(p);
		pages[i] = p;
		p->lock();
		assert(p->is_io_pending());
		if (request->get_access_method() == READ)
			p->set_data_ready(true);
		else {
			p->set_dirty(false);
			p->set_old_dirty(false);
		}
		p->set_io_pending(false);
		pending_reqs[i] = p->reset_reqs();
		if (request->get_access_method() == READ) {
			thread_safe_page *dirty = __complete_req_unlocked(orig, p);
			if (dirty)
				dirty_pages[num_dirty_pages++] = dirty;
		}
		else {
			// The page isn't flushed by the page eviction policy.
			// It's flush because we want to flush data with a large request.
			// The page that triggers the flush is saved in the private data.
			if (p != request->get_priv())
				p->dec_ref();
			assert(p->get_ref() >= 0);
		}
		p->unlock();
		off += PAGE_SIZE;
	}

	if (request->get_access_method() == READ) {
		/*
		 * For a multi-buf request, the private data actually points to
		 * the very original request.
		 */
		io_request partial;
		extract_pages(*orig, request->get_offset(), request->get_num_bufs(),
				partial);
		cached_io->finalize_partial_request(partial, orig);

		/*
		 * Now we should start to deal with all requests pending to pages
		 * All of these requests should be single buffer requests.
		 */
		for (int i = 0; i < request->get_num_bufs(); i++) {
			io_request *old = pending_reqs[i];
			thread_safe_page *p = pages[i];
			while (old) {
				io_request *next = old->get_next_req();
				thread_safe_page *dirty = __complete_req(old, p);
				if (dirty) {
					// We can be pretty certain that the same page won't appear
					// twice in the array.
					assert(num_dirty_pages < request->get_num_bufs());
					dirty_pages[num_dirty_pages++] = dirty;
				}
				cached_io->finalize_request(*old);
				// Now we can delete it.
				cached_io->get_req_allocator()->free(old);
				old = next;
			}
			assert(p->get_ref() >= 0);
		}
		cache->mark_dirty_pages(dirty_pages, num_dirty_pages);
	}
	else {
		io_request *orig = request->get_orig();
		global_cached_io *io = static_cast<global_cached_io *>(
				orig->get_io());
		// We can't invoke write() here because it may block the thread.
		// Instead, we queue the request, so it will be issue to
		// the device by the user thread.
		assert(orig->get_next_req() == NULL);
		io_request *buf[request->get_num_bufs() + 1];
		int num_req = 0;
		buf[num_req++] = orig;
		for (int i = 0; i < request->get_num_bufs(); i++) {
			if (pending_reqs[i]) {
				buf[num_req++] = pending_reqs[i];
			}
		}
		io->queue_requests(buf, num_req);
		// These requests can't be deleted yet.
		// They will be deleted when these write requests are finally served.
	}

	return -1;
}

int access_page_callback::invoke(io_request *requests[], int num)
{
	page_cache *cache = cached_io->get_global_cache();
	for (int i = 0; i < num; i++) {
		io_request *request = requests[i];
		/* 
		 * If the request doesn't have an original request,
		 * it is issued by the flushing thread.
		 */
		if (request->get_orig() == NULL) {
			cache->flush_callback(*request);
			continue;
		}

		if (request->get_num_bufs() > 1) {
			multibuf_invoke(request);
			continue;
		}

		thread_safe_page *p = (thread_safe_page *) request->get_priv();
		assert(request->get_size() <= PAGE_SIZE);

		p->lock();
		// If we write data to part of a page, we need to first read
		// the entire page to memory first.
		if (request->get_access_method() == READ) {
			p->set_data_ready(true);
		}
		// We just evict a page with dirty data and write the original
		// dirty data in the page to a file.
		else {
			p->set_old_dirty(false);
		}
		p->set_io_pending(false);
		io_request *old = p->reset_reqs();
		bool data_ready = p->data_ready();
		p->unlock();

		// If the data on the page is ready, it won't become unready.
		// The only place where data is set unready is where the page is evicted.
		// Since we have a reference of the page, it won't be evicted.
		// When data is ready, we can execuate any operations on the page.
		if (data_ready) {
			/* The request should contain the very original request. */
			io_request *orig = request->get_orig();
			assert(orig->get_orig() == NULL);
			thread_safe_page *dirty = __complete_req(orig, p);
			// TODO maybe I should make it support multi-request callback.
			if (dirty)
				cache->mark_dirty_pages(&dirty, 1);
			io_request partial;
			extract_pages(*orig, request->get_offset(), request->get_num_bufs(), partial);
			cached_io->finalize_partial_request(partial, orig);

			int num = 0;
			while (old) {
				/*
				 * It should be guaranteed that there isn't a multi-buf request
				 * in the queue. Because if a page is in IO pending, we won't
				 * issue a multi-buf request for the page.
				 */
				io_request *next = old->get_next_req();
				assert(old->get_num_bufs() == 1);
				thread_safe_page *dirty = __complete_req(old, p);
				if (dirty)
					cache->mark_dirty_pages(&dirty, 1);

				cached_io->finalize_request(*old);
				// Now we can delete it.
				cached_io->get_req_allocator()->free(old);
				old = next;
				num++;
			}
		}
		else {
			io_request *orig = request->get_orig();
			global_cached_io *io = static_cast<global_cached_io *>(
					orig->get_io());
			// We can't invoke write() here because it may block the thread.
			// Instead, we queue the request, so it will be issue to
			// the device by the user thread.
			assert(orig->get_next_req() == NULL);
			orig->set_next_req(old);
			io->queue_requests(&orig, 1);
			// These requests can't be deleted yet.
			// They will be deleted when these write requests are finally served.
		}
	}
	return 0;
}

global_cached_io::global_cached_io(io_interface *underlying): io_interface(
		underlying->get_node_id()), pending_requests(
		INIT_GCACHE_PENDING_SIZE), req_allocator(sizeof(io_request) * 1024)
{
	this->underlying = underlying;
	num_waits = 0;
	cache_size = 0;
	cb = NULL;
	cache_hits = 0;
	num_accesses = 0;
	underlying->set_callback(new access_page_callback(this));
	pthread_mutex_init(&sync_mutex, NULL);
	pthread_cond_init(&sync_cond, NULL);
	wait_req = NULL;
	status = 0;
}

global_cached_io::global_cached_io(io_interface *underlying,
		cache_config *config): io_interface(
			underlying->get_node_id()), pending_requests(
			INIT_GCACHE_PENDING_SIZE), req_allocator(sizeof(io_request) * 1024)
{
	cb = NULL;
	cache_hits = 0;
	num_accesses = 0;
	this->underlying = underlying;
	num_waits = 0;
	this->cache_size = config->get_size();
	underlying->set_callback(new access_page_callback(this));
	if (global_cache == NULL) {
		printf("Create cache on %d nodes\n", config->get_num_caches());
		global_cache = config->create_cache();
	}
	pthread_mutex_init(&sync_mutex, NULL);
	pthread_cond_init(&sync_cond, NULL);
	wait_req = NULL;
	status = 0;
}

/**
 * a write request only covers the memory within one page.
 */
ssize_t global_cached_io::__write(io_request *orig, thread_safe_page *p,
		std::vector<thread_safe_page *> &dirty_pages)
{
	ssize_t ret = 0;
//	orig->set_priv((void *) p);
	p->lock();
	assert(!p->is_old_dirty());
	if (!p->data_ready()) {
		if(!p->is_io_pending()) {
			assert(!p->is_dirty());

			// We are going to write to part of a page, therefore,
			// we need to first read the page.
			if (orig->get_size() < PAGE_SIZE) {
				off_t off = orig->get_offset();
				io_request *real_orig = orig->get_orig();
				// If the request doesn't have a private data, it is the real
				// original request.
				if (real_orig == NULL)
					real_orig = orig;
				else
					// `orig' is just part of the original request.
					// we don't need it any more.
					req_allocator.free(orig);
				assert(real_orig->get_orig() == NULL);
				io_request read_req((char *) p->get_data(),
						ROUND_PAGE(off), PAGE_SIZE, READ,
						underlying, p->get_node_id(), real_orig, p);
				p->set_io_pending(true);
				p->unlock();
				io_status status;
				underlying->access(&read_req, 1, &status);
				if (status == IO_FAIL) {
					perror("read");
					exit(1);
				}
			}
			else {
				// This is an optimization. If we can overwrite the entire page,
				// we don't need to read the page first. However, we have to
				// make sure data is written to a page without anyone else
				// having IO operations on it.
				thread_safe_page *dirty = __complete_req_unlocked(orig, p);
				if (dirty)
					dirty_pages.push_back(dirty);
				p->set_data_ready(true);
				p->unlock();
				ret = PAGE_SIZE;
				finalize_request(*orig);
				// Now we can delete it.
				req_allocator.free(orig);
			}
		}
		else {
			// If there is an IO pending, it means a read request
			// has been issuded. It can't be a write request, otherwise,
			// the data in the page will be ready.
			orig->set_priv(p);
			assert(orig->get_access_method() == WRITE);
			p->add_req(orig);
			p->unlock();
		}
	}
	else {
		// The data in the page is ready. We can write data to the page directly.
		//
		// If data is ready, there shouldn't be an IO pending.
		// In other words, if the thread for writing dirty pages is writing
		// a page, the page will be referenced and therefore, can't be returned
		// from the cache.
		// TODO we should delay the write if the page is being written back.
//		assert(!p->is_io_pending());
		p->unlock();

		thread_safe_page *dirty = __complete_req(orig, p);
		if (dirty)
			dirty_pages.push_back(dirty);
		ret = orig->get_size();
		finalize_request(*orig);
		// Now we can delete it.
		req_allocator.free(orig);
	}
	return ret;
}

ssize_t global_cached_io::__read(io_request *orig, thread_safe_page *p)
{
	ssize_t ret = 0;
//	orig->set_priv((void *) p);
	p->lock();
	if (!p->data_ready()) {
		if(!p->is_io_pending()) {
			p->set_io_pending(true);
			assert(!p->is_dirty());

			io_request req((char *) p->get_data(), p->get_offset(),
					/*
					 * it will notify the underlying IO,
					 * which then notifies global_cached_io.
					 */
					PAGE_SIZE, READ, underlying, get_node_id(), orig, p);
			p->unlock();
			assert(orig->get_orig() == NULL);
			io_status status;
			underlying->access(&req, 1, &status);
			if (status == IO_FAIL) {
				perror("read");
				exit(1);
			}
		}
		else {
			orig->set_priv(p);
			assert(orig->get_access_method() == READ);
			p->add_req(orig);
			p->unlock();
		}
	}
	else {
		// If the data in the page is ready, we don't need to change any state
		// of the page and just read data.
		p->unlock();
		ret = orig->get_size();
		__complete_req(orig, p);
		global_cached_io *io = (global_cached_io *) orig->get_io();
		assert(this == io);
		if (orig->is_sync())
			io->wakeup_on_req(orig, IO_OK);
		else
			notify_completion(orig);
	}
	return ret;
}

/**
 * In this method, we are going to issue multi-page read requests.
 * However, we may still break the input request if the data in a page
 * is ready or the page is in the state of IO pending.
 * @req: potentially part of a request.
 */
ssize_t global_cached_io::read(io_request &req, thread_safe_page *pages[],
		int npages, io_request *orig)
{
	ssize_t ret = 0;

	assert(npages <= MAX_NUM_IOVECS);
	assert(orig->get_orig() == NULL);
	io_request multibuf_req(-1, underlying, req.get_access_method(),
			get_node_id(), orig, NULL);

	/*
	 * The pages in `pages' should be sorted with their offsets.
	 * We are going to grab multiple locks below. As long as we always
	 * lock pages in the order of page offset, there won't be deadlock.
	 */
	for (int i = 0; i < npages; i++) {
		thread_safe_page *p = pages[i];
again:
		p->lock();
		if (!p->data_ready() && !p->is_io_pending()) {
			p->set_io_pending(true);
			assert(!p->is_dirty());
			if (multibuf_req.is_empty())
				multibuf_req.set_offset(p->get_offset());
			/* We don't need to worry buffer overflow here. */
			multibuf_req.add_page(p);
			multibuf_req.set_priv(p);
			p->unlock();
		}
		else if (!p->data_ready() && p->is_io_pending()) {
			/*
			 * If we have got some partial of the request, we need to submit
			 * the partial request.
			 */
			if (!multibuf_req.is_empty()) {
				p->unlock();
				underlying->access(&multibuf_req, 1);
				io_request tmp(-1, underlying, req.get_access_method(),
						get_node_id(), orig, NULL);
				multibuf_req = tmp;
				goto again;
			}
			else {
				/*
				 * All pending requests on a page have to be a single-buf request.
				 * Furthermore, the pending requests must only cover one page.
				 */
				// TODO I shouldn't allocate memory within locks.
				io_request *partial_orig = req_allocator.alloc_obj();
				extract_pages(*orig, p->get_offset(), 1, *partial_orig);
				partial_orig->set_partial(true);
				partial_orig->set_orig(orig);
				partial_orig->set_priv(p);
				p->add_req(partial_orig);
				p->unlock();
			}
		}
		/* 
		 * We have data ready in the page, we are still going to break
		 * the request.
		 */
		else {
			p->unlock();
			/*
			 * If we have collected part of the request, issue the partial
			 * request.
			 */
			if (!multibuf_req.is_empty()) {
				underlying->access(&multibuf_req, 1);
				io_request tmp(-1, underlying, req.get_access_method(),
						get_node_id(), orig, NULL);
				multibuf_req = tmp;
			}
			io_request complete_partial;
			extract_pages(*orig, p->get_offset(), 1, complete_partial);
			ret += complete_partial.get_size();
			__complete_req(&complete_partial, p);
			finalize_partial_request(complete_partial, orig);
		}
	}
	if (!multibuf_req.is_empty()) {
		underlying->access(&multibuf_req, 1);
	}
	return ret;
}

int global_cached_io::handle_pending_requests()
{
	int tot = 0;
	std::vector<thread_safe_page *> dirty_pages;
	while (!pending_requests.is_empty()) {
		io_request *reqs[MAX_FETCH_REQS];
		int num = pending_requests.fetch(reqs, MAX_FETCH_REQS);
		for (int i = 0; i < num; i++) {
			// It may be the head of a request list. All requests
			// in the list should point to the same page.
			io_request *req = reqs[i];
			thread_safe_page *p = (thread_safe_page *) req->get_priv();
			assert(p);
			if (p->is_old_dirty())
				printf("request %lx, p %lx is old dirty\n", req->get_offset(), p->get_offset());
			assert(!p->is_old_dirty());
			while (req) {
				/**
				 * Right now all pending requests are writes.
				 * All writes are single-buf requests.
				 */
				assert(req->get_num_bufs() == 1);
				io_request *next = (io_request *) req->get_next_req();
				assert(req->get_priv() == p);
				req->set_next_req(NULL);
				if (req->get_access_method() == WRITE)
					__write(req, p, dirty_pages);
				else
					__read(req, p);
				req = next;
			}
		}
		tot += num;
	}
	// It's not very likely we can get dirty pages here because this is
	// the place where we just finish writing old dirty pages to the disk.
	// The only possible reason is that we happen to overwrite the entire
	// page.
	get_global_cache()->mark_dirty_pages(dirty_pages.data(),
			dirty_pages.size());
	return tot;
}

void merge_pages2req(io_request &req, page_cache *cache)
{
#ifdef ENABLE_LARGE_WRITE
	thread_safe_page *p;
	off_t off = req.get_offset();
	off_t forward_off = off + PAGE_SIZE;
	off_t block_off = ROUND(off, params.get_RAID_block_size() * PAGE_SIZE);
	off_t block_end_off = block_off + params.get_RAID_block_size() * PAGE_SIZE;
	while (forward_off < block_end_off
			&& (p = (thread_safe_page *) cache->search(forward_off))) {
		p->lock();
		if (!p->is_dirty()) {
			p->dec_ref();
			p->unlock();
			break;
		}
		if (!p->is_io_pending()) {
			p->set_io_pending(true);
			req.add_page(p);
		}
		else {
			p->dec_ref();
			p->unlock();
			break;
		}
		p->unlock();
		forward_off += PAGE_SIZE;
	}
	if (off >= PAGE_SIZE) {
		off_t backward_off = off - PAGE_SIZE;
		while (backward_off >= block_off
				&& (p = (thread_safe_page *) cache->search(backward_off))) {
			p->lock();
			if (!p->is_dirty()) {
				p->dec_ref();
				p->unlock();
				break;
			}
			if (!p->is_io_pending()) {
				p->set_io_pending(true);
				req.add_page_front(p);
				req.set_offset(backward_off);
			}
			else {
				p->dec_ref();
				p->unlock();
				break;
			}
			p->unlock();
			if (backward_off >= PAGE_SIZE)
				backward_off -= PAGE_SIZE;
			else
				break;
		}
	}
#endif
}

/**
 * Write the dirty page. If possible, we merge it with pages adjacent to
 * it and write a larger request.
 */
void write_dirty_page(thread_safe_page *p, off_t off, io_interface *io,
		io_request *orig, page_cache *cache)
{
	p->lock();
	assert(!p->is_io_pending());
	p->set_io_pending(true);
	io_request req(off, io, WRITE, p->get_node_id(), orig, p);
	assert(p->get_ref() > 0);
	req.add_page(p);
	p->unlock();

	merge_pages2req(req, cache);

	io_status status;
	io->access(&req, 1, &status);
	if (status == IO_FAIL) {
		perror("write");
		abort();
	}
}

void global_cached_io::process_cached_reqs(io_request *cached_reqs[],
		thread_safe_page *cached_pages[], int num_cached_reqs)
{
	io_request *async_reqs[num_cached_reqs];
	int num_async_reqs = 0;
	num_fast_process += num_cached_reqs;
	for (int i = 0; i < num_cached_reqs; i++) {
		io_request *req = cached_reqs[i];
		thread_safe_page *dirty = __complete_req(req, cached_pages[i]);
		page_cache *cache = get_global_cache();
		if (dirty)
			cache->mark_dirty_pages(&dirty, 1);
		if (!req->is_sync())
			async_reqs[num_async_reqs++] = req;
	}
	// We don't need to notify completion for sync requests.
	// Actually, we don't even need to do anything for sync requests.
	notify_completion(async_reqs, num_async_reqs);
}

void global_cached_io::access(io_request *requests, int num, io_status *status)
{
	if (!pending_requests.is_empty()) {
		handle_pending_requests();
	}

	io_request *cached_reqs[num];
	thread_safe_page *cached_pages[num];
	int num_cached_reqs = 0;

	std::vector<thread_safe_page *> dirty_pages;
	for (int i = 0; i < num; i++) {
		off_t offset = requests[i].get_offset();
		int size = requests[i].get_size();
		off_t begin_pg_offset = ROUND_PAGE(offset);
		off_t end_pg_offset = ROUNDUP_PAGE(offset + size);
		thread_safe_page *pages[MAX_NUM_IOVECS];
		// TODO right now it only supports single-buf requests.
		assert(requests[i].get_num_bufs() == 1);
		io_request *orig = NULL;

		int pg_idx = 0;
		int num_pages_hit = 0;
		int num_bytes_completed = 0;
		for (off_t tmp_off = begin_pg_offset; tmp_off < end_pg_offset;
				tmp_off += PAGE_SIZE) {
			off_t old_off = -1;
			thread_safe_page *p = (thread_safe_page *) (get_global_cache()
					->search(tmp_off, old_off));

			num_accesses++;
#ifdef TEST_HIT_RATE
			if (num_accesses % 100 < params.get_test_hit_rate()) {
				if (!p->data_ready()) {
					p->set_io_pending(false);
					p->set_data_ready(true);
					old_off = -1;
					if (p->is_old_dirty()) {
						p->set_dirty(false);
						p->set_old_dirty(false);
						p->set_io_pending(false);
					}
				}
			}
#endif
			/* 
			 * If old_off is -1, it means search() didn't evict a page, i.e.,
			 * it's a cache hit.
			 */
			if (old_off == -1) {
#ifdef STATISTICS
				cache_hits++;
#endif
				num_pages_hit++;
				// Let's optimize for cached single-page requests by stealing
				// them from normal code path of processing them.
				assert(requests[i].is_valid());
				if (requests[i].within_1page() && p->data_ready()) {
					cached_reqs[num_cached_reqs] = &requests[i];
					cached_pages[num_cached_reqs] = p;
					num_cached_reqs++;
					break;
				}
			}
			// We delay copying the IO request until here, so we don't
			// need to do it for cached single-page requests..
			if (orig == NULL) {
				orig = req_allocator.alloc_obj();
				orig->init(requests[i]);
			}
			/*
			 * Cache may evict a dirty page and return the dirty page
			 * to the user before it is written back to a file.
			 *
			 * We encounter a situation that two threads get the old dirty
			 * evicted page, one thread gets its old offset thanks to
			 * old_off, the other can't, so the other thread has to wait
			 * until the dirty page is written to the file, and we need to
			 * give the page another status to indicate it's an old dirty
			 * page.
			 */

			/* This page has been evicted. */
			if (p->is_old_dirty()) {
				/*
				 * We got a few contiguous pages for read, so we should split
				 * the request and issue reads for the contiguous pages first.
				 * We always break write requests into pages, so it has to be
				 * read requests.
				 */
				if (pg_idx) {
					io_request req;
					extract_pages(*orig, pages[0]->get_offset(), pg_idx, req);
					read(req, pages, pg_idx, orig);
					pg_idx = 0;
				}

				// Extract the partial access.
				io_request *orig1;
				// If the request accesses more than one page.
				if (end_pg_offset - begin_pg_offset > PAGE_SIZE) {
					orig1 = req_allocator.alloc_obj();
					extract_pages(*orig, tmp_off, 1, *orig1);
					orig1->set_orig(orig);
					orig1->set_priv(p);
					assert(orig->get_size() > orig1->get_size());
					orig1->set_partial(true);
				}
				else {
					orig1 = orig;
					orig1->set_priv(p);
				}

				/* The page is evicted in this thread */
				if (old_off != ROUND_PAGE(offset) && old_off != -1) {
					/*
					 * Only one thread can come here because only one thread
					 * can evict the dirty page and the thread gets its old
					 * offset, and only this thread can write back the old
					 * dirty page.
					 */
					write_dirty_page(p, old_off, underlying, orig1, get_global_cache());
					continue;
				}
				else {
					// At this moment, the page is being written back to the file
					// by another thread. We should queue the request to tht page,
					// so when the dirty page completes writing back, we can proceed
					// writing.
					p->lock();
					if (p->is_old_dirty()) {
						p->add_req(orig1);
						p->unlock();
						// the request has been added to the page, when the old dirty
						// data is written back to the file, the write request will be
						// reissued to the file.
						continue;
					}
					else {
						p->unlock();
						if (orig1 != orig)
							req_allocator.free(orig1);
					}
				}
			}

			/*
			 * Large access only makes sense for reading. As large writes
			 * essentially overwrite entire pages in the memory, so we may
			 * only need to read the first and the last pages.
			 */
			if (orig->get_access_method() == WRITE) {
				/* We need to extract a page from the request. */
				io_request req;
				extract_pages(*orig, tmp_off, 1, req);

				if (orig->get_size() == req.get_size())
					num_bytes_completed += __write(orig, p, dirty_pages);
				else {
					io_request *partial_orig = req_allocator.alloc_obj();
					partial_orig->init(req);
					partial_orig->set_orig(orig);
					partial_orig->set_partial(true);
					num_bytes_completed += __write(partial_orig, p, dirty_pages);
				}
			}
			else {
				// We have to make sure all pages accessed in one request should
				// be on the same node.
				if (pg_idx > 0)
					assert(pages[pg_idx - 1]->get_node_id() == p->get_node_id());
				pages[pg_idx++] = p;
				if (pg_idx == MAX_NUM_IOVECS || (pages[0]->get_offset()
							+ PAGE_SIZE * pg_idx) % RAID_BLOCK_SIZE == 0) {
					io_request req;
					extract_pages(*orig, pages[0]->get_offset(), pg_idx, req);
					num_bytes_completed += read(req, pages, pg_idx, orig);
					pg_idx = 0;
				}
			}
		}
		/*
		 * The only reason that pg_idx > 0 is that there is a large read request.
		 */
		if (pg_idx) {
			io_request req;
			extract_pages(*orig, pages[0]->get_offset(), pg_idx, req);
			read(req, pages, pg_idx, orig);
		}

		// If all pages accessed by the request are in the cache, the request
		// can be completed by the time when the functions returns.
		if (status) {
			if (num_pages_hit == (end_pg_offset - begin_pg_offset) / PAGE_SIZE
					// It's possible that a request is completed in the slow path.
					// The requested pages may become ready in the slow path;
					// or we write the entire page.
					|| num_bytes_completed == requests[i].get_size())
				status[i] = IO_OK;
			else {
				assert(orig);
				status[i] = IO_PENDING;
				status[i].set_priv_data((long) orig);
			}
		}
	}
	process_cached_reqs(cached_reqs, cached_pages, num_cached_reqs);
	get_global_cache()->mark_dirty_pages(dirty_pages.data(),
				dirty_pages.size());
}

io_status global_cached_io::access(char *buf, off_t offset,
		ssize_t size, int access_method)
{
	io_request req(buf, offset, size, access_method, this, this->get_node_id(), true);
	io_status status;
	access(&req, 1, &status);
	underlying->flush_requests();
	if (status == IO_PENDING) {
		io_request *orig = (io_request *) status.get_priv_data();
		assert(orig);
		wait4req(orig);
	}
	// TODO IO may fail, I need to return an error in case it fails.
	status = IO_OK;
	status.set_priv_data(size);
	return status;
}

int global_cached_io::preload(off_t start, long size) {
	if (size > cache_size) {
		fprintf(stderr, "we can't preload data larger than the cache size\n");
		exit(1);
	}

	assert(ROUND_PAGE(start) == start);
	for (long offset = start; offset < start + size; offset += PAGE_SIZE) {
		off_t old_off = -1;
		thread_safe_page *p = (thread_safe_page *) (get_global_cache()->search(
					ROUND_PAGE(offset), old_off));
		// This is mainly for testing. I don't need to really read data from disks.
		if (!p->data_ready()) {
			p->set_io_pending(false);
			p->set_data_ready(true);
		}
		p->dec_ref();
	}
	return 0;
}

page_cache *global_cached_io::global_cache;