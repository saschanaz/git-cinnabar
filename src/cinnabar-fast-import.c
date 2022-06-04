#include "git-compat-util.h"
struct object_id;
static void start_packfile();
static void cinnabar_unregister_shallow(const struct object_id *oid);
#include <stdio.h>
extern FILE *helper_input;
#define stdin helper_input
extern int helper_output;
#include "dir.h"
#define fspathncmp strncmp
#include "fast-import.patched.c"
#include "cinnabar-fast-import.h"
#include "cinnabar-helper.h"
#include "cinnabar-notes.h"
#include "hg-bundle.h"
#include "hg-data.h"
#include "list.h"
#include "oid-array.h"
#include "replace-object.h"
#include "shallow.h"
#include "strslice.h"
#include "tree-walk.h"

// Including tag.h conflicts with fast-import.c, so manually define what
// we use.
extern const char *tag_type;

#define ENSURE_INIT() do { \
	if (!initialized) \
		init(); \
} while (0)

static int initialized = 0;
static int update_shallow = 0;

void cinnabar_unregister_shallow(const struct object_id *oid) {
	if (unregister_shallow(oid) == 0)
		update_shallow = 1;
}

static void cleanup();

/* Divert fast-import.c's calls to hashwrite so as to keep a fake pack window
 * on the last written bits, avoiding munmap/mmap cycles from
 * gfi_unpack_entry. */
static struct pack_window *pack_win;
static struct pack_window *prev_win;

void real_hashwrite(struct hashfile *, const void *, unsigned int);

void hashwrite(struct hashfile *f, const void *buf, unsigned int count)
{
	size_t window_size;

	if (f != pack_file) {
		real_hashwrite(f, buf, count);
		return;
	}

	if (!pack_win) {
		pack_win = xcalloc(1, sizeof(*pack_data->windows));
		pack_win->offset = 0;
		pack_win->len = 20;
		pack_win->base = xmalloc(packed_git_window_size + 20);
		pack_win->next = NULL;
	}
	/* pack_data is not set the first time hashwrite is called */
	if (pack_data && !pack_data->windows) {
		pack_data->windows = pack_win;
		pack_data->pack_size = pack_win->len;
	}

	real_hashwrite(f, buf, count);
	pack_win->last_used = -1; /* always last used */
	pack_win->inuse_cnt = -1;
	if (pack_data)
		pack_data->pack_size += count;

	window_size = packed_git_window_size + (pack_win->offset ? 20 : 0);

	if (window_size + 20 - pack_win->len > count) {
		memcpy(pack_win->base + pack_win->len - 20, buf, count);
		pack_win->len += count;
	} else {
		/* Slide our window so that it starts at an offset multiple of
		 * the window size minus 20 (we want 20 bytes of overlap with the
		 * preceding window, so that use_pack() won't create an overlapping
		 * window on its own) */
		off_t offset = pack_win->offset;
		pack_win->offset = ((pack_data->pack_size - 20)
			/ packed_git_window_size) * packed_git_window_size - 20;
		assert(offset != pack_win->offset);
		pack_win->len = pack_data->pack_size - pack_win->offset;

		/* Ensure a pack window on the data preceding that. */
		hashflush(f);
		if (prev_win)
			unuse_pack(&prev_win);
		use_pack(pack_data, &prev_win,
			 pack_win->offset + 20 - packed_git_window_size, NULL);
		assert(prev_win->len == packed_git_window_size);

		/* Copy the overlapping bytes. */
		memcpy(pack_win->base,
		       prev_win->base + packed_git_window_size - 20, 20);

		/* Fill up the new window. */
		memcpy(pack_win->base + 20, buf + count + 40 - pack_win->len,
		       pack_win->len - 40);
	}
}

off_t real_find_pack_entry_one(const unsigned char *sha1,
                               struct packed_git *p);

off_t find_pack_entry_one(const unsigned char *sha1, struct packed_git *p)
{
	if (p == pack_data) {
		struct object_entry *oe = get_object_entry(sha1);
		if (oe && oe->idx.offset > 1 && oe->pack_id == pack_id)
			return oe->idx.offset;
		return 0;
	}
	return real_find_pack_entry_one(sha1, p);
}

void *get_object_entry(const unsigned char *sha1)
{
	struct object_id oid;
	hashcpy(oid.hash, sha1);
	oid.algo = GIT_HASH_SHA1;
	return find_object(&oid);
}

