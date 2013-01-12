#include <errno.h>
#include <limits.h>

#include "thread_private.h"
#include "associative_cache.h"
#include "flush_thread.h"
#include "container.cpp"

#ifdef STATISTICS
volatile int avail_cells;
volatile int num_wait_unused;
volatile int lock_contentions;
#endif

const long default_init_cache_size = 128 * 1024 * 1024;

/* out of memory exception */
class oom_exception
{
};

class expand_exception
{
};

hash_cell::hash_cell(associative_cache *cache, long hash) {
	this->hash = hash;
	assert(hash < INT_MAX);
	pthread_spin_init(&_lock, PTHREAD_PROCESS_PRIVATE);
	this->table = cache;
	char *pages[CELL_SIZE];
	if (!table->get_manager()->get_free_pages(CELL_SIZE, pages, cache))
		throw oom_exception();
	buf.set_pages(pages);
}

/**
 * rehash the pages in the current cell 
 * to the expanded cell.
 */
void hash_cell::rehash(hash_cell *expanded) {
	pthread_spin_lock(&_lock);
	pthread_spin_lock(&expanded->_lock);
	for (int i = 0, j = 0; i < CELL_SIZE; i++) {
		thread_safe_page *pg = buf.get_page(i);
		int hash1 = table->hash1_locked(pg->get_offset());
		/*
		 * It's possible that a page is in a wrong cell.
		 * It's likely because the page is added to the cell 
		 * right when `level' is increased.
		 * But the case is rare, so we can just simple ignore
		 * the case. It doesn't affect the correctness of 
		 * the implementation. The only penalty is that
		 * we might get a cache miss.
		 * Since the page is in a wrong cell, it won't be 
		 * accessed any more, so we should shorten the time
		 * it gets evicted by setting its hit to 1.
		 */
		if (hash1 != expanded->hash) {
			pg->set_hits(1);
			continue;
		}
		/* 
		 * if the two hash values don't match,
		 * it means the page is mapped to the expanded cell.
		 * we exchange the pages in the two cells.
		 */
		if (this->hash != hash1) {
			thread_safe_page *expanded_pg = expanded->buf.get_page(j);
			/* 
			 * we have to make sure no other threads are using them
			 * before we can exchange them.
			 * If the pages are in use, skip them.
			 */
			if (pg->get_ref()) {
				continue;
			}
			/* 
			 * the page in the expanded cell shouldn't
			 * have been initialized.
			 */
			assert(!expanded_pg->initialized());

			thread_safe_page tmp = *expanded_pg;
			*expanded_pg = *pg;
			*pg = tmp;
			j++;
		}
	}
	pthread_spin_unlock(&expanded->_lock);
	pthread_spin_unlock(&_lock);
	flags.clear_flag(OVERFLOW);
}

page *hash_cell::search(off_t offset)
{
	pthread_spin_lock(&_lock);
	page *ret = NULL;
	for (int i = 0; i < CELL_SIZE; i++) {
		if (buf.get_page(i)->get_offset() == offset) {
			ret = buf.get_page(i);
			break;
		}
	}
	if (ret) {
		if (ret->get_hits() == 0xff)
			buf.scale_down_hits();
		ret->inc_ref();
		ret->hit();
	}
	pthread_spin_unlock(&_lock);
	return ret;
}

/**
 * search for a page with the offset.
 * If the page doesn't exist, return an empty page.
 */
