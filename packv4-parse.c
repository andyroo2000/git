/*
 * Code to parse pack v4 object encoding
 *
 * (C) Nicolas Pitre <nico@fluxnic.net>
 *
 * This code is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "cache.h"
#include "packv4-parse.h"
#include "varint.h"

const unsigned char *get_sha1ref(struct packed_git *p,
				 const unsigned char **bufp)
{
	const unsigned char *sha1;

	if (!**bufp) {
		sha1 = *bufp + 1;
		*bufp += 21;
	} else {
		unsigned int index = decode_varint(bufp);
		if (index < 1 || index - 1 > p->num_objects)
			die("bad index in %s", __func__);
		sha1 = p->sha1_table + (index - 1) * 20;
	}

	return sha1;
}

struct packv4_dict {
	const unsigned char *data;
	unsigned int nb_entries;
	unsigned int offsets[FLEX_ARRAY];
};

static struct packv4_dict *load_dict(struct packed_git *p, off_t *offset)
{
	struct pack_window *w_curs = NULL;
	off_t curpos = *offset;
	unsigned long dict_size, avail;
	unsigned char *src, *data;
	const unsigned char *cp;
	git_zstream stream;
	struct packv4_dict *dict;
	int nb_entries, i, st;

	/* get uncompressed dictionary data size */
	src = use_pack(p, &w_curs, curpos, &avail);
	cp = src;
	dict_size = decode_varint(&cp);
	if (dict_size < 3) {
		error("bad dict size");
		return NULL;
	}
	curpos += cp - src;

	data = xmallocz(dict_size);
	memset(&stream, 0, sizeof(stream));
	stream.next_out = data;
	stream.avail_out = dict_size + 1;

	git_inflate_init(&stream);
	do {
		src = use_pack(p, &w_curs, curpos, &stream.avail_in);
		stream.next_in = src;
		st = git_inflate(&stream, Z_FINISH);
		curpos += stream.next_in - src;
	} while ((st == Z_OK || st == Z_BUF_ERROR) && stream.avail_out);
	git_inflate_end(&stream);
	unuse_pack(&w_curs);
	if (st != Z_STREAM_END || stream.total_out != dict_size) {
		error("pack dictionary bad");
		free(data);
		return NULL;
	}

	/* count number of entries */
	nb_entries = 0;
	cp = data;
	while (cp < data + dict_size - 3) {
		cp += 2;  /* prefix bytes */
		cp += strlen((const char *)cp);  /* entry string */
		cp += 1;  /* terminating NUL */
		nb_entries++;
	}
	if (cp - data != dict_size) {
		error("dict size mismatch");
		free(data);
		return NULL;
	}

	dict = xmalloc(sizeof(*dict) + nb_entries * sizeof(dict->offsets[0]));
	dict->data = data;
	dict->nb_entries = nb_entries;

	cp = data;
	for (i = 0; i < nb_entries; i++) {
		dict->offsets[i] = cp - data;
		cp += 2;
		cp += strlen((const char *)cp) + 1;
	}

	*offset = curpos;
	return dict;
}

static void load_ident_dict(struct packed_git *p)
{
	off_t offset = 12 + p->num_objects * 20;
	struct packv4_dict *names = load_dict(p, &offset);
	if (!names)
		die("bad pack name dictionary in %s", p->pack_name);
	p->ident_dict = names;
	p->ident_dict_end = offset;
}

const unsigned char *get_identref(struct packed_git *p, const unsigned char **srcp)
{
	unsigned int index;

	if (!p->ident_dict)
		load_ident_dict(p);

	index = decode_varint(srcp);
	if (index >= p->ident_dict->nb_entries) {
		error("%s: index overflow", __func__);
		return NULL;
	}
	return p->ident_dict->data + p->ident_dict->offsets[index];
}

