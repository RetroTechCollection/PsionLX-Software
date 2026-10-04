/*
 * psionnet-spotify -- Spotify for the Psion netBook Pro running PsionLX.
 *
 * The netBook Pro cannot talk to Spotify itself: its TLS stops at 1.0 and no
 * Spotify client was ever built for its 2005 ARM userspace. PsionNet, on a
 * Mac on the same network, does the Spotify half -- a Spotify Connect speaker
 * called "netBook Pro", search, playlists and control -- and serves it over
 * plain HTTP. This program is the netBook Pro's half:
 *
 *   - finds PsionNet by UDP broadcast ("PSIONNET?" to port 8899), or asks
 *   - browses and searches through PsionNet's line-based text API
 *   - plays the audio PsionNet streams at /spotify/stream.mp3 with the
 *     GStreamer 0.8 already in Psion's image:
 *         gst-launch-0.8 gnomevfssrc location=... ! mad ! audioconvert ! esdsink
 *     but only while Spotify says this machine is the one playing
 *
 * Written for GTK 2.4 and GLib 2.4, as Psion shipped them: nothing newer is
 * used (no ellipsizing labels, no media stock icons, no g_file_set_contents),
 * and every symbol is checked against Psion's libraries after the build.
 *
 * GPL-2.0-or-later, as part of PsionNet.
 */

#include <gtk/gtk.h>
#include <glib.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "icons.h"

#define APP_NAME        "Spotify"
#define DISCOVERY_PORT  8899
#define DEFAULT_PORT    8080
#define POLL_MS         2000
#define USER_AGENT      "PsionLX-Spotify/1.0"
#define MAX_BODY        (2 * 1024 * 1024)

/* --- server address ----------------------------------------------------- */

static char server_host[128] = "";
static int  server_port = DEFAULT_PORT;
G_LOCK_DEFINE_STATIC(server);
G_LOCK_DEFINE_STATIC(resolver);        /* gethostbyname() is not thread-safe */

static char *config_path(void)
{
    return g_build_filename(g_get_home_dir(), ".gpe", "psionnet-spotify", NULL);
}

static void load_config(void)
{
    char *path = config_path(), *text = NULL;
    if (g_file_get_contents(path, &text, NULL, NULL)) {
        char **lines = g_strsplit(text, "\n", 0);
        int i;
        for (i = 0; lines[i]; i++) {
            if (g_str_has_prefix(lines[i], "server=")) {
                char *v = g_strstrip(lines[i] + 7), *colon = strrchr(v, ':');
                if (colon) {
                    *colon = '\0';
                    server_port = atoi(colon + 1) > 0 ? atoi(colon + 1) : DEFAULT_PORT;
                }
                g_strlcpy(server_host, v, sizeof server_host);
            }
        }
        g_strfreev(lines);
        g_free(text);
    }
    g_free(path);
}

static void save_config(void)
{
    char *dir = g_build_filename(g_get_home_dir(), ".gpe", NULL), *path = config_path();
    FILE *f;
    mkdir(dir, 0700);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "server=%s:%d\n", server_host, server_port);
        fclose(f);
    }
    g_free(dir);
    g_free(path);
}

/* --- HTTP: tiny, blocking, run on the worker thread --------------------- */