page *hash_cell::search(off_t off, off_t &old_off) {
	thread_safe_page *ret = NULL;
#ifndef STATISTICS
	pthread_spin_lock(&_lock);
#else
	if (pthread_spin_trylock(&_lock) == EBUSY) {
		__sync_fetch_and_add(&lock_contentions, 1);
		pthread_spin_lock(&_lock);
	}
#endif

	for (int i = 0; i < CELL_SIZE; i++) {
		if (buf.get_page(i)->get_offset() == off) {
			ret = buf.get_page(i);
			break;
		}
	}
	if (ret == NULL) {
		ret = get_empty_page();
		if (ret->is_dirty() && !ret->is_old_dirty()) {
			ret->set_dirty(false);
			ret->set_old_dirty(true);
		}
		old_off = ret->get_offset();
		if (old_off == PAGE_INVALID_OFFSET)
			old_off = -1;
		/*
		 * I have to change the offset in the spinlock,
		 * to make sure when the spinlock is unlocked, 
		 * the page can be seen by others even though
		 * it might not have data ready.
		 */
		ret->set_offset(off);
#ifdef USE_SHADOW_PAGE
		shadow_page shadow_pg = shadow.search(off);
		/*
		 * if the page has been seen before,
		 * we should set the hits info.
		 */
		if (shadow_pg.is_valid())
			ret->set_hits(shadow_pg.get_hits());
#endif
	}
	else
		policy.access_page(ret, buf);
	/* it's possible that the data in the page isn't ready */
	ret->inc_ref();
	if (ret->get_hits() == 0xff) {
		buf.scale_down_hits();
#ifdef USE_SHADOW_PAGE
		shadow.scale_down_hits();
#endif
	}
	ret->hit();
	pthread_spin_unlock(&_lock);
	return ret;
}

/* this function has to be called with lock held */
thread_safe_page *hash_cell::get_empty_page() {
	thread_safe_page *ret = NULL;

	bool expanded = false;
search_again:
	ret = policy.evict_page(buf);
	if (ret == NULL) {
		printf("all pages in the cell were all referenced\n");
		/* 
		 * If all pages in the cell are referenced, there is
		 * nothing we can do but wait. However, before busy waiting,
		 * we should unlock the lock, so other threads may still
		 * search the cell.
		 */
		pthread_spin_unlock(&_lock);
		bool all_referenced = true;
		while (all_referenced) {
			for (int i = 0; i < CELL_SIZE; i++) {
				thread_safe_page *pg = buf.get_page(i);
				/* If a page isn't referenced. */
				if (!pg->get_ref()) {
					all_referenced = false;
					break;
				}
			}
		}
		pthread_spin_lock(&_lock);
		goto search_again;
	}
	/*
	 * the selected page got hit before,
	 * we should expand the hash table
	 * if we haven't done it before.
	 */
	if (table->is_expandable() && policy.expand_buffer(*ret)) {
		flags.set_flag(OVERFLOW);
		long table_size = table->size();
		long average_size = table->get_manager()->average_cache_size();
		if (table_size < average_size && !expanded) {
			pthread_spin_unlock(&_lock);
			if (table->expand(this)) {
				throw expand_exception();
			}
			pthread_spin_lock(&_lock);
			expanded = true;
			goto search_again;
		}
	}

	/* we record the hit info of the page in the shadow cell. */
#ifdef USE_SHADOW_PAGE
	if (ret->get_hits() > 0)
		shadow.add(shadow_page(*ret));
#endif

	return ret;
}

/* 
 * the end of the vector points to the pages
 * that are most recently accessed.
 */
thread_safe_page *LRU_eviction_policy::evict_page(
		page_cell<thread_safe_page> &buf)
{
	int pos;
	if (pos_vec.size() < (unsigned) CELL_SIZE) {
		pos = pos_vec.size();
	}
	else {
		/* evict the first page */
		pos = pos_vec[0];
		pos_vec.erase(pos_vec.begin());
	}
	thread_safe_page *ret = buf.get_page(pos);
	while (ret->get_ref()) {}
	pos_vec.push_back(pos);
	ret->set_data_ready(false);
	return ret;
}

void LRU_eviction_policy::access_page(thread_safe_page *pg,
		page_cell<thread_safe_page> &buf)
{
	/* move the page to the end of the pos vector. */
	int pos = buf.get_idx(pg);
	for (std::vector<int>::iterator it = pos_vec.begin();
			it != pos_vec.end(); it++) {
		if (*it == pos) {
			pos_vec.erase(it);
			break;
		}
	}
	pos_vec.push_back(pos);
}