static void load_path_dict(struct packed_git *p)
{
	off_t offset;
	struct packv4_dict *paths;

	/*
	 * For now we need to load the name dictionary to know where
	 * it ends and therefore where the path dictionary starts.
	 */
	if (!p->ident_dict)
		load_ident_dict(p);

	offset = p->ident_dict_end;
	paths = load_dict(p, &offset);
	if (!paths)
		die("bad pack path dictionary in %s", p->pack_name);
	p->path_dict = paths;
}

const unsigned char *get_pathref(struct packed_git *p, unsigned int index)
{
	if (!p->path_dict)
		load_path_dict(p);

	if (index >= p->path_dict->nb_entries) {
		error("%s: index overflow", __func__);
		return NULL;
	}
	return p->path_dict->data + p->path_dict->offsets[index];
}

void *pv4_get_commit(struct packed_git *p, struct pack_window **w_curs,
		     off_t offset, unsigned long size)
{
	unsigned long avail;
	git_zstream stream;
	int len, st;
	unsigned int nb_parents;
	unsigned char *dst, *dcp;
	const unsigned char *src, *scp, *sha1, *ident, *author, *committer;
	unsigned long author_time, commit_time;
	int16_t author_tz, commit_tz;

	dst = xmallocz(size);
	dcp = dst;

	src = use_pack(p, w_curs, offset, &avail);
	scp = src;

	sha1 = get_sha1ref(p, &scp);
	len = snprintf((char *)dcp, size, "tree %s\n", sha1_to_hex(sha1));
	dcp += len;
	size -= len;

	nb_parents = decode_varint(&scp);
	while (nb_parents--) {
		sha1 = get_sha1ref(p, &scp);
		len = snprintf((char *)dcp, size, "parent %s\n", sha1_to_hex(sha1));
		if (len >= size)
			die("overflow in %s", __func__);
		dcp += len;
		size -= len;
	}

	commit_time = decode_varint(&scp);
	ident = get_identref(p, &scp);
	commit_tz = (ident[0] << 8) | ident[1];
	committer = &ident[2];

	author_time = decode_varint(&scp);
	ident = get_identref(p, &scp);
	author_tz = (ident[0] << 8) | ident[1];
	author = &ident[2];

	if (author_time & 1)
		author_time = commit_time + (author_time >> 1);
	else
		author_time = commit_time - (author_time >> 1);

	len = snprintf((char *)dcp, size, "author %s %lu %+05d\n",
			author, author_time, author_tz);
	if (len >= size)
		die("overflow in %s", __func__);
	dcp += len;
	size -= len;

	len = snprintf((char *)dcp, size, "committer %s %lu %+05d\n",
			committer, commit_time, commit_tz);
	if (len >= size)
		die("overflow in %s", __func__);
	dcp += len;
	size -= len;

	if (scp - src > avail)
		die("overflow in %s", __func__);
	offset += scp - src;

	memset(&stream, 0, sizeof(stream));
	stream.next_out = dcp;
	stream.avail_out = size + 1;
	git_inflate_init(&stream);
	do {
		src = use_pack(p, w_curs, offset, &stream.avail_in);
		stream.next_in = (unsigned char *)src;
		st = git_inflate(&stream, Z_FINISH);
		offset += stream.next_in - src;
	} while ((st == Z_OK || st == Z_BUF_ERROR) && stream.avail_out);
	git_inflate_end(&stream);
	if (st != Z_STREAM_END || stream.total_out != size) {
		free(dst);
		return NULL;
	}

	return dst;
}

