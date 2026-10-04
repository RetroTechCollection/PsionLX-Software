/*
 * psionlx-software -- "Find new software" for PsionLX.
 *
 * Psion's TASKS screen offered "Find new software" and never implemented it.
 * This is it: a catalogue of the programs, games and fonts the PsionLX full
 * image adds to Psion's, installed with Psion's own package manager (ipkg)
 * from the PsionLX-Software folder of the RetroTechCollection archive.
 *
 * The netBook Pro cannot fetch from the archive itself (its TLS is far too
 * old), so the files come through PsionNet on a Mac on the same network,
 * which this program finds by broadcast, as the Spotify app does:
 *
 *   GET http://<psionnet>/lx/software/catalogue.txt     the list shown here
 *   GET http://<psionnet>/lx/software/icons/<name>.png   its icons
 *   ipkg-cl, fed from http://<psionnet>/lx/software/   the packages
 *
 * Installing needs root, for a small helper, /usr/lib/psionlx-software/
 * ipkg-run, that does nothing but run ipkg. PsionLX's root password is
 * empty, so plain "su -c" runs it with no prompt at all (checked on the
 * hardware). Psion's own gpe-su cannot: it waits for su to ask for a
 * password, which with an empty one su never does. If someone has set a
 * root password, plain su fails without a terminal, and gpe-su asks for it.
 *
 * Written for GTK 2.4 / GLib 2.4, as Psion shipped them.
 * GPL-2.0-or-later, as part of PsionNet.
 */

#include <gtk/gtk.h>
#include <glib.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "psionnet-discover.h"

#define APP_TITLE     "Find new software"
#define DEFAULT_PORT  8080
#define HELPER        "/usr/lib/psionlx-software/ipkg-run"
#define LOG_FILE      "/tmp/psionlx-software.log"
#define STATUS_FILE   "/usr/lib/ipkg/status"
#define USER_AGENT    "PsionLX-Software/1.0"
#define MAX_BODY      (1024 * 1024)

/* --- the server ------------------------------------------------------------ */

static char server_host[128] = "";
static int server_port = DEFAULT_PORT;
G_LOCK_DEFINE_STATIC(server);
G_LOCK_DEFINE_STATIC(resolver);

static char *server_file(void)
{
    return g_build_filename(g_get_home_dir(), ".psionnet", "server", NULL);
}

static void set_server(const char *spec)
{
    char *v = g_strstrip(g_strdup(spec)), *colon = strrchr(v, ':');
    int port = DEFAULT_PORT;
    if (colon) {
        *colon = '\0';
        if (atoi(colon + 1) > 0)
            port = atoi(colon + 1);
    }
    G_LOCK(server);
    g_strlcpy(server_host, v, sizeof server_host);
    server_port = port;
    G_UNLOCK(server);
    g_free(v);
}

static void load_server(void)
{
    char *path = server_file(), *text = NULL;
    if (g_file_get_contents(path, &text, NULL, NULL)) {
        set_server(text);
        g_free(text);
    }
    g_free(path);
}

static void save_server(void)
{
    char *dir = g_build_filename(g_get_home_dir(), ".psionnet", NULL), *path = server_file();
    FILE *f;
    mkdir(dir, 0700);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "%s:%d\n", server_host, server_port);
        fclose(f);
    }
    g_free(dir);
    g_free(path);
}

/* --- HTTP/1.0, blocking, on the worker threads ----------------------------- */

static int connect_to(const char *host, int port)
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
    tv.tv_sec = 4;
    tv.tv_usec = 0;
    if (select(fd + 1, NULL, &w, NULL, &tv) <= 0
        || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, flags);
    tv.tv_sec = 30;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    return fd;
}