static int connect_with_timeout(const char *host, int port, int seconds)
{
    struct sockaddr_in sa;
    struct timeval tv;
    fd_set w;
    int fd, flags, err = 0;
    socklen_t len = sizeof err;

    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (!inet_aton(host, &sa.sin_addr)) {
        struct hostent *he;
        gboolean found = FALSE;
        G_LOCK(resolver);
        he = gethostbyname(host);
        if (he && he->h_addrtype == AF_INET) {
            memcpy(&sa.sin_addr, he->h_addr_list[0], sizeof sa.sin_addr);
            found = TRUE;
        }
        G_UNLOCK(resolver);
        if (!found)
            return -1;
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    if (connect(fd, (struct sockaddr *) &sa, sizeof sa) < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    FD_ZERO(&w);
    FD_SET(fd, &w);
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    if (select(fd + 1, NULL, &w, NULL, &tv) <= 0
        || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, flags);
    tv.tv_sec = 20;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    return fd;
}

/* Returns the HTTP status, or -1 if PsionNet could not be reached. */
static int http_get(const char *host, int port, const char *path, GString **body)
{
    char *req;
    char buf[4096];
    GString *raw = g_string_new(NULL);
    int fd, n, status = -1;
    char *hdr_end;

    *body = NULL;
    fd = connect_with_timeout(host, port, 4);
    if (fd < 0) {
        g_string_free(raw, TRUE);
        return -1;
    }
    req = g_strdup_printf("GET %s HTTP/1.0\r\nHost: %s:%d\r\nUser-Agent: " USER_AGENT
                          "\r\nX-PsionNet-Client: psionlx-spotify/1\r\n"
                          "Connection: close\r\n\r\n", path, host, port);
    n = write(fd, req, strlen(req));
    g_free(req);
    if (n <= 0) {
        close(fd);
        g_string_free(raw, TRUE);
        return -1;
    }
    while ((n = read(fd, buf, sizeof buf)) > 0 && raw->len < MAX_BODY)
        g_string_append_len(raw, buf, n);
    close(fd);
    if (raw->len > 12 && g_str_has_prefix(raw->str, "HTTP/"))
        status = atoi(raw->str + 9);
    hdr_end = strstr(raw->str, "\r\n\r\n");
    if (hdr_end) {
        gsize off = (hdr_end + 4) - raw->str;
        *body = g_string_new_len(raw->str + off, raw->len - off);
    }
    g_string_free(raw, TRUE);
    return status;
}

static char *url_escape(const char *s)
{
    GString *out = g_string_new(NULL);
    for (; *s; s++) {
        unsigned char c = (unsigned char) *s;
        if (g_ascii_isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            g_string_append_c(out, c);
        else
            g_string_append_printf(out, "%%%02X", c);
    }
    return g_string_free(out, FALSE);
}

/* --- discovery ------------------------------------------------------------ */

/* Where to send "PSIONNET?": the limited broadcast, and each interface's own
   subnet broadcast, which some networks pass when they drop the other. */
static int broadcast_addrs(int fd, struct in_addr *out, int max)
{
    struct ifreq reqs[16];
    struct ifconf ifc;
    int n = 0, i;

    out[n++].s_addr = htonl(INADDR_BROADCAST);
    ifc.ifc_len = sizeof reqs;
    ifc.ifc_req = reqs;
    if (ioctl(fd, SIOCGIFCONF, &ifc) < 0)
        return n;
    for (i = 0; i < ifc.ifc_len / (int) sizeof(struct ifreq) && n < max; i++) {
        struct ifreq r = reqs[i];
        if (ioctl(fd, SIOCGIFFLAGS, &r) < 0 || !(r.ifr_flags & IFF_UP)
            || (r.ifr_flags & IFF_LOOPBACK) || !(r.ifr_flags & IFF_BROADCAST))
            continue;
        r = reqs[i];
        if (ioctl(fd, SIOCGIFBRDADDR, &r) == 0)
            out[n++] = ((struct sockaddr_in *) &r.ifr_broadaddr)->sin_addr;
    }
    return n;
}

/* PsionNet answers "PSIONNET <address> <port>". The address is the one its
   proxy listens on; if that is a loopback address (a test on the Mac itself,
   or an emulator), the sender of the reply is the way to reach it instead. */
static gboolean discover(char *host, size_t hostlen, int *port)
{
    struct sockaddr_in to, from;
    struct in_addr bcast[8];
    struct timeval tv;
    fd_set r;
    char buf[256];
    socklen_t flen;
    int fd, on = 1, n, nb, b, attempt;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return FALSE;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof on);
    nb = broadcast_addrs(fd, bcast, 8);
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons(DISCOVERY_PORT);
    for (attempt = 0; attempt < 3; attempt++) {
        for (b = 0; b < nb; b++) {
            to.sin_addr = bcast[b];
            sendto(fd, "PSIONNET?\n", 10, 0, (struct sockaddr *) &to, sizeof to);
        }
        FD_ZERO(&r);
        FD_SET(fd, &r);
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        while (select(fd + 1, &r, NULL, NULL, &tv) > 0) {
            char h[128];
            int p = 0;
            flen = sizeof from;
            n = recvfrom(fd, buf, sizeof buf - 1, 0, (struct sockaddr *) &from, &flen);
            if (n <= 9)
                continue;
            buf[n] = '\0';
            if (sscanf(buf, "PSIONNET %127s %d", h, &p) != 2 || p <= 0)
                continue;
            if (g_str_has_prefix(h, "127.") || !strcmp(h, "0.0.0.0")) {
                unsigned char *a = (unsigned char *) &from.sin_addr;
                g_snprintf(h, sizeof h, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
            }
            g_strlcpy(host, h, hostlen);
            *port = p;
            close(fd);
            return TRUE;
        }
    }
    close(fd);
    return FALSE;
}

/* --- the worker thread ----------------------------------------------------- */

typedef enum { J_DISCOVER, J_HELLO, J_PLAYLISTS, J_TRACKS, J_SEARCH, J_STATUS,
               J_COMMAND, J_ART } JobKind;

typedef struct {
    JobKind kind;
    char *path;          /* request path, or NULL */
    char *tag;           /* what the reply belongs to (a context URI, an art URL) */
    int status;
    GString *body;
} Job;

static GAsyncQueue *jobs;
static gboolean job_done(gpointer data);

static gpointer worker(gpointer unused)
{
    for (;;) {
        Job *job = g_async_queue_pop(jobs);
        if (job->kind == J_DISCOVER) {
            char h[128];
            int p = DEFAULT_PORT;
            if (discover(h, sizeof h, &p)) {
                G_LOCK(server);
                g_strlcpy(server_host, h, sizeof server_host);
                server_port = p;
                G_UNLOCK(server);
                job->status = 200;
            } else {
                job->status = -1;
            }
        } else {
            char host[128];
            int port;
            G_LOCK(server);
            g_strlcpy(host, server_host, sizeof host);
            port = server_port;
            G_UNLOCK(server);
            job->status = host[0] ? http_get(host, port, job->path, &job->body) : -1;
        }
        g_idle_add(job_done, job);
    }
    return NULL;
}

static void submit(JobKind kind, char *path, const char *tag)
{
    Job *job = g_new0(Job, 1);
    job->kind = kind;
    job->path = path;                 /* takes ownership */
    job->tag = tag ? g_strdup(tag) : NULL;
    g_async_queue_push(jobs, job);
}

/* Split a text reply into rows of fields. Returns NULL for "ERR", with *err set. */
static GPtrArray *parse_reply(Job *job, char **err)
{
    GPtrArray *rows;
    char **lines;
    int i;

    *err = NULL;
    if (job->status < 0 || !job->body) {
        *err = g_strdup("PsionNet is not answering.");
        return NULL;
    }
    lines = g_strsplit(job->body->str, "\n", 0);
    if (!lines[0] || strcmp(lines[0], "OK") != 0) {
        char *tab = lines[0] ? strchr(lines[0], '\t') : NULL;
        *err = g_strdup(tab ? tab + 1 : "PsionNet sent something unexpected.");
        g_strfreev(lines);
        return NULL;
    }
    rows = g_ptr_array_new();
    for (i = 1; lines[i]; i++)
        if (lines[i][0])
            g_ptr_array_add(rows, g_strsplit(lines[i], "\t", 0));
    g_strfreev(lines);
    return rows;
}

static void free_rows(GPtrArray *rows)
{
    guint i;
    if (!rows)
        return;
    for (i = 0; i < rows->len; i++)
        g_strfreev(g_ptr_array_index(rows, i));
    g_ptr_array_free(rows, TRUE);
}

static const char *field(char **row, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (!row[i])
            return "";
    return row[n] ? row[n] : "";
}

/* --- UI state ----------------------------------------------------------- */

enum { P_NAME, P_URI, P_COLS };                       /* left: playlists */
enum { T_TITLE, T_ARTIST, T_ALBUM, T_TIME, T_URI, T_KIND, T_COLS };   /* right */

static GtkWidget *window, *status_label, *server_button, *search_entry, *search_type;
static GtkWidget *play_image, *play_button, *art_image, *title_label, *sub_label;
static GtkWidget *time_label, *where_label, *volume_scale, *list_view;
static GtkListStore *playlists, *tracks;
static GdkPixbuf *pix_play, *pix_pause;

static char *current_context = NULL;      /* playlist/album/liked URI of the track list */
static char *current_art = NULL;
static gboolean ready = FALSE, playing = FALSE, here = FALSE;
static gboolean status_pending = FALSE, volume_from_server = FALSE;
static guint volume_timer = 0;
static GPid player_pid = 0;
static guint player_retry = 0;
static guint hello_retry = 0;
static int hello_failures = 0;
static gboolean asked = FALSE;            /* the address dialog has been offered */

static void set_status(const char *text, gboolean bad)
{
    char *m = g_markup_printf_escaped(bad ? "<span foreground=\"#a01818\">%s</span>"
                                          : "%s", text);
    gtk_label_set_markup(GTK_LABEL(status_label), m);
    g_free(m);
}

static void update_server_button(void)
{
    char *t = server_host[0] ? g_strdup_printf("PsionNet: %s", server_host)
                             : g_strdup("PsionNet: not found");
    gtk_button_set_label(GTK_BUTTON(server_button), t);
    g_free(t);
}

static char *fmt_time(const char *ms_text)
{
    long ms = atol(ms_text ? ms_text : "0");
    return g_strdup_printf("%ld:%02ld", ms / 60000, (ms / 1000) % 60);
}

/* Truncate on a UTF-8 boundary: GTK 2.4 labels cannot ellipsize themselves. */
static char *clip(const char *s, int chars)
{
    if (g_utf8_strlen(s, -1) <= chars)
        return g_strdup(s);
    {
        char *end = g_utf8_offset_to_pointer(s, chars - 1);
        GString *g = g_string_new_len(s, end - s);
        g_string_append(g, "\xe2\x80\xa6");
        return g_string_free(g, FALSE);
    }
}

/* --- the player: gst-launch, only while this machine is the one playing --- */

static char *player_log = NULL;          /* ~/.gpe/psionnet-spotify.log */
static GTimeVal player_started;
static gboolean player_killed = FALSE;
static int player_failures = 0;

static void sync_player(void);

/* Runs in the child between fork and exec: no allocation here. */
static void player_child_setup(gpointer path)
{
    int fd = open((const char *) path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        dup2(fd, 1);
        dup2(fd, 2);
        close(fd);
    }
}

static void player_exited(GPid pid, gint status, gpointer unused)
{
    GTimeVal now;

    g_spawn_close_pid(pid);
    if (pid != player_pid)
        return;
    player_pid = 0;
    g_get_current_time(&now);
    if (player_killed || now.tv_sec - player_started.tv_sec >= 10)
        player_failures = 0;
    else
        player_failures++;
    player_killed = FALSE;
    if (player_failures >= 3 && ready && playing && here)
        set_status("The sound will not start. Is the sound server running? "
                   "Details in ~/.gpe/psionnet-spotify.log", TRUE);
    sync_player();
}

static void start_player(void)
{
    char *location;
    char *argv[10];
    GError *error = NULL;

    if (player_pid)
        return;
    if (!player_log)
        player_log = g_build_filename(g_get_home_dir(), ".gpe", "psionnet-spotify.log", NULL);
    location = g_strdup_printf("location=http://%s:%d/spotify/stream.mp3",
                               server_host, server_port);
    argv[0] = "gst-launch-0.8";
    argv[1] = "gnomevfssrc";
    argv[2] = location;
    argv[3] = "!";
    argv[4] = "mad";
    argv[5] = "!";
    argv[6] = "audioconvert";
    argv[7] = "!";
    argv[8] = "esdsink";
    argv[9] = NULL;
    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                       player_child_setup, player_log, &player_pid, &error)) {
        set_status(error->message, TRUE);
        g_error_free(error);
        player_pid = 0;
        player_failures++;
    } else {
        g_get_current_time(&player_started);
        g_child_watch_add(player_pid, player_exited, NULL);
    }
    g_free(location);
}

static void stop_player(void)
{
    if (player_pid) {
        player_killed = TRUE;
        kill(player_pid, SIGTERM);
    }
}

static gboolean player_retry_cb(gpointer unused)
{
    player_retry = 0;
    if (ready && playing && here && !player_pid)
        start_player();
    return FALSE;
}

/* Run gst-launch exactly while Spotify is playing on this machine. If it keeps
   dying (no sound server, network trouble), wait longer each time: 1.5 s, 3 s,
   6 s ... up to 24 s, rather than respawning it in a tight loop. */
static void sync_player(void)
{
    if (ready && playing && here) {
        if (player_pid || player_retry)
            return;
        if (player_failures == 0)
            start_player();
        else
            player_retry = g_timeout_add(1500 << MIN(player_failures - 1, 4),
                                         player_retry_cb, NULL);
    } else {
        if (player_retry) {
            g_source_remove(player_retry);
            player_retry = 0;
        }
        player_failures = 0;
        stop_player();
    }
}

/* --- requests ----------------------------------------------------------- */

static void command(const char *what)
{
    submit(J_COMMAND, g_strdup_printf("/spotify/%s", what), what);
}

static void request_status(void)
{
    if (status_pending || !server_host[0])
        return;
    status_pending = TRUE;
    submit(J_STATUS, g_strdup("/spotify/status"), NULL);
}

static void load_tracks(const char *uri)
{
    char *e = url_escape(uri);
    submit(J_TRACKS, g_strdup_printf("/spotify/tracks?uri=%s", e), uri);
    g_free(e);
    set_status("Loading\xe2\x80\xa6", FALSE);
}

static void hello(void)
{
    update_server_button();
    submit(J_HELLO, g_strdup("/spotify/hello"), NULL);
}

static gboolean hello_retry_cb(gpointer unused)
{
    hello_retry = 0;
    hello();
    return FALSE;
}

static gboolean discover_retry_cb(gpointer unused)
{
    hello_retry = 0;
    submit(J_DISCOVER, NULL, NULL);
    return FALSE;
}

static void retry_later(GSourceFunc what, int ms)
{
    if (!hello_retry)
        hello_retry = g_timeout_add(ms, what, NULL);
}

/* --- filling the lists ---------------------------------------------------- */

static void fill_tracks(GPtrArray *rows)
{
    guint i;
    gtk_list_store_clear(tracks);
    for (i = 0; i < rows->len; i++) {
        char **row = g_ptr_array_index(rows, i);
        const char *kind = field(row, 0);
        GtkTreeIter it;
        char *time;
        gtk_list_store_append(tracks, &it);
        if (strcmp(kind, "track") == 0) {
            time = fmt_time(field(row, 5));
            gtk_list_store_set(tracks, &it, T_TITLE, field(row, 2), T_ARTIST, field(row, 3),
                               T_ALBUM, field(row, 4), T_TIME, time, T_URI, field(row, 1),
                               T_KIND, kind, -1);
            g_free(time);
        } else if (strcmp(kind, "album") == 0) {
            char *a = field(row, 4)[0] ? g_strdup_printf("Album, %s", field(row, 4))
                                       : g_strdup("Album");
            gtk_list_store_set(tracks, &it, T_TITLE, field(row, 2), T_ARTIST, field(row, 3),
                               T_ALBUM, a, T_TIME, "", T_URI, field(row, 1),
                               T_KIND, kind, -1);
            g_free(a);
        } else {
            char *n = g_strdup_printf("Playlist, %s songs", field(row, 4));
            gtk_list_store_set(tracks, &it, T_TITLE, field(row, 2), T_ARTIST, field(row, 3),
                               T_ALBUM, n, T_TIME, "", T_URI, field(row, 1),
                               T_KIND, kind, -1);
            g_free(n);
        }
    }
}

static void fill_playlists(GPtrArray *rows)
{
    guint i;
    gtk_list_store_clear(playlists);
    for (i = 0; i < rows->len; i++) {
        char **row = g_ptr_array_index(rows, i);
        GtkTreeIter it;
        gtk_list_store_append(playlists, &it);
        gtk_list_store_set(playlists, &it, P_NAME, field(row, 2), P_URI, field(row, 1), -1);
    }
}

static void show_status(GPtrArray *rows)
{
    const char *title = "", *artist = "", *album = "", *art = "", *device = "";
    const char *progress = "0", *duration = "0", *volume = "", *state = "";
    gboolean was_ready = ready;
    guint i;
    char *t, *s, *a, *b, *m;

    playing = here = FALSE;
    for (i = 0; i < rows->len; i++) {
        char **row = g_ptr_array_index(rows, i);
        const char *k = field(row, 0), *v = field(row, 1);
        if (!strcmp(k, "state")) state = v;
        else if (!strcmp(k, "playing")) playing = !strcmp(v, "1");
        else if (!strcmp(k, "here")) here = !strcmp(v, "1");
        else if (!strcmp(k, "title")) title = v;
        else if (!strcmp(k, "artist")) artist = v;
        else if (!strcmp(k, "album")) album = v;
        else if (!strcmp(k, "art")) art = v;
        else if (!strcmp(k, "device")) device = v;
        else if (!strcmp(k, "progress")) progress = v;
        else if (!strcmp(k, "duration")) duration = v;
        else if (!strcmp(k, "volume")) volume = v;
    }
    ready = !strcmp(state, "ready");
    if (!ready) {
        const char *msg = "";
        for (i = 0; i < rows->len; i++) {
            char **row = g_ptr_array_index(rows, i);
            if (!strcmp(field(row, 0), "message"))
                msg = field(row, 1);
        }
        set_status(msg[0] ? msg : "Waiting for PsionNet\xe2\x80\xa6", TRUE);
        sync_player();
        return;
    }
    if (!was_ready)
        set_status("Ready.", FALSE);

    t = clip(title[0] ? title : "Nothing playing", 46);
    m = g_markup_printf_escaped("<b>%s</b>", t);
    gtk_label_set_markup(GTK_LABEL(title_label), m);
    g_free(m);
    g_free(t);
    s = g_strdup_printf("%s%s%s", artist, (artist[0] && album[0]) ? "  \xc2\xb7  " : "", album);
    t = clip(s, 60);
    gtk_label_set_text(GTK_LABEL(sub_label), t);
    g_free(t);
    g_free(s);
    a = fmt_time(progress);
    b = fmt_time(duration);
    s = title[0] ? g_strdup_printf("%s / %s", a, b) : g_strdup("");
    gtk_label_set_text(GTK_LABEL(time_label), s);
    g_free(a);
    g_free(b);
    g_free(s);
    if (!title[0])
        s = g_strdup("");
    else if (here)
        s = g_strdup(playing ? "Playing on this netBook Pro" : "Paused");
    else
        s = g_strdup_printf("%s on %s \xe2\x80\x94 press Play to listen here",
                            playing ? "Playing" : "Paused", device[0] ? device : "another device");
    gtk_label_set_text(GTK_LABEL(where_label), s);
    g_free(s);
    gtk_image_set_from_pixbuf(GTK_IMAGE(play_image), (playing && here) ? pix_pause : pix_play);

    if (volume[0] && !volume_timer) {
        volume_from_server = TRUE;
        gtk_range_set_value(GTK_RANGE(volume_scale), atoi(volume));
        volume_from_server = FALSE;
    }
    if (strcmp(art, current_art ? current_art : "") != 0) {
        g_free(current_art);
        current_art = g_strdup(art);
        if (art[0]) {
            char *e = url_escape(art);
            submit(J_ART, g_strdup_printf("/spotify/art?u=%s&s=64", e), art);
            g_free(e);
        } else {
            gtk_image_set_from_pixbuf(GTK_IMAGE(art_image), NULL);
        }
    }
    sync_player();
}

static void show_art(Job *job)
{
    GdkPixbufLoader *loader;
    GdkPixbuf *pix;

    if (!job->tag || !current_art || strcmp(job->tag, current_art) != 0)
        return;                     /* the track changed while this was loading */
    if (job->status != 200 || !job->body || !job->body->len)
        return;
    loader = gdk_pixbuf_loader_new();
    if (gdk_pixbuf_loader_write(loader, (const guchar *) job->body->str, job->body->len, NULL)
        && gdk_pixbuf_loader_close(loader, NULL)) {
        pix = gdk_pixbuf_loader_get_pixbuf(loader);
        if (pix) {
            GdkPixbuf *scaled = gdk_pixbuf_scale_simple(pix, 64, 64, GDK_INTERP_BILINEAR);
            gtk_image_set_from_pixbuf(GTK_IMAGE(art_image), scaled);
            g_object_unref(scaled);
        }
    } else {
        gdk_pixbuf_loader_close(loader, NULL);
    }
    g_object_unref(loader);
}

/* --- when a reply comes back --------------------------------------------- */

static void ask_for_server(void);

static gboolean job_done(gpointer data)
{
    Job *job = data;
    GPtrArray *rows = NULL;
    char *err = NULL;

    switch (job->kind) {
    case J_DISCOVER:
        if (job->status == 200) {
            save_config();
            hello();
        } else if (server_host[0]) {
            retry_later(hello_retry_cb, 5000);      /* keep trying the one we had */
        } else {
            set_status("Could not find PsionNet on the network. Is it running, with "
                       "\"netBook Pro (PsionLX, network)\" chosen?", TRUE);
            update_server_button();
            if (!asked) {
                asked = TRUE;
                ask_for_server();
            }
            if (!server_host[0])
                retry_later(discover_retry_cb, 10000);
        }
        break;
    case J_HELLO:
        rows = parse_reply(job, &err);
        if (!rows) {
            set_status(err, TRUE);
            /* Unreachable twice running: the Mac may have a new address. */
            if (job->status < 0 && ++hello_failures % 4 == 2)
                retry_later(discover_retry_cb, 1000);
            else
                retry_later(hello_retry_cb, 5000);
            break;
        }
        hello_failures = 0;
        save_config();
        submit(J_PLAYLISTS, g_strdup("/spotify/playlists"), NULL);
        request_status();
        break;
    case J_PLAYLISTS:
        rows = parse_reply(job, &err);
        if (rows) {
            fill_playlists(rows);
            if (rows->len && !current_context) {
                GtkTreeIter it;
                if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(playlists), &it)) {
                    char *uri;
                    gtk_tree_model_get(GTK_TREE_MODEL(playlists), &it, P_URI, &uri, -1);
                    load_tracks(uri);
                    g_free(uri);
                }
            }
        } else {
            set_status(err, TRUE);
            retry_later(hello_retry_cb, 5000);
        }
        break;
    case J_TRACKS:
    case J_SEARCH:
        rows = parse_reply(job, &err);
        if (rows) {
            fill_tracks(rows);
            if (job->kind == J_TRACKS) {
                g_free(current_context);
                current_context = g_strdup(job->tag);
            } else {
                g_free(current_context);
                current_context = NULL;
            }
            set_status(rows->len ? "Ready." : "Nothing found.", FALSE);
        } else {
            set_status(err, TRUE);
        }
        break;
    case J_STATUS:
        status_pending = FALSE;
        rows = parse_reply(job, &err);
        if (rows)
            show_status(rows);
        else {
            ready = FALSE;
            set_status(err, TRUE);
            sync_player();
        }
        break;
    case J_COMMAND:
        rows = parse_reply(job, &err);
        set_status(rows ? "" : err, rows == NULL);
        request_status();
        break;
    case J_ART:
        show_art(job);
        break;
    }
    free_rows(rows);
    g_free(err);
    if (job->body)
        g_string_free(job->body, TRUE);
    g_free(job->path);
    g_free(job->tag);
    g_free(job);
    return FALSE;
}

