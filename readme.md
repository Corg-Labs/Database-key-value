# Key-Value Database in C

A **persistent key-value store** with a hash map backend, binary file persistence, and an interactive REPL supporting SET, GET, DEL, and KEYS commands — all in a single C file.

Written in pure C with no external dependencies. Part of the Corg-Labs collection.

---

# Features
- In-memory hash map: 256 buckets with chaining for collision handling
- djb2 hash function for key distribution
- Persistent binary storage — auto-loaded on startup, flushed on every write
- REPL interface: `SET key value`, `GET key`, `DEL key`, `KEYS`, `FLUSH`, `STATS`
- Values may contain spaces (last token consumes the rest of the line)
- `STATS` command shows load factor, collision count, and longest chain
- Keys and values up to 256 bytes each

---

# Tutorial

## 1. The Entry and HashMap Structures

Each key-value pair is stored in an `Entry` with fixed-size key and value buffers. Entries in the same bucket are chained with a `next` pointer — this is open chaining (separate chaining), the simplest collision resolution strategy.

```c
typedef struct Entry {
    char          key[MAX_KEYLEN];   /* 256 bytes */
    char          val[MAX_VALLEN];   /* 256 bytes */
    struct Entry *next;
} Entry;

typedef struct {
    Entry  *buckets[BUCKETS];   /* 256 bucket heads */
    size_t  count;
    size_t  collisions;
} HashMap;
```

## 2. The djb2 Hash Function

djb2 is a simple, fast, and well-distributed hash. It starts with the magic seed `5381` and at each step does `hash = hash * 33 XOR char`, which distributes bits across the full 32-bit range.

```c
static uint32_t djb2(const char *s) {
    uint32_t h = 5381;
    while (*s)
        h = ((h << 5) + h) ^ (unsigned char)*s++;
    return h;
}

static inline int bucket_of(const char *key) {
    return (int)(djb2(key) % BUCKETS);
}
```

The `% BUCKETS` maps the 32-bit hash into [0, 255]. A good hash function means similar keys (e.g., `"user1"` and `"user2"`) land in different buckets, keeping chains short.

## 3. SET — Insert or Update

`hm_set` first searches for an existing entry. If found, the value is updated in-place (no memory allocation). If not found, a new `Entry` is prepended to the bucket's chain — prepending is O(1) and avoids traversing to the tail.

```c
static int hm_set(HashMap *hm, const char *key, const char *val) {
    Entry *e = hm_find(hm, key);
    if (e) {
        strncpy(e->val, val, MAX_VALLEN - 1);
        return 0;   /* updated */
    }

    int     b  = bucket_of(key);
    Entry  *ne = calloc(1, sizeof(Entry));
    strncpy(ne->key, key, MAX_KEYLEN - 1);
    strncpy(ne->val, val, MAX_VALLEN - 1);

    if (hm->buckets[b]) hm->collisions++;   /* bucket was already occupied */
    ne->next       = hm->buckets[b];         /* prepend to chain */
    hm->buckets[b] = ne;
    hm->count++;
    return 1;   /* new key */
}
```

## 4. GET — Lookup by Key

`hm_find` walks the chain at the key's bucket comparing strings. Average O(1) with a good hash; worst case O(n) if all keys collide.

```c
static Entry *hm_find(HashMap *hm, const char *key) {
    int b = bucket_of(key);
    for (Entry *e = hm->buckets[b]; e; e = e->next)
        if (!strcmp(e->key, key)) return e;
    return NULL;
}
```

## 5. DEL — Removal with Pointer-to-Pointer Trick

Deletion must splice the entry out of its chain. The pointer-to-pointer (`**prev`) trick elegantly handles both the head-of-bucket case and the mid-chain case without a special branch.

```c
static int hm_del(HashMap *hm, const char *key) {
    int     b    = bucket_of(key);
    Entry **prev = &hm->buckets[b];   /* pointer to the pointer we'll update */
    for (Entry *e = hm->buckets[b]; e; prev = &e->next, e = e->next) {
        if (!strcmp(e->key, key)) {
            *prev = e->next;   /* splice out: works for head or mid-chain */
            free(e);
            hm->count--;
            return 1;
        }
    }
    return 0;
}
```

