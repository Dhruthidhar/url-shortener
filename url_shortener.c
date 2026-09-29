/* ============================================================================
 * url_shortener.c — A tiny URL shortener (front end + back end) in pure C
 * ============================================================================
 * BACK END : A minimal HTTP server (POSIX sockets) that
 *              GET  /          -> serves the HTML front end below
 *              POST /shorten   -> stores the long URL in a HASH MAP and
 *                                 replies with a short code as JSON
 *              GET  /<code>    -> HASH MAP lookup of the code; if found,
 *                                 302-redirects to the long URL, else 404
 *
 * FRONT END: An embedded HTML page with a form; JavaScript POSTs the URL to
 *            /shorten and shows the returned short link as a clickable <a>.
 *
 * WHY A HASH MAP?
 *   The whole service is "give me a key (short code), give me back its value
 *   (long URL)". A hash map turns that into an O(1) average-time lookup:
 *   hash(code) -> bucket index -> walk a tiny chain. A linear array scan
 *   would be O(n) per click and would not scale past a toy number of links.
 *   Collisions (two codes hashing to one bucket) are handled by separate
 *   chaining, so the map degrades gracefully instead of losing data.
 *
 * BUILD : gcc -O2 -Wall -o shortener url_shortener.c
 * RUN   : ./shortener          (then open http://localhost:8080)
 *
 * NOTE  : The map lives in RAM and serves one request at a time — fine for a
 *         demo. A production version would persist to disk and fork/thread.
 * ==========================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define PORT    8080
#define BUCKETS 103              /* prime bucket count -> fewer collisions */
#define MAXLEN  8192             /* max size of one HTTP request           */

/* ------------------------- 1. THE HASH MAP --------------------------------*/
typedef struct Entry {           /* one key/value pair in a bucket's chain */
    char key[8];                 /* short code, e.g. "aB3x" (key)         */
    char val[MAXLEN];            /* long URL                    (value)   */
    struct Entry *next;          /* chain link for collisions              */
} Entry;

static Entry *table[BUCKETS];    /* the hash map: array of bucket heads    */

/* djb2 string hash: folds every character into a rolling number, then maps
 * it onto a bucket. Same key always lands in the same bucket -> O(1) lookups. */
static unsigned long hash(const char *s) {
    unsigned long h = 5381;
    while (*s) h = h * 33 + (unsigned char)*s++;
    return h % BUCKETS;
}

static Entry *map_get(const char *key) {           /* O(1) average lookup  */
    for (Entry *e = table[hash(key)]; e; e = e->next)
        if (strcmp(e->key, key) == 0) return e;
    return NULL;
}

static void map_put(const char *key, const char *val) {   /* insert at head */
    Entry *e = malloc(sizeof *e);
    snprintf(e->key, sizeof e->key, "%s", key);
    snprintf(e->val, sizeof e->val, "%s", val);
    unsigned long b = hash(key);
    e->next = table[b];
    table[b] = e;
}

/* ------------------------- 2. SHORT-CODE GENERATOR ------------------------*/
/* A monotonic counter rendered in base-62: 1,2,...,9,a..z,A..Z,10,11,...    */
static void new_code(char out[8]) {
    static long counter = 0;
    const char *base62 =
        "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    long n = ++counter;
    int i = 0;
    do { out[i++] = base62[n % 62]; n /= 62; } while (n && i < 7);
    out[i] = '\0';
}

/* ------------------------- 3. HTTP HELPERS --------------------------------*/
static void reply(int c, const char *status, const char *ctype,
                  const char *body) {
    dprintf(c, "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
              "Connection: close\r\n\r\n", status, ctype, strlen(body));
    send(c, body, strlen(body), 0);
}