thread_safe_page *LFU_eviction_policy::evict_page(
		page_cell<thread_safe_page> &buf)
{
	thread_safe_page *ret = NULL;
	int min_hits = 0x7fffffff;
	do {
		int num_io_pending = 0;
		for (int i = 0; i < CELL_SIZE; i++) {
			thread_safe_page *pg = buf.get_page(i);
			if (pg->get_ref()) {
				if (pg->is_io_pending())
					num_io_pending++;
				continue;
			}

			/* 
			 * refcnt only increases within the lock of the cell,
			 * so if the page's refcnt is 0 above,
			 * it'll be always 0 within the lock.
			 */

			if (min_hits > pg->get_hits()) {
				min_hits = pg->get_hits();
				ret = pg;
			}

			/* 
			 * if a page hasn't been accessed before,
			 * it's a completely new page, just use it.
			 */
			if (min_hits == 0)
				break;
		}
		if (num_io_pending == CELL_SIZE) {
			printf("all pages are at io pending\n");
			// TODO do something...
			// maybe we should use pthread_wait
		}
		/* it happens when all pages in the cell is used currently. */
	} while (ret == NULL);
	ret->set_data_ready(false);
	ret->reset_hits();
	return ret;
}

thread_safe_page *FIFO_eviction_policy::evict_page(
		page_cell<thread_safe_page> &buf)
{
	thread_safe_page *ret = buf.get_empty_page();
	/*
	 * This happens a lot if we actually read pages from the disk.
	 * So basically, we shouldn't use this eviction policy for SSDs
	 * or magnetic hard drive..
	 */
	while (ret->get_ref()) {
		ret = buf.get_empty_page();
	}
	ret->set_data_ready(false);
	return ret;
}

thread_safe_page *gclock_eviction_policy::evict_page(
		page_cell<thread_safe_page> &buf)
{
	thread_safe_page *ret = NULL;
	int num_referenced = 0;
	int num_dirty = 0;
	bool avoid_dirty = true;
	do {
		thread_safe_page *pg = buf.get_page(clock_head % CELL_SIZE);
		if (num_dirty + num_referenced >= CELL_SIZE) {
			num_dirty = 0;
			num_referenced = 0;
			avoid_dirty = false;
		}
		if (pg->get_ref()) {
			num_referenced++;
			clock_head++;
			/*
			 * If all pages in the cell are referenced, we should
			 * return NULL to notify the invoker.
			 */
			if (num_referenced >= CELL_SIZE)
				return NULL;
			continue;
		}
		if (avoid_dirty && pg->is_dirty()) {
			num_dirty++;
			clock_head++;
			continue;
		}
		if (pg->get_hits() == 0) {
			ret = pg;
			break;
		}
		pg->set_hits(pg->get_hits() - 1);
		clock_head++;
	} while (ret == NULL);
	ret->set_data_ready(false);
	return ret;
}

thread_safe_page *clock_eviction_policy::evict_page(
		page_cell<thread_safe_page> &buf)
{
	thread_safe_page *ret = NULL;
	int num_referenced = 0;
	int num_dirty = 0;
	bool avoid_dirty = true;
	do {
		thread_safe_page *pg = buf.get_page(clock_head % CELL_SIZE);
		if (num_dirty + num_referenced >= CELL_SIZE) {
			num_dirty = 0;
			num_referenced = 0;
			avoid_dirty = false;
		}
		if (pg->get_ref()) {
			num_referenced++;
			if (num_referenced >= CELL_SIZE)
				return NULL;
			clock_head++;
			continue;
		}
		if (avoid_dirty && pg->is_dirty()) {
			num_dirty++;
			clock_head++;
			continue;
		}
		if (pg->get_hits() == 0) {
			ret = pg;
			break;
		}
		pg->reset_hits();
		clock_head++;
	} while (ret == NULL);
	ret->set_data_ready(false);
	ret->reset_hits();
	return ret;
}

#ifdef USE_SHADOW_PAGE

void clock_shadow_cell::add(shadow_page pg) {
	if (!queue.is_full()) {
		queue.push_back(pg);
		return;
	}
	/*
	 * We need to evict a page from the set.
	 * Find the first page whose reference bit isn't set.
	 */
	bool inserted = false;
	do {
		for (int i = 0; i < queue.size(); i++) {
			last_idx = (last_idx + 1) % queue.size();
			shadow_page old = queue.get(last_idx);
			/* 
			 * The page has been referenced recently,
			 * we should spare it.
			 */
			if (old.referenced()) {
				queue.get(last_idx).set_referenced(false);
				continue;
			}
			queue.set(pg, last_idx);
			inserted = true;
			break;
		}
		/* 
		 * If we can't insert the page in the for loop above,
		 * we need to go through the for loop again.
		 * But for the second time, we will definitely
		 * insert the page.
		 */
	} while (!inserted);
}