When `e` is the head, `*prev` is `hm->buckets[b]`, so updating it moves the bucket head. When `e` is mid-chain, `*prev` is the previous entry's `next` field.

## 6. Binary File Persistence Format

The storage format is a sequence of length-prefixed records. No null terminators are stored in the file, so keys and values with any byte content are supported. `uint16_t` lengths (little-endian) precede each string.

```
[uint16_t klen][klen bytes key][uint16_t vlen][vlen bytes value] ...
```

```c
static void cb_flush(const char *key, const char *val, void *user) {
    FILE    *f    = (FILE *)user;
    uint16_t klen = (uint16_t)strlen(key);
    uint16_t vlen = (uint16_t)strlen(val);
    fwrite(&klen, 2, 1, f);
    fwrite(key,   1, klen, f);
    fwrite(&vlen, 2, 1, f);
    fwrite(val,   1, vlen, f);
}

static void db_flush(HashMap *hm) {
    FILE *f = fopen(DB_FILE, "wb");
    hm_each(hm, cb_flush, f);   /* iterate all entries, write each */
    fclose(f);
}
```

Loading reverses this: read `klen`, read `klen` bytes into a key buffer, read `vlen`, read `vlen` bytes into a value buffer, then call `hm_set`.

## 7. The REPL and Line Tokeniser

The tokeniser splits the input line into up to 3 tokens. Crucially, the third token consumes the remainder of the line including spaces — this allows values like `hello world` to work without quoting.

```c
static int tokenise(char *line, char **t, int max) {
    int n = 0;
    char *p = line;
    while (n < max) {
        while (*p == ' ' || *p == '\t') p++;   /* skip leading spaces */
        if (!*p || *p == '\n') break;
        t[n++] = p;
        if (n == max) {
            /* last token: strip trailing whitespace, include internal spaces */
            size_t len = strlen(p);
            while (len > 0 && (p[len-1] == '\n' || p[len-1] == ' '))
                p[--len] = '\0';
            break;
        }
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
        if (*p) *p++ = '\0';
    }
    return n;
}
```

The REPL normalises the command token to uppercase before dispatching, so `set`, `SET`, and `Set` all work.

## 8. Load Factor and Statistics

The `STATS` command computes the load factor (keys / buckets) and the longest chain length — useful for understanding hash distribution quality.

```c
static void print_stats(HashMap *hm) {
    size_t used_buckets = 0, max_chain = 0;
    for (int i = 0; i < BUCKETS; i++) {
        size_t chain = 0;
        for (Entry *e = hm->buckets[i]; e; e = e->next) chain++;
        if (chain) used_buckets++;
        if (chain > max_chain) max_chain = chain;
    }
    printf("Keys: %zu  |  Load: %.3f  |  Longest chain: %zu\n",
           hm->count, (double)hm->count / BUCKETS, max_chain);
}
```

A load factor below ~0.7 with a maximum chain of 2-3 indicates a well-behaved hash distribution.

---

# Build
```
gcc kvdb.c -o kvdb
```

# Run
```
./kvdb
```

The database file `kvdb.dat` is created in the current directory and auto-loaded on the next run.

# Controls

| Command | Action |
|---|---|
| `SET key value` | Store or update a key (value may contain spaces) |
| `GET key` | Print the value, or `(nil)` if not found |
| `DEL key` | Delete a key |
| `KEYS` | List all keys |
| `FLUSH` | Force-write all data to disk |
| `STATS` | Show load factor, collision count, longest chain |
| `QUIT` / `Q` | Save and exit |

---

# Concepts Practiced
- Hash map with open chaining (separate chaining)
- djb2 hash function: `hash = hash * 33 XOR char`
- Pointer-to-pointer (`**prev`) for clean linked-list removal
- Prepend-to-bucket insertion for O(1) amortised SET
- Binary length-prefixed file format for persistence
- Iterating all entries via a callback (`hm_each`)
- Load factor and chain length as hash quality metrics
- Multi-token REPL with last-token-consumes-rest-of-line semantics

---

# Dependencies
Standard C libraries only: `stdio.h`, `stdlib.h`, `string.h`, `stdint.h`
