/**
 * Copyright 2013 Da Zheng
 *
 * This file is part of SAFSlib.
 *
 * SAFSlib is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * SAFSlib is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with SAFSlib.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "io_request.h"
#include "cache.h"
#include "io_interface.h"

void *io_buf::get_buf() const
{
	if (is_page)
		return u.p->get_data();
	else
		return u.buf;
}

void io_buf::init(thread_safe_page *p)
{
	assert(p->get_ref() > 0);
	u.p = p;
	size = PAGE_SIZE;
	is_page = 1;
}

void io_request::init(char *buf, const data_loc_t &loc, ssize_t size,
		int access_method, io_interface *io, int node_id)
{
	assert(loc.get_offset() <= MAX_FILE_SIZE);
	this->file_id = loc.get_file_id();
	this->offset = loc.get_offset();
	this->io_addr = (long) io;
	if (is_extended_req()) {
		if (buf)
			add_buf(buf, size);
	}
	else if (payload_type == BASIC_REQ) {
		set_int_buf_size(size);
		this->payload.buf_addr = buf;
	}
	else {
		set_int_buf_size(size);
		this->payload.compute = NULL;
	}
	this->access_method = access_method & 0x1;
	// by default, a request is of high priority.
	assert(node_id <= MAX_NODE_ID);
	this->node_id = node_id;
}

int io_request::get_overlap_size(thread_safe_page *pg) const
{
	off_t start = max(pg->get_offset(), this->get_offset());
	off_t end = min(pg->get_offset() + PAGE_SIZE,
			this->get_offset() + this->get_size());
	return end - start;
}

file_id_t io_request::get_file_id() const
{
	return file_id;
}

void io_req_extension::add_io_buf(const io_buf &buf)
{
	if (num_bufs >= vec_capacity) {
		if (vec_pointer == embedded_vecs) {
			vec_capacity = MIN_NUM_ALLOC_IOVECS;
			vec_pointer = new io_buf[vec_capacity];
			memcpy(vec_pointer, embedded_vecs,
					sizeof(embedded_vecs[0]) * NUM_EMBEDDED_IOVECS);
		}
		else {
			vec_capacity *= 2;
			io_buf *tmp = new io_buf[vec_capacity];
			memcpy(tmp, vec_pointer,
					sizeof(vec_pointer[0]) * vec_capacity / 2);
			delete [] vec_pointer;
			vec_pointer = tmp;
		}
	}
	assert(num_bufs < vec_capacity);
	vec_pointer[num_bufs] = buf;
	num_bufs++;
}

void io_req_extension::add_buf(char *buf, int size, bool is_page)
{
	io_buf tmp;
	tmp.init(buf, size, is_page);
	add_io_buf(tmp);
}

void io_req_extension::add_buf_front(char *buf, int size, bool is_page)
{
	if (num_bufs >= vec_capacity) {
		if (vec_pointer == embedded_vecs) {
			vec_capacity = MIN_NUM_ALLOC_IOVECS;
			vec_pointer = new io_buf[vec_capacity];
			memcpy(vec_pointer + 1, embedded_vecs,
					sizeof(embedded_vecs[0]) * NUM_EMBEDDED_IOVECS);
		}
		else {
			vec_capacity *= 2;
			io_buf *tmp = new io_buf[vec_capacity];
			memcpy(tmp + 1, vec_pointer,
					sizeof(vec_pointer[0]) * vec_capacity / 2);
			delete [] vec_pointer;
			vec_pointer = tmp;
		}
	}
	else {
		memmove(vec_pointer + 1, vec_pointer,
				sizeof(vec_pointer[0]) * num_bufs);
	}
	assert(num_bufs < vec_capacity);
	vec_pointer[0].init((void *) buf, size, is_page);
	num_bufs++;
}

void user_compute::fetch_requests(io_interface *io, compute_allocator *alloc,
		std::vector<io_request> &reqs)
{
	while (has_requests()) {
		request_range range = get_next_request();
		user_compute *comp = alloc->alloc();
		io_request req(comp, range.get_loc(), range.get_size(),
				range.get_access_method(), io, io->get_node_id());
		reqs.push_back(req);
	}
}