shadow_page clock_shadow_cell::search(off_t off) {
	for (int i = 0; i < queue.size(); i++) {
		shadow_page pg = queue.get(i);
		if (pg.get_offset() == off) {
			queue.get(i).set_referenced(true);
			return pg;
		}
	}
	return shadow_page();
}

void clock_shadow_cell::scale_down_hits() {
	for (int i = 0; i < queue.size(); i++) {
		queue.get(i).set_hits(queue.get(i).get_hits() / 2);
	}
}

shadow_page LRU_shadow_cell::search(off_t off) {
	for (int i = 0; i < queue.size(); i++) {
		shadow_page pg = queue.get(i);
		if (pg.get_offset() == off) {
			queue.remove(i);
			queue.push_back(pg);
			return pg;
		}
	}
	return shadow_page();
}

void LRU_shadow_cell::scale_down_hits() {
	for (int i = 0; i < queue.size(); i++) {
		queue.get(i).set_hits(queue.get(i).get_hits() / 2);
	}
}

#endif

/**
 * expand the cache.
 * @trigger_cell: the cell triggers the cache expansion.
 */
bool associative_cache::expand(hash_cell *trigger_cell) {
	hash_cell *cells = NULL;
	unsigned int i;

	if (flags.set_flag(TABLE_EXPANDING)) {
		/*
		 * if the flag has been set before,
		 * it means another thread is expanding the table,
		 */
		return false;
	}

	/* starting from this point, only one thred can be here. */
	for (i = 0; i < cells_table.size(); i++) {
		cells = cells_table[i];
		if (cells == NULL)
			break;
		if (trigger_cell >= cells && trigger_cell < cells + init_ncells)
			break;
	}
	assert(cells);

	hash_cell *cell = get_cell(split);
	long size = pow(2, level) * init_ncells;
	while (trigger_cell->is_overflow()) {
		unsigned int cells_idx = (split + size) / init_ncells;
		/* 
		 * I'm sure only this thread can change the table,
		 * so it doesn't need to hold a lock when accessing the size.
		 */
		unsigned int orig_size = ncells.get();
		if (cells_idx >= orig_size) {
			bool out_of_memory = false;
			/* create cells and put them in a temporary table. */
			std::vector<hash_cell *> table;
			for (unsigned int i = orig_size; i <= cells_idx; i++) {
				hash_cell *cells = new hash_cell[init_ncells];
				printf("create %d cells: %p\n", init_ncells, cells);
				try {
					for (int j = 0; j < init_ncells; j++) {
						cells[j] = hash_cell(this, i * init_ncells + j);
					}
					table.push_back(cells);
				} catch (oom_exception e) {
					out_of_memory = true;
					delete [] cells;
					break;
				}
			}

			/*
			 * here we need to hold the lock because other threads
			 * might be accessing the table. by using the write lock,
			 * we notify others the table has been changed.
			 */
			table_lock.write_lock();
			for (unsigned int i = 0; i < table.size(); i++) {
				cells_table[orig_size + i] = table[i];
			}
			ncells.inc(table.size());
			table_lock.write_unlock();
			if (out_of_memory)
				return false;
		}

		hash_cell *expanded_cell = get_cell(split + size);
		cell->rehash(expanded_cell);
		table_lock.write_lock();
		split++;
		if (split == size) {
			level++;
			printf("increase level to %d\n", level);
			split = 0;
			table_lock.write_unlock();
			break;
		}
		table_lock.write_unlock();
		cell = get_cell(split);
	}
	flags.clear_flag(TABLE_EXPANDING);
	return true;
}