/* --- user actions ----------------------------------------------------------- */

static void on_search(GtkWidget *w, gpointer unused)
{
    static const char *types[] = { "track", "album", "playlist" };
    const char *q = gtk_entry_get_text(GTK_ENTRY(search_entry));
    int t = gtk_combo_box_get_active(GTK_COMBO_BOX(search_type));
    char *e;
    if (!q[0])
        return;
    e = url_escape(q);
    submit(J_SEARCH, g_strdup_printf("/spotify/search?q=%s&type=%s", e,
                                     types[t >= 0 && t < 3 ? t : 0]), NULL);
    g_free(e);
    set_status("Searching\xe2\x80\xa6", FALSE);
    gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(
        GTK_TREE_VIEW(g_object_get_data(G_OBJECT(window), "playlist-view"))));
}

static void on_playlist_selected(GtkTreeSelection *sel, gpointer unused)
{
    GtkTreeModel *model;
    GtkTreeIter it;
    char *uri;
    if (!gtk_tree_selection_get_selected(sel, &model, &it))
        return;
    gtk_tree_model_get(model, &it, P_URI, &uri, -1);
    if (!current_context || strcmp(uri, current_context) != 0)
        load_tracks(uri);
    g_free(uri);
}

static void on_track_activated(GtkTreeView *view, GtkTreePath *path,
                               GtkTreeViewColumn *col, gpointer unused)
{
    GtkTreeIter it;
    char *uri, *kind, *e, *c;
    if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(tracks), &it, path))
        return;
    gtk_tree_model_get(GTK_TREE_MODEL(tracks), &it, T_URI, &uri, T_KIND, &kind, -1);
    if (strcmp(kind, "track") != 0) {
        load_tracks(uri);                 /* an album or playlist from search */
    } else {
        e = url_escape(uri);
        c = url_escape(current_context ? current_context : "");
        submit(J_COMMAND, g_strdup_printf("/spotify/play?uri=%s&context=%s", e, c), "play");
        g_free(e);
        g_free(c);
        set_status("Starting\xe2\x80\xa6", FALSE);
    }
    g_free(uri);
    g_free(kind);
}