/* Mostly copied from fast-import.c's cmd_main() */
static void init()
{
	int i;

	reset_pack_idx_option(&pack_idx_opts);
	git_pack_config();
	warn_on_object_refname_ambiguity = 0;

	alloc_objects(object_entry_alloc);
	atom_table_sz = 131071;
	atom_table = xcalloc(atom_table_sz, sizeof(struct atom_str*));
	branch_table = xcalloc(branch_table_sz, sizeof(struct branch*));
	avail_tree_table = xcalloc(avail_tree_table_sz, sizeof(struct avail_tree_content*));
	marks = mem_pool_calloc(&fi_mem_pool, 1, sizeof(struct mark_set));

	hashmap_init(&object_table, object_entry_hashcmp, NULL, 0);

	global_argc = 1;

	rc_free = mem_pool_alloc(&fi_mem_pool, cmd_save * sizeof(*rc_free));
	for (i = 0; i < (cmd_save - 1); i++)
		rc_free[i].next = &rc_free[i + 1];
	rc_free[cmd_save - 1].next = NULL;

	start_packfile();

	parse_one_feature("force", 0);
	initialized = 1;
	atexit(cleanup);
}

static void cleanup()
{
	if (!initialized)
		return;

	if (require_explicit_termination)
		object_count = 0;
	end_packfile();
	reprepare_packed_git(the_repository);

	if (!require_explicit_termination) {
		if (update_shallow) {
			struct shallow_lock shallow_lock;
			const char *alternate_shallow_file;
			setup_alternate_shallow(
				&shallow_lock, &alternate_shallow_file,
				NULL);
			commit_shallow_file(the_repository, &shallow_lock);
		}
		dump_branches();
	}

	unkeep_all_packs();

	initialized = 0;

	if (cinnabar_check & CHECK_HELPER)
		pack_report();
}

static void start_packfile()
{
	real_start_packfile();
	install_packed_git(the_repository, pack_data);
	list_add_tail(&pack_data->mru, &the_repository->objects->packed_git_mru);
}

static void end_packfile()
{
	if (prev_win)
		unuse_pack(&prev_win);
	if (pack_data) {
		struct pack_window *win, *prev;
		for (prev = NULL, win = pack_data->windows;
		     win; prev = win, win = win->next) {
			if (win != pack_win)
				continue;
			if (prev)
				prev->next = win->next;
			else
				pack_data->windows = win->next;
			break;
		}
	}
	if (pack_win) {
		free(pack_win->base);
		free(pack_win);
		pack_win = NULL;
	}

	/* uninstall_packed_git(pack_data) */
	if (pack_data) {
		struct packed_git *pack, *prev;
		for (prev = NULL, pack = the_repository->objects->packed_git;
		     pack; prev = pack, pack = pack->next) {
			if (pack != pack_data)
				continue;
			if (prev)
				prev->next = pack->next;
			else
				the_repository->objects->packed_git = pack->next;
			hashmap_remove(&the_repository->objects->pack_map,
			               &pack_data->packmap_ent,
			               pack_data->pack_name);
			break;
		}
		list_del_init(&pack_data->mru);
	}

	real_end_packfile();
}

const struct object_id empty_tree = { {
	0x4b, 0x82, 0x5d, 0xc6, 0x42, 0xcb, 0x6e, 0xb9, 0xa0, 0x60,
	0xe5, 0x4b, 0xf8, 0xd6, 0x92, 0x88, 0xfb, 0xee, 0x49, 0x04,
}, GIT_HASH_SHA1 };

void maybe_reset_notes(const char *branch)
{
	struct notes_tree *notes = NULL;

	// The python frontend will use fast-import commands to commit the
	// hg2git and git2hg trees as separate temporary branches, and then
	// remove them. We want to update the notes tree on the temporary
	// branches, and keep them there when they are removed.
	if (!strcmp(branch, "refs/cinnabar/hg2git")) {
		notes = &hg2git;
	} else if (!strcmp(branch, "refs/notes/cinnabar")) {
		notes = &git2hg;
	}
	if (notes) {
		struct branch *b = lookup_branch(branch);
		if (!is_null_oid(&b->oid)) {
			if (notes_initialized(notes))
				free_notes(notes);
			init_notes(notes, oid_to_hex(&b->oid),
				   combine_notes_ignore, 0);
		}
	}
}

struct oid_array manifest_heads = OID_ARRAY_INIT;
int manifest_heads_dirty = 0;

static void oid_array_insert(struct oid_array *array, int index,
                             const struct object_id *oid)
{
	ALLOC_GROW(array->oid, array->nr + 1, array->alloc);
	memmove(&array->oid[index+1], &array->oid[index],
	        sizeof(array->oid[0]) * (array->nr++ - index));
	oidcpy(&array->oid[index], oid);
	if (array == &manifest_heads)
		manifest_heads_dirty = 1;
}

static void oid_array_remove(struct oid_array *array, int index)
{
	memmove(&array->oid[index], &array->oid[index+1],
	        sizeof(array->oid[0]) * (--array->nr - index));
	if (array == &manifest_heads)
		manifest_heads_dirty = 1;
}