page *associative_cache::search(off_t offset, off_t &old_off) {
	/*
	 * search might change the structure of the cell,
	 * and cause the cell table to expand.
	 * Thus, the page might not be placed in the cell
	 * we found before. Therefore, we need to research
	 * for the cell.
	 */
	do {
		try {
			return get_cell_offset(offset)->search(offset, old_off);
		} catch (expand_exception e) {
		}
	} while (true);
}

page *associative_cache::search(off_t offset)
{
	do {
		try {
			return get_cell_offset(offset)->search(offset);
		} catch (expand_exception e) {
		}
	} while (true);
}

associative_cache::associative_cache(long cache_size, bool expandable) {
	printf("associative cache is used\n");
	level = 0;
	split = 0;
	this->expandable = expandable;
	this->manager = new memory_manager(cache_size);
	manager->register_cache(this);
	long init_cache_size = default_init_cache_size;
	if (init_cache_size > cache_size
			// If the cache isn't expandable, let's just use the maximal
			// cache size at the beginning.
			|| !expandable)
		init_cache_size = cache_size;
	int npages = init_cache_size / PAGE_SIZE;
	assert(init_cache_size >= CELL_SIZE * PAGE_SIZE);
	init_ncells = npages / CELL_SIZE;
	hash_cell *cells = new hash_cell[init_ncells];
	printf("%d cells: %p\n", init_ncells, cells);
	int max_npages = manager->get_max_size() / PAGE_SIZE;
	try {
		for (int i = 0; i < init_ncells; i++)
			cells[i] = hash_cell(this, i);
	} catch (oom_exception e) {
		fprintf(stderr,
				"out of memory: max npages: %d, init npages: %d\n",
				max_npages, npages);
		exit(1);
	}

	cells_table.push_back(cells);
	ncells.inc(1);

	int max_ncells = max_npages / CELL_SIZE;
	for (int i = 1; i < max_ncells / init_ncells; i++)
		cells_table.push_back(NULL);
}

class associative_flush_thread: public flush_thread
{
	associative_cache *cache;
	io_interface *io;
	thread_safe_FIFO_queue<hash_cell *> dirty_cells;
public:
	associative_flush_thread(associative_cache *cache,
			io_interface *io): dirty_cells(MAX_NUM_DIRTY_CELLS_IN_QUEUE) {
		this->cache = cache;
		this->io = io;
	}

	void run();
	void request_callback(io_request &req);
	void dirty_pages(thread_safe_page *pages[], int num);
};

void merge_pages2reqs(std::vector<io_request *> &requests,
		std::map<off_t, thread_safe_page *> &dirty_pages,
		bool forward, std::vector<io_request *> &complete)
{
	for (unsigned i = 0; i < requests.size(); ) {
		io_request *req = requests[i];
		// there is a page adjacent to the request.
		std::map<off_t, thread_safe_page *>::iterator it;
		if (forward)
			it = dirty_pages.find(req->get_offset() + req->get_size());
		else
			it = dirty_pages.find(req->get_offset() - PAGE_SIZE);
		if (it != dirty_pages.end()) {
			thread_safe_page *p = it->second;
			dirty_pages.erase(it);
			p->lock();
			assert(!p->is_old_dirty());
			assert(p->data_ready());
			if (!p->is_io_pending()) {
				req->add_buf((char *) p->get_data(), PAGE_SIZE);
				// If the dirty pages are in front of the requests.
				if (!forward) {
					assert(p->get_offset() == req->get_offset() - PAGE_SIZE);
					req->set_offset(p->get_offset());
				}
				p->set_io_pending(true);
				i++;
				p->unlock();
			}
			else {
				/* The page is being written back, so we don't need to do it. */
				p->dec_ref();
				p->unlock();
				/* 
				 * We are going to break the request and write the existing
				 * request.
				 */
				complete.push_back(req);
				requests.erase(requests.begin() + i);
			}
		}
		else {
			/*
			 * There isn't a page adjacency to the request,
			 * we should remove the request from the vector and write it
			 * to the disk.
			 */
			complete.push_back(req);
			requests.erase(requests.begin() + i);
		}
	}

	/*
	 * We just release the reference count on the remaining pages,
	 * as we won't use them any more.
	 */
	for (std::map<off_t, thread_safe_page *>::const_iterator it
			= dirty_pages.begin(); it != dirty_pages.end(); it++) {
		thread_safe_page *p = it->second;
		p->dec_ref();
	}
}