/* The HTTP status, or -1 if PsionNet could not be reached. */
static int http_get(const char *host, int port, const char *path, GString **body)
{
    char buf[4096], *req, *hdr_end;
    GString *raw = g_string_new(NULL);
    int fd, n, status = -1;

    *body = NULL;
    fd = connect_to(host, port);
    if (fd < 0) {
        g_string_free(raw, TRUE);
        return -1;
    }
    req = g_strdup_printf("GET %s HTTP/1.0\r\nHost: %s:%d\r\nUser-Agent: " USER_AGENT
                          "\r\nConnection: close\r\n\r\n", path, host, port);
    n = write(fd, req, strlen(req));
    g_free(req);
    if (n > 0)
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

/* --- jobs ------------------------------------------------------------------ */

typedef enum { J_DISCOVER, J_CATALOGUE, J_ICON } JobKind;

typedef struct {
    JobKind kind;
    char *path;
    char *tag;          /* the package an icon belongs to */
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
            if (psionnet_discover(h, sizeof h, &p, 3)) {
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
    job->path = path;
    job->tag = tag ? g_strdup(tag) : NULL;
    g_async_queue_push(jobs, job);
}

/* --- the catalogue ---------------------------------------------------------- */

typedef struct {
    char *name, *title, *category, *version, *icon, *exec, *summary, *description;
    long size;
    GdkPixbuf *pix32, *pix48;
} Pkg;

static GPtrArray *catalogue;          /* of Pkg* */
static GHashTable *installed;         /* package -> installed version */

static const char *CATEGORIES[] = { "All", "Games", "Tools", "Sound", "Internet", "Fonts", "System" };
#define N_CATEGORIES 7

static char *unescape(const char *s)
{
    GString *out = g_string_new(NULL);
    for (; *s; s++) {
        if (s[0] == '\\' && s[1] == 'n') {
            g_string_append_c(out, '\n');
            s++;
        } else {
            g_string_append_c(out, *s);
        }
    }
    return g_string_free(out, FALSE);
}

static int count_fields(char **f)          /* g_strv_length is GLib 2.6 */
{
    int n = 0;
    while (f[n])
        n++;
    return n;
}

static void free_pkg(Pkg *p)
{
    g_free(p->name); g_free(p->title); g_free(p->category); g_free(p->version);
    g_free(p->icon); g_free(p->exec); g_free(p->summary); g_free(p->description);
    if (p->pix32) g_object_unref(p->pix32);
    if (p->pix48) g_object_unref(p->pix48);
    g_free(p);
}

/* "PSIONLX-SOFTWARE 1", then: name title category version size icon exec summary description */
static gboolean parse_catalogue(const char *text)
{
    char **lines = g_strsplit(text, "\n", 0);
    int i;
    if (!lines[0] || !g_str_has_prefix(lines[0], "PSIONLX-SOFTWARE")) {
        g_strfreev(lines);
        return FALSE;
    }
    if (catalogue) {
        g_ptr_array_foreach(catalogue, (GFunc) free_pkg, NULL);
        g_ptr_array_free(catalogue, TRUE);
    }
    catalogue = g_ptr_array_new();
    for (i = 1; lines[i]; i++) {
        char **f;
        Pkg *p;
        if (!lines[i][0] || lines[i][0] == '#')
            continue;
        f = g_strsplit(lines[i], "\t", 0);
        if (count_fields(f) >= 9) {
            p = g_new0(Pkg, 1);
            p->name = g_strdup(f[0]);
            p->title = g_strdup(f[1]);
            p->category = g_strdup(f[2]);
            p->version = g_strdup(f[3]);
            p->size = atol(f[4]);
            p->icon = g_strdup(f[5]);
            p->exec = g_strdup(f[6]);
            p->summary = g_strdup(f[7]);
            p->description = unescape(f[8]);
            g_ptr_array_add(catalogue, p);
        }
        g_strfreev(f);
    }
    g_strfreev(lines);
    return TRUE;
}

/* What ipkg has installed, from its status file (readable by everyone). */
static void load_installed(void)
{
    char *text = NULL, **blocks;
    int i;
    if (installed)
        g_hash_table_destroy(installed);
    installed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    if (!g_file_get_contents(STATUS_FILE, &text, NULL, NULL))
        return;
    blocks = g_strsplit(text, "\n\n", 0);
    for (i = 0; blocks[i]; i++) {
        char **l = g_strsplit(blocks[i], "\n", 0), *name = NULL, *version = NULL;
        gboolean ok = FALSE;
        int j;
        for (j = 0; l[j]; j++) {
            if (g_str_has_prefix(l[j], "Package: "))
                name = l[j] + 9;
            else if (g_str_has_prefix(l[j], "Version: "))
                version = l[j] + 9;
            else if (g_str_has_prefix(l[j], "Status: "))
                ok = strstr(l[j], " installed") && !strstr(l[j], "not-installed");
        }
        if (name && ok)
            g_hash_table_insert(installed, g_strdup(name), g_strdup(version ? version : ""));
        g_strfreev(l);
    }
    g_strfreev(blocks);
    g_free(text);
}

typedef enum { ST_NEW, ST_INSTALLED, ST_UPDATE } State;

static State state_of(Pkg *p)
{
    const char *v = installed ? g_hash_table_lookup(installed, p->name) : NULL;
    if (!v)
        return ST_NEW;
    return strcmp(v, p->version) == 0 ? ST_INSTALLED : ST_UPDATE;
}

/* --- the window ------------------------------------------------------------- */

enum { C_PIX, C_TEXT, C_STATE, C_INDEX, C_COLS };

static GtkWidget *window, *status_label, *server_button, *cat_view, *pkg_view;
static GtkWidget *d_image, *d_title, *d_meta, *d_desc, *act_button, *open_button;
static GtkListStore *cat_store, *pkg_store;
static int current_category = 0;
static int selected = -1;             /* index into catalogue */
static gboolean busy = FALSE;
static GPid helper_pid = 0;
static gboolean via_gpe_su = FALSE;    /* second attempt: a root password is set */
static char *pending_action = NULL;
static char *pending_package = NULL;
static guint log_timer = 0;
static gboolean asked = FALSE;
static guint retry_id = 0;

static void set_status(const char *text, gboolean bad)
{
    char *m = g_markup_printf_escaped(bad ? "<span foreground=\"#a01818\">%s</span>" : "%s", text);
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

static char *human_size(long bytes)
{
    if (bytes >= 1024 * 1024)
        return g_strdup_printf("%.1f MB", bytes / 1048576.0);
    return g_strdup_printf("%ld KB", (bytes + 1023) / 1024);
}

static const char *state_text(State s)
{
    return s == ST_INSTALLED ? "Installed" : s == ST_UPDATE ? "Update" : "";
}

static void show_details(void)
{
    Pkg *p;
    char *m, *size;
    State s;

    if (!catalogue || selected < 0 || selected >= (int) catalogue->len) {
        gtk_label_set_text(GTK_LABEL(d_title), "");
        gtk_label_set_text(GTK_LABEL(d_meta), "");
        gtk_label_set_text(GTK_LABEL(d_desc), "Choose something on the list.");
        gtk_image_set_from_pixbuf(GTK_IMAGE(d_image), NULL);
        gtk_widget_set_sensitive(act_button, FALSE);
        gtk_widget_set_sensitive(open_button, FALSE);
        return;
    }
    p = g_ptr_array_index(catalogue, selected);
    s = state_of(p);
    m = g_markup_printf_escaped("<b><big>%s</big></b>", p->title);
    gtk_label_set_markup(GTK_LABEL(d_title), m);
    g_free(m);
    size = human_size(p->size);
    m = g_strdup_printf("%s  \xc2\xb7  version %s  \xc2\xb7  %s%s", p->category, p->version, size,
                        s == ST_INSTALLED ? "  \xc2\xb7  installed" :
                        s == ST_UPDATE ? "  \xc2\xb7  a newer version is available" : "");
    gtk_label_set_text(GTK_LABEL(d_meta), m);
    g_free(m);
    g_free(size);
    gtk_label_set_text(GTK_LABEL(d_desc), p->description);
    gtk_image_set_from_pixbuf(GTK_IMAGE(d_image), p->pix48);
    gtk_button_set_label(GTK_BUTTON(act_button),
                         s == ST_INSTALLED ? "Remove" : s == ST_UPDATE ? "Update" : "Install");
    /* This program removing itself would leave TASKS pointing at nothing. */
    gtk_widget_set_sensitive(act_button, !busy && !(s == ST_INSTALLED
                                                    && !strcmp(p->name, "psionlx-software")));
    gtk_widget_set_sensitive(open_button, !busy && s != ST_NEW && p->exec[0]
                                          && strcmp(p->name, "psionlx-software") != 0);
}

static void fill_list(void)
{
    guint i;
    gtk_list_store_clear(pkg_store);
    if (!catalogue)
        return;
    for (i = 0; i < catalogue->len; i++) {
        Pkg *p = g_ptr_array_index(catalogue, i);
        GtkTreeIter it;
        char *m;
        if (current_category > 0 && strcmp(p->category, CATEGORIES[current_category]) != 0)
            continue;
        m = g_markup_printf_escaped("<b>%s</b>\n<span size=\"small\">%s</span>",
                                    p->title, p->summary);
        gtk_list_store_append(pkg_store, &it);
        gtk_list_store_set(pkg_store, &it, C_PIX, p->pix32, C_TEXT, m,
                           C_STATE, state_text(state_of(p)), C_INDEX, (int) i, -1);
        g_free(m);
    }
}

static void refresh_states(void)
{
    GtkTreeIter it;
    gboolean ok = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(pkg_store), &it);
    load_installed();
    while (ok) {
        int i;
        gtk_tree_model_get(GTK_TREE_MODEL(pkg_store), &it, C_INDEX, &i, -1);
        gtk_list_store_set(pkg_store, &it, C_STATE,
                           state_text(state_of(g_ptr_array_index(catalogue, i))), -1);
        ok = gtk_tree_model_iter_next(GTK_TREE_MODEL(pkg_store), &it);
    }
    show_details();
}

static void set_icon(const char *name, GdkPixbuf *pix)
{
    guint i;
    GtkTreeIter it;
    gboolean ok;
    for (i = 0; catalogue && i < catalogue->len; i++) {
        Pkg *p = g_ptr_array_index(catalogue, i);
        if (strcmp(p->name, name) != 0)
            continue;
        if (p->pix48) g_object_unref(p->pix48);
        if (p->pix32) g_object_unref(p->pix32);
        p->pix48 = gdk_pixbuf_scale_simple(pix, 48, 48, GDK_INTERP_BILINEAR);
        p->pix32 = gdk_pixbuf_scale_simple(pix, 32, 32, GDK_INTERP_BILINEAR);
        ok = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(pkg_store), &it);
        while (ok) {
            int idx;
            gtk_tree_model_get(GTK_TREE_MODEL(pkg_store), &it, C_INDEX, &idx, -1);
            if (idx == (int) i)
                gtk_list_store_set(pkg_store, &it, C_PIX, p->pix32, -1);
            ok = gtk_tree_model_iter_next(GTK_TREE_MODEL(pkg_store), &it);
        }
        if ((int) i == selected)
            show_details();
    }
}

/* --- talking to PsionNet ------------------------------------------------------ */

static void ask_for_server(void);

static gboolean retry_cb(gpointer discover)
{
    retry_id = 0;
    if (GPOINTER_TO_INT(discover))
        submit(J_DISCOVER, NULL, NULL);
    else
        submit(J_CATALOGUE, g_strdup("/lx/software/catalogue.txt"), NULL);
    return FALSE;
}

static void retry_later(gboolean discover, int ms)
{
    if (!retry_id)
        retry_id = g_timeout_add(ms, retry_cb, GINT_TO_POINTER(discover));
}

static void fetch_catalogue(void)
{
    update_server_button();
    set_status("Fetching the catalogue from PsionNet\xe2\x80\xa6", FALSE);
    submit(J_CATALOGUE, g_strdup("/lx/software/catalogue.txt"), NULL);
}

static gboolean job_done(gpointer data)
{
    Job *job = data;
    static int failures = 0;

    switch (job->kind) {
    case J_DISCOVER:
        if (job->status == 200) {
            save_server();
            fetch_catalogue();
        } else if (!server_host[0]) {
            set_status("Could not find PsionNet on the network. Is it running on the Mac, with "
                       "\"netBook Pro (PsionLX, network)\" chosen?", TRUE);
            update_server_button();
            if (!asked) {
                asked = TRUE;
                ask_for_server();
            }
            if (!server_host[0])
                retry_later(TRUE, 10000);
        } else {
            retry_later(FALSE, 5000);
        }
        break;
    case J_CATALOGUE:
        if (job->status == 200 && job->body && parse_catalogue(job->body->str)) {
            guint i;
            failures = 0;
            save_server();
            load_installed();
            fill_list();
            for (i = 0; i < catalogue->len; i++) {
                Pkg *p = g_ptr_array_index(catalogue, i);
                if (p->icon[0])
                    submit(J_ICON, g_strdup_printf("/lx/software/%s", p->icon), p->name);
            }
            set_status("Choose something, then Install. PsionNet fetches it from the "
                       "RetroTechCollection archive.", FALSE);
        } else {
            set_status(job->status < 0 ? "PsionNet is not answering." :
                       job->status == 404 ? "This PsionNet has no software catalogue. Update "
                       "PsionNet, and choose \"netBook Pro (PsionLX, network)\"." :
                       "The catalogue could not be fetched.", TRUE);
            /* Unreachable twice running: the Mac may have a new address. */
            if (job->status < 0 && ++failures % 4 == 2)
                retry_later(TRUE, 1000);
            else
                retry_later(FALSE, 5000);
        }
        break;
    case J_ICON:
        if (job->status == 200 && job->body && job->body->len) {
            GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
            if (gdk_pixbuf_loader_write(loader, (const guchar *) job->body->str,
                                        job->body->len, NULL)
                && gdk_pixbuf_loader_close(loader, NULL)) {
                GdkPixbuf *pix = gdk_pixbuf_loader_get_pixbuf(loader);
                if (pix)
                    set_icon(job->tag, pix);
            } else {
                gdk_pixbuf_loader_close(loader, NULL);
            }
            g_object_unref(loader);
        }
        break;
    }
    if (job->body)
        g_string_free(job->body, TRUE);
    g_free(job->path);
    g_free(job->tag);
    g_free(job);
    return FALSE;
}

/* --- installing: gpe-su runs the helper, which runs ipkg ---------------------- */

static char *last_log_line(gboolean *done, int *rc)
{
    char *text = NULL, **lines, *last = NULL;
    int i;
    *done = FALSE;
    if (!g_file_get_contents(LOG_FILE, &text, NULL, NULL))
        return NULL;
    lines = g_strsplit(text, "\n", 0);
    for (i = 0; lines[i]; i++) {
        if (g_str_has_prefix(lines[i], "PSIONLX-SOFTWARE-DONE ")) {
            *done = TRUE;
            *rc = atoi(lines[i] + 22);
        } else if (lines[i][0]) {
            g_free(last);
            last = g_strdup(lines[i]);
        }
    }
    g_strfreev(lines);
    g_free(text);
    return last;
}

static gboolean log_tick(gpointer unused)
{
    gboolean done;
    int rc;
    char *line = last_log_line(&done, &rc);
    if (line && !done)
        set_status(line, FALSE);
    g_free(line);
    return busy;
}

static void spawn_helper(gboolean ask_password);

static void helper_exited(GPid pid, gint status, gpointer unused)
{
    gboolean done;
    int rc = 1;
    char *line = last_log_line(&done, &rc);

    g_spawn_close_pid(pid);
    helper_pid = 0;
    if (!done && !via_gpe_su) {
        /* Plain su could not run it: root has a password. Ask for it. */
        g_free(line);
        set_status("PsionLX has a root password set: type it in the window that opens.", FALSE);
        spawn_helper(TRUE);
        return;
    }
    busy = FALSE;
    if (log_timer) {
        g_source_remove(log_timer);
        log_timer = 0;
    }
    if (!done)
        set_status("Nothing was changed: the root password was not accepted, or the window "
                   "was closed.", TRUE);
    else if (rc == 0)
        set_status("Done. New programs are in PROGRAMS.", FALSE);
    else {
        char *m = g_strdup_printf("ipkg could not finish: %s", line ? line : "see " LOG_FILE);
        set_status(m, TRUE);
        g_free(m);
    }
    g_free(line);
    refresh_states();
}

static gboolean safe_word(const char *s, const char *extra)
{
    for (; *s; s++)
        if (!g_ascii_isalnum(*s) && !strchr(extra, *s))
            return FALSE;
    return TRUE;
}

/* Runs ipkg-run as root: plain su first (PsionLX's empty root password asks
   nothing), then gpe-su, which asks for a password, if su could not. */
static void spawn_helper(gboolean ask_password)
{
    char *cmd, *argv[4];
    GError *error = NULL;

    unlink(LOG_FILE);
    via_gpe_su = ask_password;
    cmd = g_strdup_printf(HELPER " %s %s:%d %s", pending_action, server_host, server_port,
                          pending_package);
    argv[0] = ask_password ? "gpe-su" : "su";
    argv[1] = "-c";
    argv[2] = cmd;
    argv[3] = NULL;
    /* stdin is /dev/null: if su wants a password it fails at once, not waits */
    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                       NULL, NULL, &helper_pid, &error)) {
        set_status(error->message, TRUE);
        g_error_free(error);
        busy = FALSE;
        show_details();
    } else {
        g_child_watch_add(helper_pid, helper_exited, NULL);
        if (!log_timer)
            log_timer = g_timeout_add(500, log_tick, NULL);
    }
    g_free(cmd);
}