void ensure_heads(struct oid_array *heads)
{
	struct commit *c = NULL;
	struct commit_list *parent;
	const char *body = NULL;

	/* We always keep the array sorted, so if it's not sorted, it's
	 * not initialized. */
	if (heads->sorted)
		return;

	heads->sorted = 1;
	if (heads == &manifest_heads)
		c = lookup_commit_reference_by_name(MANIFESTS_REF);
	if (c) {
		const char *msg = get_commit_buffer(c, NULL);
		body = strstr(msg, "\n\n") + 2;
		unuse_commit_buffer(c, msg);
	}
	for (parent = c ? c->parents : NULL; parent;
	     parent = parent->next) {
		const struct object_id *parent_sha1 =
			&parent->item->object.oid;
		/* Skip first parent when "has-flat-manifest-tree" is
		 * there */
		if (heads == &manifest_heads && body && parent == c->parents &&
		    !strcmp(body, "has-flat-manifest-tree"))
			continue;
		if (!heads->nr || oidcmp(&heads->oid[heads->nr-1],
		                         parent_sha1)) {
			oid_array_insert(heads, heads->nr, parent_sha1);
		} else {
			/* This should not happen, but just in case,
			 * instead of failing. */
			add_head(heads, parent_sha1);
		}
	}
}

void add_head(struct oid_array *heads, const struct object_id *oid)
{
	struct commit *c = NULL;
	struct commit_list *parent;
	int pos;

	ensure_heads(heads);
	c = lookup_commit(the_repository, oid);
	parse_commit_or_die(c);

	for (parent = c->parents; parent; parent = parent->next) {
		pos = oid_array_lookup(heads, &parent->item->object.oid);
		if (pos >= 0)
			oid_array_remove(heads, pos);
	}
	pos = oid_array_lookup(heads, oid);
	if (pos >= 0)
		return;
	oid_array_insert(heads, -pos - 1, oid);
}

static void handle_changeset_conflict(struct hg_object_id *hg_id,
                                      struct object_id *git_id)
{
	/* There are cases where two changesets would map to the same git
	 * commit because their differences are not in information stored in
	 * the git commit (different manifest node, but identical tree ;
	 * different branches ; etc.)
	 * In that case, add invisible characters to the commit message until
	 * we find a commit that doesn't map to another changeset.
	 */
	struct strbuf buf = STRBUF_INIT;
	const struct object_id *note;

	ensure_notes(&git2hg);
	while ((note = get_note(&git2hg, git_id))) {
		struct hg_object_id oid;
		enum object_type type;
		unsigned long len;
		char *content = read_object_file_extended(
			the_repository, note, &type, &len, 0);
		if (len < 50 || !starts_with(content, "changeset ") ||
		    get_sha1_hex(&content[10], oid.hash))
			die("Invalid git2hg note for %s", oid_to_hex(git_id));

		free(content);

		/* We might just already have the changeset in store */
		if (hg_oideq(&oid, hg_id))
			break;

		if (!buf.len) {
			content = read_object_file_extended(
				the_repository, git_id, &type, &len, 0);
			strbuf_add(&buf, content, len);
			free(content);
		}

		strbuf_addch(&buf, '\0');
		store_object(OBJ_COMMIT, &buf, NULL, git_id, 0);
	}
	strbuf_release(&buf);

}

static void do_set_replace(struct string_list *args)
{
	struct object_id replaced;
	struct object_id replace_with;
	struct replace_object *replace;

	if (get_oid_hex(args->items[1].string, &replaced))
		die("Invalid sha1");
	if (get_oid_hex(args->items[2].string, &replace_with))
		die("Invalid sha1");

	if (is_null_oid(&replace_with)) {
		oidmap_remove(the_repository->objects->replace_map, &replaced);
	} else {
		replace = xmalloc(sizeof(*replace));
		oidcpy(&replace->original.oid, &replaced);
		oidcpy(&replace->replacement, &replace_with);
		struct replace_object *old = oidmap_put(
			the_repository->objects->replace_map, replace);
		if (old)
			free(old);
	}
}

extern add_changeset_head(struct hg_object_id *cs, struct object_id *meta);

