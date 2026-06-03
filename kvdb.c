/*
 * kvdb.c — Persistent key-value database with hash-map storage.
 *
 * Features:
 *   - In-memory hash map, 256 buckets, chaining via linked list
 *   - Persist to a binary file: load on startup, flush on every write
 *   - REPL: SET key value | GET key | DEL key | KEYS | FLUSH | STATS | QUIT
 *   - Keys and values up to 256 bytes each
 *   - File format: simple length-prefixed records
 *
 * Compile: gcc kvdb.c -o kvdb
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Configuration
 * ---------------------------------------------------------------------- */

#define BUCKETS      256
#define MAX_KEYLEN   256
#define MAX_VALLEN   256
#define DB_FILE      "kvdb.dat"

/* ANSI helpers */
#define RED   "\033[1;31m"
#define GRN   "\033[1;32m"
#define YEL   "\033[1;33m"
#define CYN   "\033[1;36m"
#define MAG   "\033[1;35m"
#define RST   "\033[0m"

/* -------------------------------------------------------------------------
 * Data structures
 * ---------------------------------------------------------------------- */

typedef struct Entry {
    char          key[MAX_KEYLEN];
    char          val[MAX_VALLEN];
    struct Entry *next;
} Entry;

typedef struct {
    Entry  *buckets[BUCKETS];
    size_t  count;
    size_t  collisions;   /* buckets that have > 1 entry */
} HashMap;

/* -------------------------------------------------------------------------
 * Hash function — djb2
 * ---------------------------------------------------------------------- */

static uint32_t djb2(const char *s)
{
    uint32_t h = 5381;
    while (*s) h = ((h << 5) + h) ^ (unsigned char)*s++;
    return h;
}

static inline int bucket_of(const char *key)
{
    return (int)(djb2(key) % BUCKETS);
}

/* -------------------------------------------------------------------------
 * HashMap operations
 * ---------------------------------------------------------------------- */

static HashMap *hm_new(void)
{
    HashMap *hm = calloc(1, sizeof(HashMap));
    return hm;
}

static void hm_free(HashMap *hm)
{
    for (int i = 0; i < BUCKETS; i++) {
        Entry *e = hm->buckets[i];
        while (e) {
            Entry *nx = e->next;
            free(e);
            e = nx;
        }
    }
    free(hm);
}

/* Returns existing entry or NULL */
static Entry *hm_find(HashMap *hm, const char *key)
{
    int b = bucket_of(key);
    for (Entry *e = hm->buckets[b]; e; e = e->next)
        if (!strcmp(e->key, key)) return e;
    return NULL;
}

/* SET — returns 1 if new key, 0 if updated */
static int hm_set(HashMap *hm, const char *key, const char *val)
{
    Entry *e = hm_find(hm, key);
    if (e) {
        strncpy(e->val, val, MAX_VALLEN - 1);
        e->val[MAX_VALLEN - 1] = '\0';
        return 0;
    }

    int b = bucket_of(key);
    Entry *ne = calloc(1, sizeof(Entry));
    strncpy(ne->key, key, MAX_KEYLEN - 1);
    strncpy(ne->val, val, MAX_VALLEN - 1);

    if (hm->buckets[b]) hm->collisions++;   /* bucket already occupied */
    ne->next        = hm->buckets[b];
    hm->buckets[b]  = ne;
    hm->count++;
    return 1;
}

/* DEL — returns 1 on success, 0 if not found */
static int hm_del(HashMap *hm, const char *key)
{
    int     b    = bucket_of(key);
    Entry **prev = &hm->buckets[b];
    for (Entry *e = hm->buckets[b]; e; prev = &e->next, e = e->next) {
        if (!strcmp(e->key, key)) {
            *prev = e->next;
            free(e);
            hm->count--;
            /* Recount collisions for this bucket */
            int chain = 0;
            for (Entry *x = hm->buckets[b]; x; x = x->next) chain++;
            if (chain < 2 && hm->collisions > 0) hm->collisions--;
            return 1;
        }
    }
    return 0;
}

/* Iterate over all keys: callback(key, val, user) */
static void hm_each(HashMap *hm,
                    void (*cb)(const char *, const char *, void *),
                    void *user)
{
    for (int i = 0; i < BUCKETS; i++)
        for (Entry *e = hm->buckets[i]; e; e = e->next)
            cb(e->key, e->val, user);
}

/* -------------------------------------------------------------------------
 * Persistence
 *
 * File format (binary, little-endian lengths):
 *   [uint16_t klen][klen bytes key][uint16_t vlen][vlen bytes value] ...
 *   Repeat for every key/value pair. No null terminators in file.
 * ---------------------------------------------------------------------- */

static int write_u16(FILE *f, uint16_t v)
{
    return fwrite(&v, 2, 1, f) == 1 ? 0 : -1;
}

static int read_u16(FILE *f, uint16_t *out)
{
    return fread(out, 2, 1, f) == 1 ? 0 : -1;
}

static void cb_flush(const char *key, const char *val, void *user)
{
    FILE *f = (FILE *)user;
    uint16_t klen = (uint16_t)strlen(key);
    uint16_t vlen = (uint16_t)strlen(val);
    write_u16(f, klen);
    fwrite(key, 1, klen, f);
    write_u16(f, vlen);
    fwrite(val, 1, vlen, f);
}