static void run_helper(const char *action, Pkg *p)
{
    if (busy || !server_host[0])
        return;
    if (!safe_word(p->name, ".+-") || !safe_word(server_host, ".-")) {
        set_status("That package name is not one ipkg would accept.", TRUE);
        return;
    }
    g_free(pending_action);
    g_free(pending_package);
    pending_action = g_strdup(action);
    pending_package = g_strdup(p->name);
    busy = TRUE;
    set_status(strcmp(action, "remove") == 0 ? "Removing\xe2\x80\xa6" : "Installing\xe2\x80\xa6",
               FALSE);
    show_details();
    spawn_helper(FALSE);
}

static void on_action(GtkWidget *w, gpointer unused)
{
    Pkg *p;
    if (!catalogue || selected < 0)
        return;
    p = g_ptr_array_index(catalogue, selected);
    run_helper(state_of(p) == ST_INSTALLED ? "remove" : "install", p);
}

static void on_open(GtkWidget *w, gpointer unused)
{
    Pkg *p;
    GError *error = NULL;
    if (!catalogue || selected < 0)
        return;
    p = g_ptr_array_index(catalogue, selected);
    if (p->exec[0] && !g_spawn_command_line_async(p->exec, &error)) {
        set_status(error->message, TRUE);
        g_error_free(error);
    }
}