static void do_set(struct string_list *args)
{
	enum object_type type;
	struct hg_object_id hg_id;
	struct object_id git_id;
	struct oid_array *heads = NULL;
	struct notes_tree *notes = &hg2git;
	int is_changeset = 0;
	int is_head = 0;

	if (args->nr != 3)
		die("set needs 3 arguments");

	if (!strcmp(args->items[0].string, "file")) {
		type = OBJ_BLOB;
	} else if (!strcmp(args->items[0].string, "manifest") ||
	           !strcmp(args->items[0].string, "changeset")) {
		type = OBJ_COMMIT;
		if (args->items[0].string[0] == 'm')
			heads = &manifest_heads;
		else
			is_changeset = 1;
	} else if (!strcmp(args->items[0].string, "changeset-metadata")) {
		type = OBJ_BLOB;
		notes = &git2hg;
	} else if (!strcmp(args->items[0].string, "changeset-head")) {
		is_head = 1;
	} else if (!strcmp(args->items[0].string, "file-meta")) {
		type = OBJ_BLOB;
		notes = &files_meta;
	} else if (!strcmp(args->items[0].string, "replace")) {
		do_set_replace(args);
		return;
	} else {
		die("Unknown kind of object: %s", args->items[0].string);
	}

	if (get_sha1_hex(args->items[1].string, hg_id.hash))
		die("Invalid sha1");

	if (get_oid_hex(args->items[2].string, &git_id))
		die("Invalid sha1");

	if (is_head) {
		add_changeset_head(&hg_id, &git_id);
		return;
	}
	if (notes == &git2hg) {
		const struct object_id *note;
		ensure_notes(&hg2git);
		note = get_note_hg(&hg2git, &hg_id);
		if (note) {
			ensure_notes(&git2hg);
			if (is_null_oid(&git_id)) {
				remove_note(notes, note->hash);
			} else if (oid_object_info(the_repository, &git_id,
			                           NULL) != OBJ_BLOB) {
				die("Invalid object");
			} else {
				add_note(notes, note, &git_id);
			}
		} else if (!is_null_oid(&git_id))
			die("Invalid sha1");
		return;
	}

	ensure_notes(notes);
	if (is_null_oid(&git_id)) {
		remove_note_hg(notes, &hg_id);
	} else if (oid_object_info(the_repository, &git_id, NULL) != type) {
		die("Invalid object");
	} else {
		if (is_changeset)
			handle_changeset_conflict(&hg_id, &git_id);
		add_note_hg(notes, &hg_id, &git_id);
		if (heads)
			add_head(heads, &git_id);
	}
}

int write_object_file_flags(const void *buf, size_t len, enum object_type type,
                            struct object_id *oid, unsigned flags)
{
	struct strbuf data;
	data.buf = (void *)buf;
	data.len = len;
	data.alloc = len;
	store_object(type, &data, NULL, oid, 0);
	return 0;
}

static void store_notes(struct notes_tree *notes, struct object_id *result)
{
	oidclr(result);
	if (notes_dirty(notes)) {
		unsigned int mode = (notes == &hg2git) ? S_IFGITLINK
		                                       : S_IFREG | 0644;
		write_notes_tree(notes, result, mode);
	}
}

void hg_file_store(struct hg_file *file, struct hg_file *reference)
{
	struct object_id oid;
	struct last_object last_blob = { STRBUF_INIT, 0, 0, 1 };
	struct object_entry *oe = NULL;

	ENSURE_INIT();

	if (file->metadata.buf) {
		store_object(OBJ_BLOB, &file->metadata, NULL, &oid, 0);
		ensure_notes(&files_meta);
		add_note_hg(&files_meta, &file->oid, &oid);
	}

	if (reference)
		oe = (struct object_entry *) reference->content_oe;

	if (oe && oe->idx.offset > 1 && oe->pack_id == pack_id) {
		last_blob.data.buf = reference->content.buf;
		last_blob.data.len = reference->content.len;
		last_blob.offset = oe->idx.offset;
		last_blob.depth = oe->depth;
	}
	store_object(OBJ_BLOB, &file->content, &last_blob, &oid, 0);
	ensure_notes(&hg2git);
	add_note_hg(&hg2git, &file->oid, &oid);

	file->content_oe = find_object(&oid);
}

static void store_file(struct rev_chunk *chunk)
{
	static struct hg_file last_file;
	struct hg_file file;
	struct strbuf data = STRBUF_INIT;
	struct rev_diff_part diff;
	size_t last_end = 0;

	if (is_empty_hg_file(chunk->node))
		return;

	if (!hg_oideq(chunk->delta_node, &last_file.oid)) {
		hg_file_release(&last_file);

		if (!is_null_hg_oid(chunk->delta_node))
			hg_file_load(&last_file, chunk->delta_node);

	}

	rev_diff_start_iter(&diff, chunk);
	while (rev_diff_iter_next(&diff)) {
		if (diff.start > last_file.file.len || diff.start < last_end)
			die("Malformed file chunk for %s",
			    hg_oid_to_hex(chunk->node));
		strbuf_add(&data, last_file.file.buf + last_end,
		           diff.start - last_end);
		strbuf_addbuf(&data, &diff.data);

		last_end = diff.end;
	}

	if (last_file.file.len < last_end)
		die("Malformed file chunk for %s", hg_oid_to_hex(chunk->node));

	strbuf_add(&data, last_file.file.buf + last_end,
		   last_file.file.len - last_end);

	hg_file_init(&file);
	hg_file_from_memory(&file, chunk->node, &data);

	hg_file_store(&file, &last_file);
	hg_file_swap(&file, &last_file);
	hg_file_release(&file);
}