/* Turn "%2F%3A..." form-encoding back into normal characters, in place.    */
static void url_decode(char *s) {
    char *o = s;
    while (*s) {
        if (s[0] == '%' && isxdigit((unsigned char)s[1]) &&
                          isxdigit((unsigned char)s[2])) {
            char hex[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            s += 3;
        } else
            *o++ = (*s == '+') ? ' ' : *s, s++;
    }
    *o = '\0';
}

/* ------------------------- 4. THE FRONT END (embedded HTML) ---------------*/
static const char PAGE[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'><title>C URL Shortener"
"</title><style>body{font-family:sans-serif;max-width:520px;margin:90px auto;"
"text-align:center}input{width:68%;padding:10px}button{padding:10px 18px}"
"#r a{color:#06c;font-size:1.2em}</style></head><body>"
"<h2>URL Shortener <small style='font-size:.5em'>(C + hash map)</small></h2>"
"<form onsubmit='return go(event)'>"
"<input id='u' placeholder='Paste a long URL here' required>"
"<button>Shorten</button></form><p id='r'></p><script>"
"async function go(e){e.preventDefault();"
"const r=await fetch('/shorten',{method:'POST',"
"body:'url='+encodeURIComponent(u.value)});const j=await r.json();"
"document.getElementById('r').innerHTML=j.code?"
"\"<a href='\"+j.code+\"'>\"+location.origin+j.code+\"</a>\":j.error;"
"return false;}</script></body></html>";

/* ------------------------- 5. THE BACK END (routing) ----------------------*/
static void handle(int c) {
    char buf[MAXLEN] = "", method[8] = "", path[MAXLEN] = "";
    read(c, buf, MAXLEN - 1);
    sscanf(buf, "%7s %8191s", method, path);        /* request line only */

    if (!strcmp(method, "GET") && !strcmp(path, "/"))          /* front end */
        reply(c, "200 OK", "text/html", PAGE);

    else if (!strcmp(method, "POST") && !strcmp(path, "/shorten")) { /* API */
        char *url = strstr(buf, "url=");
        if (!url) { reply(c, "400 Bad Request", "application/json",
                          "{\"error\":\"missing url field\"}"); return; }
        url += 4;
        url_decode(url);
        for (char *p = url; *p; p++)                  /* trim trailing CR/LF */
            if (*p == '\r' || *p == '\n') { *p = '\0'; break; }
        if (strncmp(url, "http", 4)) {                /* sanity check       */
            reply(c, "400 Bad Request", "application/json",
                  "{\"error\":\"url must start with http\"}"); return;
        }
        char code[8];
        new_code(code);
        map_put(code, url);                           /* store in hash map */
        char body[64];
        snprintf(body, sizeof body, "{\"code\":\"/%s\"}", code);
        reply(c, "200 OK", "application/json", body);
    }

    else if (!strcmp(method, "GET") && path[0] == '/' && path[1]) {
        Entry *e = map_get(path + 1);                 /* O(1) hash lookup  */
        if (e) dprintf(c, "HTTP/1.1 302 Found\r\nLocation: %s\r\n"
                          "Content-Length: 0\r\n\r\n", e->val);
        else reply(c, "404 Not Found", "text/html",
                  "<h2>404 — short code not found</h2>");
    }

    else reply(c, "404 Not Found", "text/html", "<h2>404</h2>");
}

int main(int argc, char **argv) {
    int port = argc > 1 ? atoi(argv[1]) : PORT;   /* optional: ./shortener 3000 */
    signal(SIGCHLD, SIG_IGN);
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { perror("socket"); return 1; }
    struct sockaddr_in a = { .sin_family = AF_INET,
                             .sin_addr.s_addr = INADDR_ANY,
                             .sin_port = htons(port) };
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0) { perror("bind"); return 1; }
    listen(s, 16);
    printf("URL shortener running -> http://localhost:%d\n", port);

    for (;;) {                       /* serial accept loop: one request at a */
        int c = accept(s, NULL, NULL);
        if (c < 0) continue;
        handle(c);
        close(c);
    }
}
