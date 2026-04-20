// tree.c — Tree object serialization and construction
//
// PROVIDED functions: get_file_mode, tree_parse, tree_serialize
// TODO functions:     tree_from_index
//
// Binary tree format (per entry, concatenated with no separators):
//   "<mode-as-ascii-octal> <name>\0<32-byte-binary-hash>"
//
// Example single entry (conceptual):
//   "100644 hello.txt\0" followed by 32 raw bytes of SHA-256

#include "tree.h"
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

// Forward declaration — object_write is implemented in object.c
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── Mode Constants ─────────────────────────────────────────────────────────

#define MODE_FILE      0100644
#define MODE_EXEC      0100755
#define MODE_DIR       0040000

// ─── PROVIDED ───────────────────────────────────────────────────────────────

// Determine the object mode for a filesystem path.
uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;

    if (S_ISDIR(st.st_mode))  return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

// Parse binary tree data into a Tree struct safely.
// Returns 0 on success, -1 on parse error.
int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        // 1. Safely find the space character for the mode
        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1;

        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);

        ptr = space + 1;

        // 2. Safely find the null terminator for the name
        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1;

        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0';

        ptr = null_byte + 1;

        // 3. Read the 32-byte binary hash
        if (ptr + HASH_SIZE > end) return -1;
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

// Helper for qsort to ensure consistent tree hashing
static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

// Serialize a Tree struct into binary format for storage.
// Caller must free(*data_out).
// Returns 0 on success, -1 on error.
int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    size_t max_size = tree->count * 296;
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];

        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written + 1; // +1 steps over the '\0' written by sprintf

        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;
    return 0;
}

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

// Helper: sort IndexEntry pointers by path for consistent tree construction.
static int compare_index_ptrs(const void *a, const void *b) {
    return strcmp((*(const IndexEntry **)a)->path, (*(const IndexEntry **)b)->path);
}

// Recursive helper: build a tree for all entries whose paths start with
// `prefix`, write it to the object store, and return its hash in *id_out.
//
// `entries`  — pointer array of IndexEntry*, pre-sorted by path
// `count`    — number of entries in this slice
// `prefix`   — directory prefix for this level (e.g. "" for root, "src/" for src/)
//
// At each level we scan entries left-to-right:
//   - If the path after `prefix` contains no '/', it's a file → blob entry.
//   - Otherwise the next component is a subdirectory → recurse with that subdir's
//     entries and a longer prefix, producing a tree entry.
static int write_tree_recursive(IndexEntry **entries, int count,
                                const char *prefix, ObjectID *id_out) {
    Tree tree;
    tree.count = 0;

    size_t prefix_len = strlen(prefix);
    int i = 0;

    while (i < count) {
        // Path relative to the current directory level
        const char *rel = entries[i]->path + prefix_len;
        const char *slash = strchr(rel, '/');

        if (!slash) {
            // ── File entry: blob lives directly at this level ──────────────
            if (tree.count >= MAX_TREE_ENTRIES) return -1;
            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = entries[i]->mode;
            te->hash = entries[i]->hash;
            strncpy(te->name, rel, sizeof(te->name) - 1);
            te->name[sizeof(te->name) - 1] = '\0';
            i++;

        } else {
            // ── Subdirectory: collect all entries sharing this dir prefix ──
            size_t dir_len = (size_t)(slash - rel);
            char dir_name[256];
            memcpy(dir_name, rel, dir_len);
            dir_name[dir_len] = '\0';

            // Build the prefix for the subdirectory level ("src/", "src/utils/", …)
            char new_prefix[512];
            snprintf(new_prefix, sizeof(new_prefix), "%s%s/", prefix, dir_name);
            size_t new_prefix_len = strlen(new_prefix);

            // Find the end of this subdirectory's slice
            int j = i;
            while (j < count &&
                   strncmp(entries[j]->path, new_prefix, new_prefix_len) == 0) {
                j++;
            }

            // Recurse: build + write the subtree object
            ObjectID sub_id;
            if (write_tree_recursive(entries + i, j - i, new_prefix, &sub_id) != 0)
                return -1;

            // Add the subtree as a directory entry at this level
            if (tree.count >= MAX_TREE_ENTRIES) return -1;
            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = MODE_DIR;
            te->hash = sub_id;
            // dir_len is already bounds-checked (< 256) from the path parse above
            memcpy(te->name, dir_name, dir_len);
            te->name[dir_len] = '\0';

            i = j; // skip past all entries we just consumed
        }
    }

    // Serialize and write this tree to the object store
    void *data = NULL;
    size_t data_len = 0;
    if (tree_serialize(&tree, &data, &data_len) != 0) return -1;
    int ret = object_write(OBJ_TREE, data, data_len, id_out);
    free(data);
    return ret;
}

// Build a tree hierarchy from the current index and write all tree
// objects to the object store.
//
// Returns 0 on success, -1 on error.
int tree_from_index(ObjectID *id_out) {
    // 1) Load the staged index
    Index index;
    if (index_load(&index) != 0) return -1;
    if (index.count == 0) return -1; // nothing staged — refuse empty commit

    // 2) Build a sortable pointer array (avoids copying the full structs)
    IndexEntry **ptrs = malloc((size_t)index.count * sizeof(IndexEntry *));
    if (!ptrs) return -1;
    for (int i = 0; i < index.count; i++)
        ptrs[i] = &index.entries[i];

    // 3) Sort by path so subdirectories are grouped together
    qsort(ptrs, (size_t)index.count, sizeof(IndexEntry *), compare_index_ptrs);

    // 4) Recursively build and write the root tree
    int ret = write_tree_recursive(ptrs, index.count, "", id_out);

    free(ptrs);
    return ret;
}