static void db_flush(HashMap *hm)
{
    FILE *f = fopen(DB_FILE, "wb");
    if (!f) { perror("fopen"); return; }
    hm_each(hm, cb_flush, f);
    fclose(f);
}

static void db_load(HashMap *hm)
{
    FILE *f = fopen(DB_FILE, "rb");
    if (!f) return;   /* first run */

    char key[MAX_KEYLEN], val[MAX_VALLEN];
    uint16_t klen, vlen;

    while (read_u16(f, &klen) == 0) {
        if (klen == 0 || klen >= MAX_KEYLEN) break;
        if (fread(key, 1, klen, f) != klen) break;
        key[klen] = '\0';

        if (read_u16(f, &vlen) != 0) break;
        if (vlen >= MAX_VALLEN) break;
        if (fread(val, 1, vlen, f) != vlen) break;
        val[vlen] = '\0';

        hm_set(hm, key, val);
    }
    fclose(f);
}

/* -------------------------------------------------------------------------
 * Statistics helper
 * ---------------------------------------------------------------------- */

static void print_stats(HashMap *hm)
{
    size_t used_buckets = 0;
    size_t max_chain    = 0;
    for (int i = 0; i < BUCKETS; i++) {
        size_t chain = 0;
        for (Entry *e = hm->buckets[i]; e; e = e->next) chain++;
        if (chain) used_buckets++;
        if (chain > max_chain) max_chain = chain;
    }
    double load = (double)hm->count / BUCKETS;

    printf("%s--- Statistics ---%s\n", CYN, RST);
    printf("  Keys          : %zu\n",     hm->count);
    printf("  Buckets used  : %zu / %d\n", used_buckets, BUCKETS);
    printf("  Load factor   : %.3f\n",    load);
    printf("  Collision cnt : %zu\n",     hm->collisions);
    printf("  Longest chain : %zu\n",     max_chain);
    printf("  Storage file  : %s\n",      DB_FILE);
}

/* -------------------------------------------------------------------------
 * REPL helpers
 * ---------------------------------------------------------------------- */

/* Tokenise line into up to 3 tokens in-place */
static int tokenise(char *line, char **t, int max)
{
    int n = 0;
    char *p = line;
    while (n < max) {
        /* skip spaces */
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '\n') break;

        t[n++] = p;

        if (n == max) {
            /* Last token: consume rest of line (allows spaces in value) */
            size_t len = strlen(p);
            while (len > 0 && (p[len-1] == '\n' || p[len-1] == '\r' || p[len-1] == ' '))
                p[--len] = '\0';
            break;
        }

        /* advance to next whitespace */
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
        if (*p) *p++ = '\0';
    }
    return n;
}

/* -------------------------------------------------------------------------
 * main — REPL
 * ---------------------------------------------------------------------- */

int main(void)
{
    HashMap *hm = hm_new();
    db_load(hm);

    printf("%skvdb — persistent key-value store%s\n", CYN, RST);
    printf("Loaded %zu key(s) from " DB_FILE "\n", hm->count);
    printf("Commands: SET key value | GET key | DEL key | KEYS | FLUSH | STATS | QUIT\n\n");

    char  line[640];
    char *tok[3];

    while (1) {
        printf(YEL "kvdb> " RST);
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;

        int n = tokenise(line, tok, 3);
        if (n == 0) continue;

        /* Normalise command to uppercase */
        for (char *p = tok[0]; *p; p++)
            if (*p >= 'a' && *p <= 'z') *p -= 32;

        if (!strcmp(tok[0], "QUIT") || !strcmp(tok[0], "Q")) {
            db_flush(hm);
            printf("Database saved. Bye.\n");
            break;

        } else if (!strcmp(tok[0], "SET")) {
            if (n < 3) { printf("Usage: SET key value\n"); continue; }
            int isnew = hm_set(hm, tok[1], tok[2]);
            db_flush(hm);
            printf(GRN "%s \"%s\"\n" RST, isnew ? "Created" : "Updated", tok[1]);

        } else if (!strcmp(tok[0], "GET")) {
            if (n < 2) { printf("Usage: GET key\n"); continue; }
            Entry *e = hm_find(hm, tok[1]);
            if (e) printf(GRN "\"%s\"\n" RST, e->val);
            else   printf(RED "(nil)\n" RST);

        } else if (!strcmp(tok[0], "DEL")) {
            if (n < 2) { printf("Usage: DEL key\n"); continue; }
            if (hm_del(hm, tok[1])) {
                db_flush(hm);
                printf(GRN "Deleted \"%s\"\n" RST, tok[1]);
            } else {
                printf(RED "(key not found)\n" RST);
            }

        } else if (!strcmp(tok[0], "KEYS")) {
            if (hm->count == 0) {
                printf("(empty)\n");
            } else {
                size_t idx = 0;
                for (int i = 0; i < BUCKETS; i++)
                    for (Entry *e = hm->buckets[i]; e; e = e->next)
                        printf(MAG "[%3zu] " RST "%s\n", ++idx, e->key);
            }

        } else if (!strcmp(tok[0], "FLUSH")) {
            db_flush(hm);
            printf(GRN "Flushed %zu key(s) to disk.\n" RST, hm->count);

        } else if (!strcmp(tok[0], "STATS")) {
            print_stats(hm);

        } else {
            printf("Unknown command: %s\n", tok[0]);
        }
    }

    hm_free(hm);
    return 0;
}