void write_requests(const std::vector<io_request *> &requests, io_interface *io)
{
	for (unsigned i = 0; i < requests.size(); i++) {
		assert(requests[i]->get_orig() == NULL);
		if (requests[i]->get_num_bufs() > 1)
			io->access(requests[i], 1);
		else {
			thread_safe_page *p = (thread_safe_page *) requests[i]->get_priv();
			p->set_io_pending(false);
			p->dec_ref();
		}
	}
}

void associative_flush_thread::request_callback(io_request &req)
{
	if (req.get_num_bufs() == 1) {
		thread_safe_page *p = (thread_safe_page *) req.get_priv();
		p->lock();
		assert(p->is_dirty());
		p->set_dirty(false);
		p->set_io_pending(false);
		p->dec_ref();
		p->unlock();
	}
	else {
		off_t off = req.get_offset();
		for (int i = 0; i < req.get_num_bufs(); i++) {
			thread_safe_page *p = (thread_safe_page *) cache->search(off);
			assert(p);
			p->lock();
			assert(p->is_dirty());
			p->set_dirty(false);
			p->set_io_pending(false);
			p->dec_ref();
			p->dec_ref();
			assert(p->get_ref() >= 0);
			p->unlock();
			off += PAGE_SIZE;
		}
	}
}

void associative_flush_thread::run()
{
	int num_fetches;
	// We can't get more requests than the number of pages in a cell.
	io_request req_array[CELL_SIZE];
	while ((num_fetches = dirty_cells.get_num_entries()) > 0) {
		hash_cell *cells[num_fetches];
		int ret = dirty_cells.fetch(cells, num_fetches);
		// This is the only place where we fetches entries in the queue,
		// and there is only one thread fetching entries, so we can be 
		// very sure we can fetch the number of entries we specify.
		assert(ret == num_fetches);

		for (int i = 0; i < num_fetches; i++) {
			std::map<off_t, thread_safe_page *> dirty_pages;
			std::vector<io_request *> requests;
			cells[i]->get_dirty_pages(dirty_pages);
			int num_init_reqs = 0;
			for (std::map<off_t, thread_safe_page *>::const_iterator it
					= dirty_pages.begin(); it != dirty_pages.end(); it++) {
				thread_safe_page *p = it->second;
				p->lock();
				assert(!p->is_old_dirty());
				assert(p->data_ready());
				if (!p->is_io_pending()) {
					req_array[num_init_reqs].init((char *) p->get_data(),
								p->get_offset(), PAGE_SIZE, WRITE, io, NULL, p);
					requests.push_back(&req_array[num_init_reqs]);
					num_init_reqs++;
					p->set_io_pending(true);
				}
				else {
					/*
					 * If there is IO pending on the page, it means the page is being
					 * written back to the file, so we don't need to do anything on
					 * the page other than relasing the reference to the page.
					 */
					p->dec_ref();
				}
				p->unlock();
			}
			std::vector<io_request *> forward_complete;
			hash_cell *curr_cell = cells[i];
			size_t num_reqs = requests.size();
			// Search forward and find pages that can merge with the current requests.
			while (!requests.empty()) {
				hash_cell *next_cell = cache->get_next_cell(curr_cell);
				if (next_cell == NULL)
					break;
				dirty_pages.clear();
				next_cell->get_dirty_pages(dirty_pages);
				merge_pages2reqs(requests, dirty_pages, true, forward_complete);
				curr_cell = next_cell;
			}
			// Add the remaining merged requests to the same array with others.
			for (unsigned k = 0; k < requests.size(); k++)
				forward_complete.push_back(requests[k]);
			assert(forward_complete.size() == num_reqs);

			std::vector<io_request *> complete;
			curr_cell = cells[i];
			// Search backward and find pages that can merge with the current requests.
			while (!forward_complete.empty()) {
				hash_cell *prev_cell = cache->get_prev_cell(curr_cell);
				if (prev_cell == NULL)
					break;
				dirty_pages.clear();
				prev_cell->get_dirty_pages(dirty_pages);
				merge_pages2reqs(forward_complete, dirty_pages, false, complete);
			}
			for (unsigned k = 0; k < forward_complete.size(); k++)
				complete.push_back(forward_complete[k]);
			assert(complete.size() == num_reqs);
			write_requests(complete, io);
			// TODO maybe I shouldn't clear the bit here.
			cells[i]->set_in_queue(false);
		}
	}
}