static void on_play(GtkWidget *w, gpointer unused)
{
    if (playing && here) {
        stop_player();                    /* silence now, not after the buffer drains */
        command("pause");
    } else {
        command("resume");
    }
}

static void on_next(GtkWidget *w, gpointer unused) { command("next"); }
static void on_prev(GtkWidget *w, gpointer unused) { command("previous"); }

static gboolean send_volume(gpointer unused)
{
    char *v = g_strdup_printf("volume?v=%d", (int) gtk_range_get_value(GTK_RANGE(volume_scale)));
    volume_timer = 0;
    command(v);
    g_free(v);
    return FALSE;
}

static void on_volume(GtkRange *range, gpointer unused)
{
    if (volume_from_server)
        return;
    if (volume_timer)
        g_source_remove(volume_timer);
    volume_timer = g_timeout_add(400, send_volume, NULL);
}

static gboolean poll_status(gpointer unused)
{
    request_status();
    return TRUE;
}

static void ask_for_server(void)
{
    GtkWidget *dlg, *entry, *label;
    dlg = gtk_dialog_new_with_buttons("PsionNet", GTK_WINDOW(window), GTK_DIALOG_MODAL,
                                      GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                                      GTK_STOCK_OK, GTK_RESPONSE_OK, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
    label = gtk_label_new("Enter the address PsionNet shows on the Mac,\n"
                          "for example 192.168.1.4. The Mac must be running\n"
                          "PsionNet with \"netBook Pro (PsionLX, network)\".");
    gtk_misc_set_padding(GTK_MISC(label), 12, 8);
    entry = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_entry_set_text(GTK_ENTRY(entry), server_host);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->vbox), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->vbox), entry, FALSE, FALSE, 8);
    gtk_widget_show_all(dlg);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        char *v = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(entry))));
        char *colon = strrchr(v, ':');
        if (colon) {
            *colon = '\0';
            server_port = atoi(colon + 1) > 0 ? atoi(colon + 1) : DEFAULT_PORT;
        } else {
            server_port = DEFAULT_PORT;
        }
        if (v[0]) {
            G_LOCK(server);
            g_strlcpy(server_host, v, sizeof server_host);
            G_UNLOCK(server);
            hello();
        }
        g_free(v);
    }
    gtk_widget_destroy(dlg);
}