struct manifest_line {
       struct strslice path;
       struct hg_object_id oid;
       char attr;
};

static int split_manifest_line(struct strslice *slice,
                               struct manifest_line *result)
{
       // The format of a manifest line is:
       //    <path>\0<sha1><attr>
       // where attr is one of '', 'l', 'x'
       result->path = strslice_split_once(slice, '\0');
       if (result->path.len == 0)
	       return -1;

       if (slice->len < 41)
	       return -1;
       if (get_sha1_hex(slice->buf, result->oid.hash))
	       return -1;
       *slice = strslice_slice(*slice, 40, SIZE_MAX);

       result->attr = slice->buf[0];
       if (result->attr == 'l' || result->attr == 'x') {
	       *slice = strslice_slice(*slice, 1, SIZE_MAX);
       } else if (result->attr == '\n')
	       result->attr = '\0';
       else
	       return -1;
       if (slice->len < 1 || slice->buf[0] != '\n')
	       return -1;
       *slice = strslice_slice(*slice, 1, SIZE_MAX);
       return 0;
}

static int add_parent(struct strbuf *data,
                      const struct hg_object_id *last_manifest_oid,
                      const struct branch *last_manifest,
                      const struct hg_object_id *parent_oid)
{
	if (!is_null_hg_oid(parent_oid)) {
		const struct object_id *note;
		if (hg_oideq(parent_oid, last_manifest_oid))
			note = &last_manifest->oid;
		else {
			note = get_note_hg(&hg2git, parent_oid);
		}
		if (!note)
			return -1;
		strbuf_addf(data, "parent %s\n", oid_to_hex(note));
	}
	return 0;
}

static void manifest_metadata_path(struct strbuf *out, struct strslice *in)
{
	struct strslice part;
	size_t len = in->len;
	part = strslice_split_once(in, '/');
	while (len != in->len) {
		strbuf_addch(out, '_');
		strbuf_addslice(out, part);
		strbuf_addch(out, '/');
		len = in->len;
		part = strslice_split_once(in, '/');
	}
	strbuf_addch(out, '_');
	strbuf_addslice(out, *in);
}