flush_thread *associative_cache::create_flush_thread(io_interface *io)
{
	_flush_thread = new associative_flush_thread(this, io);
	return _flush_thread;
}

hash_cell *associative_cache::get_prev_cell(hash_cell *cell) {
	long index = cell->get_hash();
	// The first cell in the hash table.
	if (index == 0)
		return NULL;
	// The cell is in the middle of a cell array.
	if (index % init_ncells)
		return cell - 1;
	else {
		unsigned i;
		for (i = 0; i < cells_table.size(); i++) {
			if (cell == cells_table[i]) {
				assert(i > 0);
				// return the last cell in the previous cell array.
				return (cells_table[i - 1] + init_ncells - 1);
			}
		}
		// we should reach here if the cell exists in the table.
		abort();
	}
}

hash_cell *associative_cache::get_next_cell(hash_cell *cell)
{
	long index = cell->get_hash();
	// If it's not the last cell in the cell array.
	if (index % init_ncells != init_ncells - 1)
		return cell + 1;
	else {
		unsigned i;
		hash_cell *first = cell + 1 - init_ncells;
		for (i = 0; i < cells_table.size(); i++) {
			if (first == cells_table[i]) {
				if (i == cells_table.size() - 1)
					return NULL;
				else
					return cells_table[i + 1];
			}
		}
		// We should reach here.
		abort();
	}
}

void hash_cell::get_dirty_pages(std::map<off_t, thread_safe_page *> &pages)
{
	pthread_spin_lock(&_lock);
	for (int i = 0; i < CELL_SIZE; i++) {
		thread_safe_page *p = buf.get_page(i);
		/*
		 * When we are here, the page can't be evicted, so it won't be
		 * set old dirty.
		 * By checking the IO pending bit, we can reduce the chance of
		 * returning the page to the flush thread, so we might be able
		 * to improve performance.
		 */
		if (p->is_dirty() && !p->is_io_pending()) {
			p->inc_ref();
			pages.insert(std::pair<off_t, thread_safe_page *>(p->get_offset(), p));
		}
	}
	pthread_spin_unlock(&_lock);
}

int hash_cell::num_pages(char set_flags, char clear_flags)
{
	int num = 0;
	pthread_spin_lock(&_lock);
	for (int i = 0; i < CELL_SIZE; i++) {
		thread_safe_page *p = buf.get_page(i);
		if (p->test_flags(set_flags) && !p->test_flags(clear_flags))
			num++;
	}
	pthread_spin_unlock(&_lock);
	return num;
}

void associative_flush_thread::dirty_pages(thread_safe_page *pages[], int num)
{
#ifdef ENABLE_FLUSH_THREAD
	hash_cell *cells[num];
	int num_queued_cells = 0;
	for (int i = 0; i < num; i++) {
		hash_cell *cell = cache->get_cell_offset(pages[i]->get_offset());
		if (!cell->is_in_queue()) {
			char dirty_flag = 0;
			char io_pending_flag = 0;
			page_set_flag(dirty_flag, DIRTY_BIT, true);
			page_set_flag(io_pending_flag, IO_PENDING_BIT, true);
			/*
			 * We only count the number of dirty pages without IO pending.
			 * If a page is dirty but has IO pending, it means the page
			 * is being written back, so we don't need to do anything with it.
			 */
			int n = cell->num_pages(dirty_flag, io_pending_flag);
			if (n > DIRTY_PAGES_THRESHOLD && !cell->set_in_queue(true))
				cells[num_queued_cells++] = cell;
		}
	}
	if (num_queued_cells > 0) {
		dirty_cells.add(cells, num_queued_cells);
		activate();
	}
#endif
}
