# URL Shortener in C (front end + back end with a hash map)

A complete, dependency-free URL shortener in a single C file (~185 lines,
heavily commented). No frameworks, no external libraries — just POSIX
sockets for the back end and one embedded HTML page for the front end.

## Build & run

```bash
gcc -O2 -Wall -o shortener url_shortener.c   # any C compiler works (cc/clang)
./shortener            # serves http://localhost:8080
./shortener 3000       # or pick another port
```

Then open `http://localhost:8080` in a browser, paste a long URL, click
**Shorten** — you get a short link like `http://localhost:8080/1` that
redirects to the original page.

## How it works

| Route | What happens |
|---|---|
| `GET /` | Server sends the embedded HTML/JS front-end page |
| `POST /shorten` (body: `url=...`) | Generates a short code, stores `code → URL` in the hash map, replies `{"code":"/1"}` |
| `GET /<code>` | Hash-map lookup; on a hit replies `302 Found` with `Location: <long URL>`, else `404` |

## The use case of the hash map

The entire service reduces to one question: *given a key (the short code),
return its value (the long URL)*. That is exactly what a hash map is for:

- **O(1) average lookup.** `hash(code) % BUCKETS` computes the bucket index
  directly, so a redirect never scans the table. A plain array would force
  an O(n) `strcmp` scan per click.
- **O(1) insert.** New short links are pushed onto the front of a bucket's
  chain in constant time.
- **Graceful collisions.** Two different codes can hash to the same bucket;
  *separate chaining* (a linked list per bucket) keeps both — no data is
  lost. A prime bucket count (103) spreads base-62 codes evenly, keeping
  chains short.

The implementation is three small pieces in `url_shortener.c`:

```c
typedef struct Entry {            /* a key/value pair in one bucket      */
    char key[8];                  /* short code, e.g. "aB3"              */
    char val[MAXLEN];             /* the long URL                         */
    struct Entry *next;           /* chain link for colliding keys        */
} Entry;

static Entry *table[BUCKETS];     /* the hash map: 103 bucket heads       */

static unsigned long hash(const char *s)      /* djb2 rolling string hash */
{
    unsigned long h = 5381;
    while (*s) h = h * 33 + (unsigned char)*s++;
    return h % BUCKETS;
}
```

`map_get()` walks only the one bucket `hash(key)` points at — that is the
whole payoff of hashing.

## File map

- `url_shortener.c` — everything: hash map, base-62 code generator,
  HTTP parsing/reply helpers, embedded front-end page, routing, server loop.
  Section comments (`1. THE HASH MAP`, `2. SHORT-CODE GENERATOR`, …) split it
  into readable pieces.

## Design choices & limits (documented on purpose)

- **In-memory only** — the map vanishes when the process exits. A production
  version would persist to disk or a database.
- **Serial request loop** — handles one connection at a time, which keeps the
  code minimal and the shared counter safe (no locking needed).
- **Base-62 counter codes** — short, unambiguous, and they never collide as
  *keys*, so every chain collision is only from the hash, handled by chaining.
- **`http`-prefix check** — a minimal sanity check on submitted URLs; a real
  deployment would validate much more (length caps, scheme allow-list, etc.).