static int decode_entries(struct packed_git *p, struct pack_window **w_curs,
			  off_t offset, unsigned int start, unsigned int count,
			  unsigned char **dstp, unsigned long *sizep,
			  int parse_hdr)
{
	unsigned long avail;
	unsigned int nb_entries;
	const unsigned char *src, *scp;
	off_t copy_objoffset = 0;

	src = use_pack(p, w_curs, offset, &avail);
	scp = src;

	if (parse_hdr) {
		/* we need to skip over the object header */
		while (*scp & 128)
			if (++scp - src >= avail - 20)
				return -1;
		/* let's still make sure this is actually a tree */
		if ((*scp++ & 0xf) != OBJ_PV4_TREE)
			return -1;
	}

	nb_entries = decode_varint(&scp);
	if (scp == src || start > nb_entries || count > nb_entries - start)
		return -1;
	offset += scp - src;
	avail -= scp - src;
	src = scp;

	while (count) {
		unsigned int what;

		if (avail < 20) {
			src = use_pack(p, w_curs, offset, &avail);
			if (avail < 20)
				return -1;
		}
		scp = src;

		what = decode_varint(&scp);
		if (scp == src)
			return -1;

		if (!(what & 1) && start != 0) {
			/*
			 * This is a single entry and we have to skip it.
			 * The path index was parsed and is in 'what'.
			 * Skip over the SHA1 index.
			 */
			if (!*scp)
				scp += 1 + 20;
			else
				while (*scp++ & 128);
			start--;
		} else if (!(what & 1) && start == 0) {
			/*
			 * This is an actual tree entry to recreate.
			 */
			const unsigned char *path, *sha1;
			unsigned mode;
			int len;

			path = get_pathref(p, what >> 1);
			sha1 = get_sha1ref(p, &scp);
			if (!path || !sha1)
				return -1;
			mode = (path[0] << 8) | path[1];
			len = snprintf((char *)*dstp, *sizep, "%o %s%c",
					   mode, path+2, '\0');
			if (len + 20 > *sizep)
				return -1;
			hashcpy(*dstp + len, sha1);
			*dstp += len + 20;
			*sizep -= len + 20;
			count--;
		} else if (what & 1) {
			/*
			 * Copy from another tree object.
			 */
			unsigned int copy_start, copy_count;

			copy_start = what >> 1;
			copy_count = decode_varint(&scp);
			if (!copy_count)
				return -1;

			/*
			 * The LSB of copy_count is a flag indicating if
			 * a third value is provided to specify the source
			 * object.  This may be omitted when it doesn't
			 * change, but has to be specified at least for the
			 * first copy sequence.
			 */
			if (copy_count & 1) {
				unsigned index = decode_varint(&scp);
				if (!index) {
					/*
					 * SHA1 follows. We assume the
					 * object is in the same pack.
					 */
					copy_objoffset =
						find_pack_entry_one(scp, p);
					scp += 20;
				} else {
					/*
					 * From the SHA1 index we can get
					 * the object offset directly.
					 */
					copy_objoffset =
						nth_packed_object_offset(p, index - 1);
				}
			}
			if (!copy_objoffset)
				return -1;
			copy_count >>= 1;

			if (start >= copy_count) {
				start -= copy_count;
			} else {
				int ret;
				copy_count -= start;
				copy_start += start;
				start = 0;
				if (copy_count > count)
					copy_count = count;
				count -= copy_count;
				ret = decode_entries(p, w_curs,
					copy_objoffset, copy_start, copy_count,
					dstp, sizep, 1);
				if (ret)
					return ret;
				/* force pack window readjustment */
				avail = scp - src;
			}
		}

		offset += scp - src;
		avail -= scp - src;
		src = scp;
	}

	return 0;
}

void *pv4_get_tree(struct packed_git *p, struct pack_window **w_curs,
		   off_t offset, unsigned long size)
{
	unsigned long avail;
	unsigned int nb_entries;
	unsigned char *dst, *dcp;
	const unsigned char *src, *scp;
	int ret;

	src = use_pack(p, w_curs, offset, &avail);
	scp = src;
	nb_entries = decode_varint(&scp);
	if (scp == src)
		return NULL;

	dst = xmallocz(size);
	dcp = dst;
	ret = decode_entries(p, w_curs, offset, 0, nb_entries, &dcp, &size, 0);
	if (ret < 0 || size != 0) {
		free(dst);
		return NULL;
	}
	return dst;
}