static void store_manifest(struct rev_chunk *chunk)
{
	static struct hg_object_id last_manifest_oid;
	static struct branch *last_manifest;
	static struct strbuf last_manifest_content = STRBUF_INIT;
	struct strbuf data = STRBUF_INIT;
	struct strbuf path = STRBUF_INIT;
	struct rev_diff_part diff;
	size_t last_end = 0;
	struct strslice slice;
	struct manifest_line line;

	if (!last_manifest) {
		last_manifest = new_branch("refs/cinnabar/manifests");
	}
	if (!is_null_hg_oid(chunk->delta_node) &&
	    !hg_oideq(chunk->delta_node, &last_manifest_oid)) {
		const struct object_id *note;
		ensure_notes(&hg2git);
		note = get_note_hg(&hg2git, chunk->delta_node);
		if (!note)
			die("Cannot find delta node %s for %s",
			    hg_oid_to_hex(chunk->delta_node),
			    hg_oid_to_hex(chunk->node));

		// TODO: this could be smarter, avoiding to throw everything
		// away. But this is what the equivalent fast-import commands
		// would do so for now, this is good enough.
		if (last_manifest->branch_tree.tree) {
			release_tree_content_recursive(
				last_manifest->branch_tree.tree);
			last_manifest->branch_tree.tree = NULL;
		}
		hg_oidcpy(&last_manifest_oid, chunk->delta_node);
		oidcpy(&last_manifest->oid, note);
		parse_from_existing(last_manifest);
		load_tree(&last_manifest->branch_tree);
		strbuf_reset(&last_manifest_content);
		strbuf_addbuf(&last_manifest_content, generate_manifest(note));
	}

	// Start with the same allocation size as last manifest. (-1 before
	// strbuf_grow always adds 1 for a final '\0')
	if (last_manifest_content.alloc)
		strbuf_grow(&data, last_manifest_content.alloc - 1);
	// While not exact, the total length of the previous manifest and the
	// chunk will be an upper bound on the size of the new manifest, so
	// ensure we'll have enough room for that.
	strbuf_grow(&data, last_manifest_content.len + chunk->raw.len);
	rev_diff_start_iter(&diff, chunk);
	while (rev_diff_iter_next(&diff)) {
		if (diff.start > last_manifest_content.len ||
		    diff.start < last_end || diff.start > diff.end)
			goto malformed;
		strbuf_add(&data, last_manifest_content.buf + last_end,
		           diff.start - last_end);
		strbuf_addbuf(&data, &diff.data);

		last_end = diff.end;

		// We assume manifest diffs are line-based.
		if (diff.start > 0 &&
		    last_manifest_content.buf[diff.start - 1] != '\n')
			goto malformed;
		if (diff.end > 0 &&
		    last_manifest_content.buf[diff.end - 1] != '\n')
			goto malformed;

		// TODO: Avoid a remove+add cycle for same-file modifications.

		// Process removed files.
		slice = strbuf_slice(&last_manifest_content, diff.start,
                                     diff.end - diff.start);
		while (split_manifest_line(&slice, &line) == 0) {
			manifest_metadata_path(&path, &line.path);
			tree_content_remove(&last_manifest->branch_tree,
			                    path.buf, NULL, 1);
			strbuf_reset(&path);
		}

		// Some manifest chunks can have diffs like:
		//   - start: off, end: off, data: string of length len
		//   - start: off, end: off + len, data: ""
		// which is valid, albeit wasteful.
		// (example: 13b23929aeb7d1f1f21458dfcb32b8efe9aad39d in the
		// mercurial mercurial repository, as of writing)
		// What that means, however, is that we can't
		// tree_content_set for additions until the end because a
		// subsequent iteration might be removing what we just
		// added. So we don't do them now, we'll re-iterate the diff
		// later.
	}

	rev_diff_start_iter(&diff, chunk);
	while (rev_diff_iter_next(&diff)) {
		// Process added files.
		slice = strbuf_slice(&diff.data, 0, diff.data.len);
		while (split_manifest_line(&slice, &line) == 0) {
			uint16_t mode;
			struct object_id file_node;
			hg_oidcpy2git(&file_node, &line.oid);

			if (line.attr == '\0')
				mode = 0160644;
			else if (line.attr == 'x')
				mode = 0160755;
			else if (line.attr == 'l')
				mode = 0160000;
			else
				goto malformed;

			manifest_metadata_path(&path, &line.path);
			tree_content_set(&last_manifest->branch_tree,
			                 path.buf, &file_node, mode, NULL);
			strbuf_reset(&path);
		}
	}

	strbuf_release(&path);

	if (last_manifest_content.len < last_end)
		goto malformed;

	strbuf_add(&data, last_manifest_content.buf + last_end,
		   last_manifest_content.len - last_end);

	strbuf_swap(&last_manifest_content, &data);
	strbuf_release(&data);

	store_tree(&last_manifest->branch_tree);
	oidcpy(&last_manifest->branch_tree.versions[0].oid,
	       &last_manifest->branch_tree.versions[1].oid);

	strbuf_addf(&data, "tree %s\n",
	            oid_to_hex(&last_manifest->branch_tree.versions[1].oid));

	if ((add_parent(&data, &last_manifest_oid, last_manifest,
	                chunk->parent1) == -1) ||
	    (add_parent(&data, &last_manifest_oid, last_manifest,
	                chunk->parent2) == -1))
		goto malformed;

	hg_oidcpy(&last_manifest_oid, chunk->node);
	strbuf_addstr(&data, "author  <cinnabar@git> 0 +0000\n"
	                     "committer  <cinnabar@git> 0 +0000\n"
	                     "\n");
	strbuf_addstr(&data, hg_oid_to_hex(&last_manifest_oid));
	store_object(OBJ_COMMIT, &data, NULL, &last_manifest->oid, 0);
	strbuf_release(&data);
	ensure_notes(&hg2git);
	add_note_hg(&hg2git, &last_manifest_oid, &last_manifest->oid);
	add_head(&manifest_heads, &last_manifest->oid);
	if ((cinnabar_check & CHECK_MANIFESTS) &&
	    !check_manifest(&last_manifest->oid, NULL))
		die("sha1 mismatch for node %s", hg_oid_to_hex(chunk->node));
	return;

malformed:
	die("Malformed manifest chunk for %s", hg_oid_to_hex(chunk->node));
}

static void for_each_changegroup_chunk(FILE *in, int version,
                                       void (*callback)(struct rev_chunk *))
{
	int cg2 = version == 2;
	struct strbuf buf = STRBUF_INIT;
	struct rev_chunk chunk = { STRBUF_INIT, };
	struct hg_object_id delta_node = {{ 0, }};

	while (read_rev_chunk(in, &buf), buf.len) {
		rev_chunk_from_memory(&chunk, &buf, cg2 ? NULL : &delta_node);
		if (!cg2 && is_null_hg_oid(&delta_node))
			hg_oidcpy(&delta_node, chunk.parent1);
		callback(&chunk);
		if (!cg2)
			hg_oidcpy(&delta_node, chunk.node);
		rev_chunk_release(&chunk);
	}
}