static void on_server(GtkWidget *w, gpointer unused) { ask_for_server(); }

static void on_quit(GtkWidget *w, gpointer unused)
{
    stop_player();
    gtk_main_quit();
}

/* --- building the window ---------------------------------------------------- */

static GtkWidget *icon_button(const char **xpm, GtkWidget **image_out, GCallback cb)
{
    GdkPixbuf *pix = gdk_pixbuf_new_from_xpm_data(xpm);
    GtkWidget *img = gtk_image_new_from_pixbuf(pix), *b = gtk_button_new();
    gtk_container_add(GTK_CONTAINER(b), img);
    gtk_widget_set_size_request(b, 44, 36);
    g_signal_connect(b, "clicked", cb, NULL);
    if (image_out)
        *image_out = img;
    g_object_unref(pix);
    return b;
}

static GtkWidget *scrolled(GtkWidget *child)
{
    GtkWidget *s = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(s), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(s), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(s), child);
    return s;
}

static GtkTreeViewColumn *text_column(const char *title, int col, int width, gboolean expand)
{
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(title, r, "text", col, NULL);
    gtk_tree_view_column_set_sizing(c, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width(c, width);
    gtk_tree_view_column_set_resizable(c, TRUE);
    gtk_tree_view_column_set_expand(c, expand);
    return c;
}

static void build(void)
{
    GtkWidget *vbox, *top, *paned, *pl_view, *bottom, *texts, *controls, *search_btn;
    GtkWidget *btn, *vlabel;
    GtkTreeSelection *sel;
    char *icon;

    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), APP_NAME);
    gtk_window_set_default_size(GTK_WINDOW(window), 800, 552);
    icon = g_build_filename("/usr/share/pixmaps", "psionnet-spotify.png", NULL);
    gtk_window_set_icon_from_file(GTK_WINDOW(window), icon, NULL);
    g_free(icon);
    g_signal_connect(window, "destroy", G_CALLBACK(on_quit), NULL);

    vbox = gtk_vbox_new(FALSE, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 6);
    gtk_container_add(GTK_CONTAINER(window), vbox);

    /* search bar */
    top = gtk_hbox_new(FALSE, 6);
    search_entry = gtk_entry_new();
    g_signal_connect(search_entry, "activate", G_CALLBACK(on_search), NULL);
    search_type = gtk_combo_box_new_text();
    gtk_combo_box_append_text(GTK_COMBO_BOX(search_type), "Songs");
    gtk_combo_box_append_text(GTK_COMBO_BOX(search_type), "Albums");
    gtk_combo_box_append_text(GTK_COMBO_BOX(search_type), "Playlists");
    gtk_combo_box_set_active(GTK_COMBO_BOX(search_type), 0);
    search_btn = gtk_button_new_with_label("Search");
    g_signal_connect(search_btn, "clicked", G_CALLBACK(on_search), NULL);
    server_button = gtk_button_new_with_label("PsionNet");
    g_signal_connect(server_button, "clicked", G_CALLBACK(on_server), NULL);
    gtk_box_pack_start(GTK_BOX(top), search_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(top), search_type, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), search_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), server_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), top, FALSE, FALSE, 0);

    /* playlists | tracks */
    paned = gtk_hpaned_new();
    playlists = gtk_list_store_new(P_COLS, G_TYPE_STRING, G_TYPE_STRING);
    pl_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(playlists));
    gtk_tree_view_append_column(GTK_TREE_VIEW(pl_view),
                                text_column("Your library", P_NAME, 180, TRUE));
    sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(pl_view));
    g_signal_connect(sel, "changed", G_CALLBACK(on_playlist_selected), NULL);
    g_object_set_data(G_OBJECT(window), "playlist-view", pl_view);
    gtk_paned_pack1(GTK_PANED(paned), scrolled(pl_view), FALSE, TRUE);

    tracks = gtk_list_store_new(T_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    list_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(tracks));
    gtk_tree_view_set_rules_hint(GTK_TREE_VIEW(list_view), TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(list_view), text_column("Title", T_TITLE, 230, TRUE));
    gtk_tree_view_append_column(GTK_TREE_VIEW(list_view), text_column("Artist", T_ARTIST, 150, FALSE));
    gtk_tree_view_append_column(GTK_TREE_VIEW(list_view), text_column("Album", T_ALBUM, 140, FALSE));
    gtk_tree_view_append_column(GTK_TREE_VIEW(list_view), text_column("Time", T_TIME, 48, FALSE));
    g_signal_connect(list_view, "row-activated", G_CALLBACK(on_track_activated), NULL);
    gtk_paned_pack2(GTK_PANED(paned), scrolled(list_view), TRUE, TRUE);
    gtk_paned_set_position(GTK_PANED(paned), 190);
    gtk_box_pack_start(GTK_BOX(vbox), paned, TRUE, TRUE, 0);

    /* now playing */
    bottom = gtk_hbox_new(FALSE, 10);
    art_image = gtk_image_new();
    gtk_widget_set_size_request(art_image, 64, 64);
    gtk_box_pack_start(GTK_BOX(bottom), art_image, FALSE, FALSE, 0);
    texts = gtk_vbox_new(FALSE, 2);
    title_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title_label), "<b>Nothing playing</b>");
    sub_label = gtk_label_new("");
    time_label = gtk_label_new("");
    where_label = gtk_label_new("");
    gtk_misc_set_alignment(GTK_MISC(title_label), 0, 0.5);
    gtk_misc_set_alignment(GTK_MISC(sub_label), 0, 0.5);
    gtk_misc_set_alignment(GTK_MISC(time_label), 0, 0.5);
    gtk_misc_set_alignment(GTK_MISC(where_label), 0, 0.5);
    gtk_box_pack_start(GTK_BOX(texts), title_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(texts), sub_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(texts), time_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(texts), where_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bottom), texts, TRUE, TRUE, 0);

    controls = gtk_hbox_new(FALSE, 4);
    pix_play = gdk_pixbuf_new_from_xpm_data(play_xpm);
    pix_pause = gdk_pixbuf_new_from_xpm_data(pause_xpm);
    btn = icon_button(prev_xpm, NULL, G_CALLBACK(on_prev));
    gtk_box_pack_start(GTK_BOX(controls), btn, FALSE, FALSE, 0);
    play_button = icon_button(play_xpm, &play_image, G_CALLBACK(on_play));
    gtk_box_pack_start(GTK_BOX(controls), play_button, FALSE, FALSE, 0);
    btn = icon_button(next_xpm, NULL, G_CALLBACK(on_next));
    gtk_box_pack_start(GTK_BOX(controls), btn, FALSE, FALSE, 0);
    vlabel = gtk_label_new("Volume");
    volume_scale = gtk_hscale_new_with_range(0, 100, 5);
    gtk_scale_set_draw_value(GTK_SCALE(volume_scale), FALSE);
    gtk_widget_set_size_request(volume_scale, 110, -1);
    g_signal_connect(volume_scale, "value-changed", G_CALLBACK(on_volume), NULL);
    gtk_box_pack_start(GTK_BOX(controls), vlabel, FALSE, FALSE, 6);
    gtk_box_pack_start(GTK_BOX(controls), volume_scale, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(bottom), controls, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), bottom, FALSE, FALSE, 0);

    status_label = gtk_label_new("");
    gtk_misc_set_alignment(GTK_MISC(status_label), 0, 0.5);
    gtk_box_pack_start(GTK_BOX(vbox), status_label, FALSE, FALSE, 0);

    gtk_widget_show_all(window);
    gtk_widget_grab_focus(search_entry);
}

int main(int argc, char **argv)
{
    g_thread_init(NULL);
    gtk_init(&argc, &argv);
    signal(SIGPIPE, SIG_IGN);

    jobs = g_async_queue_new();
    g_thread_create(worker, NULL, FALSE, NULL);
    g_thread_create(worker, NULL, FALSE, NULL);    /* so a slow search never blocks status */

    build();
    load_config();
    if (argc > 1 && argv[1][0]) {                   /* psionnet-spotify 192.168.1.4[:8080] */
        char *v = g_strdup(argv[1]), *colon = strrchr(v, ':');
        if (colon) {
            *colon = '\0';
            server_port = atoi(colon + 1);
        }
        g_strlcpy(server_host, v, sizeof server_host);
        g_free(v);
    }
    if (server_host[0]) {
        set_status("Connecting to PsionNet\xe2\x80\xa6", FALSE);
        hello();
    } else {
        set_status("Looking for PsionNet on the network\xe2\x80\xa6", FALSE);
        submit(J_DISCOVER, NULL, NULL);
    }
    g_timeout_add(POLL_MS, poll_status, NULL);
    gtk_main();
    stop_player();
    return 0;
}