/* --- selection and the address dialog ----------------------------------------- */

static void on_category(GtkTreeSelection *sel, gpointer unused)
{
    GtkTreeModel *model;
    GtkTreeIter it;
    GtkTreePath *path;
    if (!gtk_tree_selection_get_selected(sel, &model, &it))
        return;
    path = gtk_tree_model_get_path(model, &it);
    current_category = gtk_tree_path_get_indices(path)[0];
    gtk_tree_path_free(path);
    selected = -1;
    fill_list();
    show_details();
}

static void on_package(GtkTreeSelection *sel, gpointer unused)
{
    GtkTreeModel *model;
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(sel, &model, &it))
        return;
    gtk_tree_model_get(model, &it, C_INDEX, &selected, -1);
    show_details();
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
        const char *v = gtk_entry_get_text(GTK_ENTRY(entry));
        if (v[0]) {
            set_server(v);
            fetch_catalogue();
        }
    }
    gtk_widget_destroy(dlg);
}

static void on_server(GtkWidget *w, gpointer unused) { ask_for_server(); }

/* --- building it ---------------------------------------------------------------- */

static GtkWidget *scrolled(GtkWidget *child)
{
    GtkWidget *s = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(s), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(s), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(s), child);
    return s;
}

static void build(void)
{
    GtkWidget *vbox, *top, *title, *paned, *details, *texts, *buttons;
    GtkCellRenderer *r;
    GtkTreeViewColumn *col;
    int i;

    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), APP_TITLE);
    gtk_window_set_default_size(GTK_WINDOW(window), 800, 552);
    gtk_window_set_icon_from_file(GTK_WINDOW(window),
                                  "/usr/share/pixmaps/psionlx-software.png", NULL);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    vbox = gtk_vbox_new(FALSE, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 6);
    gtk_container_add(GTK_CONTAINER(window), vbox);

    top = gtk_hbox_new(FALSE, 6);
    title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title), "<b><big>Find new software</big></b>");
    gtk_misc_set_alignment(GTK_MISC(title), 0, 0.5);
    server_button = gtk_button_new_with_label("PsionNet");
    g_signal_connect(server_button, "clicked", G_CALLBACK(on_server), NULL);
    gtk_box_pack_start(GTK_BOX(top), title, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(top), server_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), top, FALSE, FALSE, 0);

    paned = gtk_hpaned_new();
    cat_store = gtk_list_store_new(1, G_TYPE_STRING);
    for (i = 0; i < N_CATEGORIES; i++) {
        GtkTreeIter it;
        gtk_list_store_append(cat_store, &it);
        gtk_list_store_set(cat_store, &it, 0, CATEGORIES[i], -1);
    }
    cat_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(cat_store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(cat_view), FALSE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(cat_view),
        gtk_tree_view_column_new_with_attributes("", gtk_cell_renderer_text_new(), "text", 0, NULL));
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(cat_view)), "changed",
                     G_CALLBACK(on_category), NULL);
    gtk_paned_pack1(GTK_PANED(paned), scrolled(cat_view), FALSE, TRUE);

    pkg_store = gtk_list_store_new(C_COLS, GDK_TYPE_PIXBUF, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT);
    pkg_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(pkg_store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(pkg_view), FALSE);
    gtk_tree_view_set_rules_hint(GTK_TREE_VIEW(pkg_view), TRUE);
    r = gtk_cell_renderer_pixbuf_new();
    gtk_cell_renderer_set_fixed_size(r, 44, 40);
    gtk_tree_view_append_column(GTK_TREE_VIEW(pkg_view),
        gtk_tree_view_column_new_with_attributes("", r, "pixbuf", C_PIX, NULL));
    col = gtk_tree_view_column_new_with_attributes("", gtk_cell_renderer_text_new(),
                                                   "markup", C_TEXT, NULL);
    gtk_tree_view_column_set_expand(col, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(pkg_view), col);
    r = gtk_cell_renderer_text_new();
    g_object_set(r, "foreground", "#1a7f37", NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(pkg_view),
        gtk_tree_view_column_new_with_attributes("", r, "text", C_STATE, NULL));
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(pkg_view)), "changed",
                     G_CALLBACK(on_package), NULL);
    gtk_paned_pack2(GTK_PANED(paned), scrolled(pkg_view), TRUE, TRUE);
    gtk_paned_set_position(GTK_PANED(paned), 130);
    gtk_box_pack_start(GTK_BOX(vbox), paned, TRUE, TRUE, 0);

    details = gtk_hbox_new(FALSE, 10);
    d_image = gtk_image_new();
    gtk_widget_set_size_request(d_image, 56, 56);
    gtk_box_pack_start(GTK_BOX(details), d_image, FALSE, FALSE, 0);
    texts = gtk_vbox_new(FALSE, 2);
    d_title = gtk_label_new("");
    d_meta = gtk_label_new("");
    d_desc = gtk_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(d_desc), TRUE);
    gtk_widget_set_size_request(d_desc, 520, -1);
    gtk_misc_set_alignment(GTK_MISC(d_title), 0, 0.5);
    gtk_misc_set_alignment(GTK_MISC(d_meta), 0, 0.5);
    gtk_misc_set_alignment(GTK_MISC(d_desc), 0, 0);
    gtk_box_pack_start(GTK_BOX(texts), d_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(texts), d_meta, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(texts), d_desc, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(details), texts, TRUE, TRUE, 0);
    buttons = gtk_vbox_new(FALSE, 6);
    act_button = gtk_button_new_with_label("Install");
    gtk_widget_set_size_request(act_button, 96, 34);
    g_signal_connect(act_button, "clicked", G_CALLBACK(on_action), NULL);
    open_button = gtk_button_new_with_label("Open");
    g_signal_connect(open_button, "clicked", G_CALLBACK(on_open), NULL);
    gtk_box_pack_start(GTK_BOX(buttons), act_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(buttons), open_button, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(details), buttons, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), details, FALSE, FALSE, 0);

    status_label = gtk_label_new("");
    gtk_misc_set_alignment(GTK_MISC(status_label), 0, 0.5);
    gtk_box_pack_start(GTK_BOX(vbox), status_label, FALSE, FALSE, 0);

    gtk_widget_show_all(window);
    show_details();
}

int main(int argc, char **argv)
{
    g_thread_init(NULL);
    gtk_init(&argc, &argv);
    signal(SIGPIPE, SIG_IGN);

    jobs = g_async_queue_new();
    g_thread_create(worker, NULL, FALSE, NULL);
    g_thread_create(worker, NULL, FALSE, NULL);

    build();
    load_installed();
    if (argc > 1 && argv[1][0])            /* psionlx-software 192.168.1.4[:8080] */
        set_server(argv[1]);
    else
        load_server();                     /* written at login by psionnet-connect */
    if (server_host[0]) {
        fetch_catalogue();
    } else {
        set_status("Looking for PsionNet on the network\xe2\x80\xa6", FALSE);
        submit(J_DISCOVER, NULL, NULL);
    }
    gtk_main();
    return 0;
}