static void skip_chunk(struct rev_chunk *chunk) {}

static int add_manifests_parent(const struct object_id *oid, void *data)
{
	struct strbuf *buf = data;
	strbuf_addstr(buf, "parent ");
	strbuf_addstr(buf, oid_to_hex(oid));
	strbuf_addch(buf, '\n');
	return 0;
}

static void do_store(struct string_list *args)
{
	if (args->nr < 2)
		die("store needs at least 3 arguments");

	if (!strcmp(args->items[0].string, "metadata")) {
		struct object_id result = {{ 0, }};

		if (args->nr != 2)
			die("store metadata needs 3 arguments");
		if (!strcmp(args->items[1].string, "hg2git") ||
		    !strcmp(args->items[1].string, "git2hg") ||
		    !strcmp(args->items[1].string, "files-meta")) {
			struct object_id tree;
			struct notes_tree *notes = NULL;
			const char *ref;

			switch (args->items[1].string[0]) {
			case 'f':
				notes = &files_meta;
				ref = FILES_META_REF;
				break;
			case 'g':
				notes = &git2hg;
				ref = NOTES_REF;
				break;
			case 'h':
				notes = &hg2git;
				ref = HG2GIT_REF;
			}
			store_notes(notes, &tree);

			if (is_null_oid(&tree)) {
				get_oid_committish(ref, &result);
				if (is_null_oid(&result)) {
					oidcpy(&tree, &empty_tree);
				}
			}
			if (!is_null_oid(&tree)) {
				struct strbuf buf = STRBUF_INIT;
				strbuf_addf(
					&buf, "tree %s\n",
					oid_to_hex(&tree));
				strbuf_addstr(
					&buf,
					"author  <cinnabar@git> 0 +0000\n"
					"committer  <cinnabar@git> 0 +0000\n"
					"\n");
				store_git_commit(&buf, &result);
				strbuf_release(&buf);
			}
		} else if (!strcmp(args->items[1].string, "manifests")) {
			if (manifest_heads_dirty) {
			        struct strbuf buf = STRBUF_INIT;
				strbuf_addf(
					&buf, "tree %s\n",
					oid_to_hex(&empty_tree));
				oid_array_sort(&manifest_heads);
				oid_array_for_each_unique(
					&manifest_heads, add_manifests_parent,
					&buf);
				strbuf_addstr(
					&buf,
					"author  <cinnabar@git> 0 +0000\n"
					"committer  <cinnabar@git> 0 +0000\n"
					"\n");
				store_git_commit(&buf, &result);
				strbuf_release(&buf);
			} else {
				get_oid_committish(MANIFESTS_REF, &result);
			}
		} else {
			die("Unknown metadata kind: %s", args->items[1].string);
		}
		write_or_die(helper_output, oid_to_hex(&result), 40);
		write_or_die(helper_output, "\n", 1);
	} else if (!strcmp(args->items[0].string, "file") ||
		   !strcmp(args->items[0].string, "manifest")) {
		struct strbuf buf = STRBUF_INIT;
		struct rev_chunk chunk;
		size_t length;
		struct hg_object_id oid;
		struct hg_object_id *delta_node;

		if (args->nr != 3)
			die("store file needs 4 arguments");
		if (!strcmp(args->items[1].string, "cg2")) {
			delta_node = NULL;
		} else {
			if (get_sha1_hex(args->items[1].string, oid.hash))
				die("Neither 'cg2' nor a sha1: %s",
				    args->items[1].string);
			delta_node = &oid;
		}

		// TODO: Error handling
		length = strtol(args->items[2].string, NULL, 10);
		strbuf_fread(&buf, length, helper_input);
		rev_chunk_from_memory(&chunk, &buf, delta_node);
		if (args->items[0].string[0] == 'f')
			store_file(&chunk);
		else
			store_manifest(&chunk);
		rev_chunk_release(&chunk);
	} else if (!strcmp(args->items[0].string, "changegroup")) {
		int version;
		struct strbuf buf = STRBUF_INIT;
		if (args->nr != 2)
			die("store changegroup only takes one argument");
		if (!strcmp(args->items[1].string, "1"))
			version = 1;
		else if (!strcmp(args->items[1].string, "2"))
			version = 2;
		else
			die("unsupported version");

		/* changesets */
		for_each_changegroup_chunk(helper_input, version, skip_chunk);
		/* manifests */
		for_each_changegroup_chunk(helper_input, version, store_manifest);
		/* files */
		while (read_rev_chunk(helper_input, &buf), buf.len) {
			strbuf_release(&buf);
			for_each_changegroup_chunk(helper_input, version, store_file);
		}
	} else if (!strcmp(args->items[0].string, "blob")) {
		struct object_id result;
		struct strbuf buf = STRBUF_INIT;
		size_t length, s;
		if (args->nr != 2)
			die("store blob only takes one argument");
		length = strtol(args->items[1].string, NULL, 10);
		while (length) {
			s = strbuf_fread(&buf, length, helper_input);
			length -= s;
		}
		store_object(OBJ_BLOB, &buf, NULL, &result, 0);
		strbuf_release(&buf);
		write_or_die(helper_output, oid_to_hex(&result), 40);
		write_or_die(helper_output, "\n", 1);
	} else {
		die("Unknown store kind: %s", args->items[0].string);
	}
}

void store_git_tree(struct strbuf *tree_buf, const struct object_id *reference,
                    struct object_id *result)
{
	struct last_object ref_tree = { STRBUF_INIT, 0, 0, 1 };
	struct last_object *last_tree = NULL;
	struct object_entry *oe = NULL;
	char *buf = NULL;

	ENSURE_INIT();
	if (reference) {
		oe = find_object((struct object_id *)reference);
	}
	if (oe && oe->idx.offset > 1 && oe->pack_id == pack_id) {
		unsigned long len;
		ref_tree.data.buf = buf = gfi_unpack_entry(oe, &len);
		ref_tree.data.len = len;
		ref_tree.offset = oe->idx.offset;
		ref_tree.depth = oe->depth;
		last_tree = &ref_tree;
	}
	store_object(OBJ_TREE, tree_buf, last_tree, result, 0);
	if (last_tree) {
		// store_object messes with last_tree so free using an old
		// copy of the pointer.
		free(buf);
	}
}

void store_git_commit(struct strbuf *commit_buf, struct object_id *result)
{
	ENSURE_INIT();
	store_object(OBJ_COMMIT, commit_buf, NULL, result, 0);
}

const struct object_id empty_blob = { {
	0xe6, 0x9d, 0xe2, 0x9b, 0xb2, 0xd1, 0xd6, 0x43, 0x4b, 0x8b,
	0x29, 0xae, 0x77, 0x5a, 0xd8, 0xc2, 0xe4, 0x8c, 0x53, 0x91,
}, GIT_HASH_SHA1 };

const struct object_id *ensure_empty_blob() {
	struct object_entry *oe = find_object((struct object_id *)&empty_blob);
	if (!oe) {
		struct object_id hash;
		struct strbuf buf = STRBUF_INIT;
		store_object(OBJ_BLOB, &buf, NULL, &hash, 0);
		assert(oidcmp(&hash, &empty_blob) == 0);
	}
	return &empty_blob;
}

static void do_reset(struct string_list *args) {
	struct branch *b;
	struct object_id oid;

	if (args->nr != 2)
		die("reset needs 2 arguments");

	b = lookup_branch(args->items[0].string);
	if (b) {
		oidclr(&b->oid);
		oidclr(&b->branch_tree.versions[0].oid);
		oidclr(&b->branch_tree.versions[1].oid);
		if (b->branch_tree.tree) {
			release_tree_content_recursive(b->branch_tree.tree);
			b->branch_tree.tree = NULL;
		}
	} else {
		b = new_branch(args->items[0].string);
	}
	if (get_sha1_hex(args->items[1].string, b->oid.hash))
		die("Invalid sha1");
	parse_from_existing(b);
	if (is_null_oid(&b->oid))
		b->delete = 1;
	maybe_reset_notes(args->items[0].string);
}

int maybe_handle_command(const char *command, struct string_list *args)
{
	if (!strcmp(command, "done")) {
		ENSURE_INIT();
		require_explicit_termination = 0;
		cleanup();
		write_or_die(helper_output, "ok\n", 3);
		return 2;
	} else if (!strcmp(command, "rollback")) {
		if (initialized) {
			ENSURE_INIT();
			cleanup();
		}
		write_or_die(helper_output, "ok\n", 3);
		return 2;
	} else if (!strcmp(command, "set")) {
		ENSURE_INIT();
		do_set(args);
	} else if (!strcmp(command, "store")) {
		ENSURE_INIT();
		require_explicit_termination = 1;
		do_store(args);
	} else if (!strcmp(command, "commit")) {
		struct branch *b;
		char *arg;
		ENSURE_INIT();
		require_explicit_termination = 1;
		arg = args->items[0].string;
		parse_new_commit(arg);
		b = lookup_branch(arg);
		write_or_die(helper_output, oid_to_hex(&b->oid), 40);
		write_or_die(helper_output, "\n", 1);
		maybe_reset_notes(arg);
	} else if (!strcmp(command, "reset")) {
		ENSURE_INIT();
		do_reset(args);
	} else
		return 0;

	return 1;
}
