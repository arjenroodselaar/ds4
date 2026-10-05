#define DS4_AGENT_TEST
#define DS4_AGENT_TEST_NO_MAIN
#include "../ds4_agent.c"
#include <sys/resource.h>
#if defined(__APPLE__) || defined(__linux__)
#include <sys/xattr.h>
#endif

static const char *test_output_dir;

static void test_fixture(const char *name, const char *data, size_t len) {
    if (!test_output_dir) return;
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", test_output_dir, name);
    FILE *fp = fopen(path, "wb");
    AGENT_TEST_ASSERT(fp != NULL);
    if (!fp) return;
    AGENT_TEST_ASSERT(fwrite(data, 1, len, fp) == len);
    AGENT_TEST_ASSERT(fclose(fp) == 0);
}

static void test_tool_arg(agent_tool_call *call, const char *name, const char *value) {
    agent_tool_call_add_arg(call, name, value, strlen(value), true, "</arg_value>");
}

static int test_write_file(const char *path, const char *data, size_t len,
                           char *err, size_t errlen) {
    return agent_replace_file(path, data, len, NULL, 0, err, errlen);
}

static void test_atomic_file_tools(void) {
    char dir[] = "/tmp/ds4-agent-files-XXXXXX";
    AGENT_TEST_ASSERT(mkdtemp(dir) != NULL);
    char path[PATH_MAX], linkpath[PATH_MAX], err[256];
    snprintf(path, sizeof(path), "%s/file", dir);
    snprintf(linkpath, sizeof(linkpath), "%s/link", dir);
    char original[4096];
    memset(original, 'x', sizeof(original));
    AGENT_TEST_ASSERT(test_write_file(path, original, sizeof(original), err, sizeof(err)) == 0);
    AGENT_TEST_ASSERT(chmod(path, 0751) == 0);
#ifdef __APPLE__
    AGENT_TEST_ASSERT(setxattr(path, "com.ds4.agent-test", "keep", 4, 0, 0) == 0);
#elif defined(__linux__)
    AGENT_TEST_ASSERT(setxattr(path, "user.ds4-agent-test", "keep", 4, 0) == 0);
#endif

    pid_t child = fork();
    AGENT_TEST_ASSERT(child >= 0);
    if (child == 0) {
        signal(SIGXFSZ, SIG_IGN);
        struct rlimit limit = {64, 64};
        if (setrlimit(RLIMIT_FSIZE, &limit)) _exit(2);
        int rc = agent_replace_file(path, original, sizeof(original),
                                     original, sizeof(original), err, sizeof(err));
        _exit(rc == -1 ? 0 : 3);
    }
    int status = 0;
    if (child > 0) waitpid(child, &status, 0);
    AGENT_TEST_ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    char *data = NULL;
    size_t len = 0;
    AGENT_TEST_ASSERT(agent_read_file_bytes(path, &data, &len, err, sizeof(err)) == 0);
    AGENT_TEST_ASSERT(len == sizeof(original) && !memcmp(data, original, len));
    free(data);
    AGENT_TEST_ASSERT(agent_replace_file(path, "bad", 3, "stale", 5, err, sizeof(err)) == -1);
    AGENT_TEST_ASSERT(strstr(err, "changed") != NULL);

    AGENT_TEST_ASSERT(symlink("file", linkpath) == 0);
    AGENT_TEST_ASSERT(test_write_file(linkpath, "new", 3, err, sizeof(err)) == 0);
    struct stat st;
    AGENT_TEST_ASSERT(lstat(linkpath, &st) == 0 && S_ISLNK(st.st_mode));
    AGENT_TEST_ASSERT(stat(path, &st) == 0 && (st.st_mode & 0777) == 0751);
    AGENT_TEST_ASSERT(st.st_uid == getuid());
#ifdef __APPLE__
    char attribute[16];
    AGENT_TEST_ASSERT(getxattr(path, "com.ds4.agent-test", attribute, sizeof(attribute), 0, 0) == 4);
    AGENT_TEST_ASSERT(!memcmp(attribute, "keep", 4));
#elif defined(__linux__)
    char attribute[16];
    AGENT_TEST_ASSERT(getxattr(path, "user.ds4-agent-test", attribute, sizeof(attribute)) == 4);
    AGENT_TEST_ASSERT(!memcmp(attribute, "keep", 4));
#endif
    AGENT_TEST_ASSERT(agent_read_file_bytes(path, &data, &len, err, sizeof(err)) == 0);
    AGENT_TEST_ASSERT(len == 3 && !memcmp(data, "new", 3));
    free(data);
    unlink(linkpath);
    AGENT_TEST_ASSERT(link(path, linkpath) == 0);
    AGENT_TEST_ASSERT(test_write_file(path, "bad", 3, err, sizeof(err)) == -1);
    AGENT_TEST_ASSERT(strstr(err, "hard-linked") != NULL);
    unlink(linkpath);
    unlink(path);
    /* Failed replacements must not leave temporary files behind. */
    AGENT_TEST_ASSERT(rmdir(dir) == 0);

    const char *match = NULL;
    size_t match_len = 0;
    bool anchored = true;
    AGENT_TEST_ASSERT(agent_edit_find_old_span("literal [upto] text", 19,
                         "[upto]", false, &match, &match_len, &anchored, err, sizeof(err)));
    AGENT_TEST_ASSERT(!anchored && match_len == 6 && !memcmp(match, "[upto]", 6));
}

static void test_streaming_file_tools(void) {
    char path[] = "/tmp/ds4-agent-read-XXXXXX";
    int fd = mkstemp(path);
    AGENT_TEST_ASSERT(fd >= 0);
    FILE *fp = fdopen(fd, "wb");
    /* Larger than the old whole-file cap, with matches at both ends. */
    fputs("needle first\r\n", fp);
    for (int i = 0; i < 1024 * 1024; i++) fputs("0123456789abcdef\n", fp);
    fputs("needle last\r", fp);
    fclose(fp);
    agent_worker w = {0};
    char *text = agent_read_range(&w, path, 1, 1, false, false, true);
    AGENT_TEST_ASSERT(strstr(text, "needle first") && w.more_valid);
    AGENT_TEST_ASSERT(w.more_next_line == 2 && w.more_byte_offset == 14);
    free(text);
    agent_tool_call more = {0};
    test_tool_arg(&more, "count", "1");
    text = agent_tool_more(&w, &more);
    AGENT_TEST_ASSERT(strstr(text, "2 0123456789abcdef"));
    free(text);
    agent_tool_call_free(&more);

    agent_tool_call call = {0};
    test_tool_arg(&call, "path", path);
    test_tool_arg(&call, "query", "needle");
    char linkpath[PATH_MAX];
    snprintf(linkpath, sizeof(linkpath), "%s-link", path);
    AGENT_TEST_ASSERT(symlink(path, linkpath) == 0);
    agent_tool_call linked = {0};
    test_tool_arg(&linked, "path", linkpath);
    test_tool_arg(&linked, "query", "needle");
    text = agent_tool_search(&w, &linked);
    AGENT_TEST_ASSERT(strstr(text, "2 matches") && strstr(text, "needle last"));
    free(text);
    agent_tool_call_free(&linked);
    unlink(linkpath);
    text = agent_tool_search(&w, &call);
    AGENT_TEST_ASSERT(strstr(text, "2 matches") && strstr(text, "needle last"));
    free(text);
    test_tool_arg(&call, "mode", "regexp");
    text = agent_tool_search(&w, &call);
    AGENT_TEST_ASSERT(strstr(text, "Tool error:"));
    free(text);
    agent_tool_call_free(&call);

    fp = fopen(path, "wb");
    for (int i = 0; i < 256 * 1024; i++) fputc('x', fp);
    fclose(fp);
    size_t total = 0;
    text = agent_read_range(&w, path, 1, 1, false, true, true);
    do {
        size_t n = strspn(text, "x");
        total += n;
        AGENT_TEST_ASSERT(n > 0 && n < AGENT_TOOL_MAX_BYTES);
        if (w.more_valid) AGENT_TEST_ASSERT(strstr(text, "Read truncated") != NULL);
        free(text);
        if (!w.more_valid) break;
        text = agent_tool_more(&w, &more);
    } while (total < 512 * 1024);
    AGENT_TEST_ASSERT(total == 256 * 1024 && !w.more_valid);
    text = agent_read_range(&w, path, 1, INT_MAX, true, true, true);
    AGENT_TEST_ASSERT(strstr(text, "Tool error: whole read") && !w.more_valid);
    free(text);
    fp = fopen(path, "wb");
    for (int i = 0; i < 256 * 1024; i++) fputc(0x80, fp);
    fclose(fp);
    text = agent_read_range(&w, path, 1, 1, false, true, true);
    AGENT_TEST_ASSERT(strlen(text) < AGENT_TOOL_MAX_BYTES && w.more_valid);
    free(text);
    unlink(path);
    AGENT_TEST_ASSERT(mkfifo(path, 0600) == 0);
    double started = now_sec();
    text = agent_read_range(&w, path, 1, 1, false, false, true);
    AGENT_TEST_ASSERT(strstr(text, "Tool error:") && now_sec() - started < 0.5);
    free(text);
    unlink(path);
    test_tool_arg(&call, "path", path);
    test_tool_arg(&call, "query", "needle");
    text = agent_tool_search(&w, &call);
    AGENT_TEST_ASSERT(strstr(text, "Tool error:"));
    free(text);
    agent_tool_call_free(&call);

    agent_buf b = {0};
    char *large = xmalloc(200000);
    memset(large, 'q', 200000);
    agent_buf_append(&b, large, 200000);
    text = agent_buf_take(&b);
    AGENT_TEST_ASSERT(strlen(text) == 200000);
    free(text);
    b.limit = 100;
    agent_buf_append(&b, large, 200000);
    text = agent_buf_take(&b);
    AGENT_TEST_ASSERT(strstr(text, "Output truncated") != NULL);
    free(large);
    free(text);
    b.limit = 3;
    agent_buf_puts(&b, "a\xe4\xb8\xad" "b");
    text = agent_buf_take(&b);
    AGENT_TEST_ASSERT(!strncmp(text, "a\n[Output truncated", 19));
    free(text);
}

static void test_shell_spawn(void) {
    agent_worker w = {0};
    pthread_mutex_init(&w.mu, NULL);
    w.wake_fd[0] = w.wake_fd[1] = -1;
    char err[256], cwd[PATH_MAX];
    AGENT_TEST_ASSERT(getcwd(cwd, sizeof(cwd)) != NULL);
    setenv("DS4_TEST_SHELL_ENV", "inherited", 1);
    for (int closed_stdio = 0; closed_stdio <= 1; closed_stdio++) {
        int saved[3];
        if (closed_stdio) {
            fflush(NULL);
            for (int i = 0; i < 3; i++) {
                saved[i] = fcntl(i, F_DUPFD_CLOEXEC, 3);
                AGENT_TEST_ASSERT(saved[i] >= 0);
                if (saved[i] < 0) exit(1);
            }
            for (int i = 0; i < 3; i++) close(i);
        }
        agent_bash_job *job = agent_bash_start(&w,
            "read line || printf 'stdin-eof\\n'; pwd; "
            "printf 'env=%s\\n' \"$DS4_TEST_SHELL_ENV\"; "
            "printf 'stderr-captured\\n' >&2; sleep 0.1; exit 23",
            5, err, sizeof(err));
        bool group_ok = job && getpgid(job->pid) == job->pid;
        double start = now_sec();
        while (job && agent_bash_is_running(job) && now_sec() - start < 6)
            usleep(10000);
        bool finished = false;
        char *obs = job ? agent_bash_observation(job, true, &finished) : NULL;
        if (job) {
            unlink(job->path);
            agent_bash_remove_job(&w, job);
        }
        if (closed_stdio) {
            for (int i = 0; i < 3; i++) {
                if (dup2(saved[i], i) < 0) _exit(1);
                close(saved[i]);
            }
        }
        AGENT_TEST_ASSERT(group_ok && finished && obs);
        if (obs) {
            AGENT_TEST_ASSERT(strstr(obs, "exit_status=23"));
            AGENT_TEST_ASSERT(strstr(obs, "stdin-eof"));
            AGENT_TEST_ASSERT(strstr(obs, cwd));
            AGENT_TEST_ASSERT(strstr(obs, "env=inherited"));
            AGENT_TEST_ASSERT(strstr(obs, "stderr-captured"));
        }
        free(obs);
    }
    unsetenv("DS4_TEST_SHELL_ENV");
    free(w.out);
    pthread_mutex_destroy(&w.mu);
}

static void test_background_jobs(void) {
    agent_worker w = {0};
    pthread_mutex_init(&w.mu, NULL);
    w.wake_fd[0] = w.wake_fd[1] = -1;
    char err[256], marker[] = "/tmp/ds4-agent-deadline-XXXXXX";
    int fd = mkstemp(marker);
    close(fd);
    unlink(marker);
    char cmd[PATH_MAX + 128];
    snprintf(cmd, sizeof(cmd), "sleep 2; printf late > %s", marker);
    agent_bash_job *job = agent_bash_start(&w, cmd, 1, err, sizeof(err));
    AGENT_TEST_ASSERT(job != NULL);
    if (!job) goto done;
    /* Deliberately no status polling: this stands in for model generation. */
    usleep(2400000);
    AGENT_TEST_ASSERT(access(marker, F_OK) != 0);
    bool finished = false;
    char *obs = agent_bash_observation(job, true, &finished);
    AGENT_TEST_ASSERT(finished && strstr(obs, "timed_out=1"));
    free(obs);
    unlink(job->path);
    agent_bash_remove_job(&w, job);

    job = agent_bash_start(&w, "(sleep 0.1; printf descendant-output) &", 5, err, sizeof(err));
    AGENT_TEST_ASSERT(job != NULL);
    if (!job) goto done;
    usleep(350000);
    obs = agent_bash_observation(job, true, &finished);
    AGENT_TEST_ASSERT(finished && strstr(obs, "descendant-output"));
    free(obs);
    unlink(job->path);
    agent_bash_remove_job(&w, job);

    job = agent_bash_start(&w, "head -c 2097152 /dev/zero | tr '\\000' x", 5, err, sizeof(err));
    AGENT_TEST_ASSERT(job != NULL);
    if (!job) goto done;
    usleep(900000);
    obs = agent_bash_observation(job, true, &finished);
    AGENT_TEST_ASSERT(finished && strstr(obs, "exit_status=0"));
    AGENT_TEST_ASSERT(job->bytes == 2097152);
    free(obs);
    obs = agent_bash_observation(job, true, &finished);
    AGENT_TEST_ASSERT(strlen(obs) < AGENT_BASH_TAIL_BYTES + 2048);
    free(obs);
    unlink(job->path);
    agent_bash_remove_job(&w, job);

    job = agent_bash_start(&w, "sleep 0.3; printf completed", 5, err, sizeof(err));
    AGENT_TEST_ASSERT(job != NULL);
    if (!job) goto done;
    char output_path[PATH_MAX];
    snprintf(output_path, sizeof(output_path), "%s", job->path);
    agent_tool_call call = {.name = xstrdup("bash_status")};
    char id[32];
    snprintf(id, sizeof(id), "%d", job->id);
    test_tool_arg(&call, "job", id);
    test_tool_arg(&call, "refresh_sec", "1");
    double start = now_sec();
    obs = agent_execute_tool_call(&w, &call);
    AGENT_TEST_ASSERT(now_sec() - start >= 0.2);
    AGENT_TEST_ASSERT(strstr(obs, "status=done") && strstr(obs, "completed"));
    AGENT_TEST_ASSERT(w.bash_jobs == NULL);
    free(obs);
    agent_tool_call_free(&call);
    unlink(output_path);

    /* Two monitors must make progress together, and stopping one must neither
     * block shutdown nor kill the other job's process group. */
    job = agent_bash_start(&w, "sleep 30", 60, err, sizeof(err));
    agent_bash_job *other = agent_bash_start(&w, "sleep 0.2; printf independent", 5, err, sizeof(err));
    AGENT_TEST_ASSERT(job && other);
    if (!job || !other) goto done;
    start = now_sec();
    agent_bash_signal(job, SIGKILL);
    unlink(job->path);
    agent_bash_remove_job(&w, job);
    AGENT_TEST_ASSERT(now_sec() - start < 2);
    while (agent_bash_is_running(other) && now_sec() - start < 3) usleep(10000);
    obs = agent_bash_observation(other, true, &finished);
    AGENT_TEST_ASSERT(finished && strstr(obs, "exit_status=0") && strstr(obs, "independent"));
    free(obs);
    unlink(other->path);
    agent_bash_remove_job(&w, other);
done:
    agent_bash_jobs_free(&w);
    unlink(marker);
    free(w.out);
    pthread_mutex_destroy(&w.mu);
}

/* --- sandbox subprocess: predicates and snapshots for the reader thread --- */

static bool test_sandbox_saw_stderr(void *ctx) {
    pthread_mutex_lock(&g_sandbox->mu);
    bool ok = g_sandbox->err_len > 0 &&
              strstr(g_sandbox->err, (const char *)ctx) != NULL;
    pthread_mutex_unlock(&g_sandbox->mu);
    return ok;
}

static bool test_sandbox_is_dead(void *ctx) {
    (void)ctx;
    return g_sandbox && g_sandbox->dead;
}

/* A sandbox that cannot be talked to any more, whether it died or broke the
 * protocol. */
static bool test_sandbox_unusable(void *ctx) {
    (void)ctx;
    return g_sandbox && (g_sandbox->dead || g_sandbox->fault[0]);
}

static bool test_sandbox_wait(bool (*done)(void *), void *ctx, double timeout_sec) {
    double start = now_sec();
    while (now_sec() - start < timeout_sec) {
        if (done(ctx)) return true;
        usleep(5000);
    }
    return done(ctx);
}

static char *test_sandbox_snapshot_out(void) {
    pthread_mutex_lock(&g_sandbox->mu);
    char *copy = xstrdup(g_sandbox->out ? g_sandbox->out : "");
    pthread_mutex_unlock(&g_sandbox->mu);
    return copy;
}

static void test_sandbox_lifecycle(void) {
    char err[512] = {0};

    /* Every later step reads the handle, so a start that fails must end the
     * test instead of falling through and dereferencing a NULL sandbox. */
#define SB_START(cmd, trace)                                    \
    do {                                                        \
        if (!agent_sandbox_start((cmd), (trace), err, sizeof(err))) { \
            fprintf(stderr, "sandbox start failed: %s\n", err); \
            AGENT_TEST_ASSERT(!"sandbox start");                \
            return;                                             \
        }                                                       \
    } while (0)

    /* A command that cannot run must fail here, before the model is loaded,
     * and must not leave a handle behind. */
    AGENT_TEST_ASSERT(!agent_sandbox_start("ds4-no-such-command-xyz", NULL,
                                           err, sizeof(err)));
    AGENT_TEST_ASSERT(err[0] && strstr(err, "127"));
    AGENT_TEST_ASSERT(g_sandbox == NULL);

    /* Anything the sandbox prints to stdout outside a frame breaks the protocol,
     * and a plain `cat` is exactly that: what comes back is read as a header and
     * is not a byte count. */
    SB_START("cat", NULL);
    /* Ignoring SIGPIPE is what turns a write to a dead sandbox into an error the
     * caller can report instead of a signal that kills the agent. */
    AGENT_TEST_ASSERT(signal(SIGPIPE, SIG_IGN) == SIG_IGN);
    write_all(g_sandbox->in_fd, "hello sandbox\n", 14);
    char why[512] = {0};
    AGENT_TEST_ASSERT(test_sandbox_wait(test_sandbox_unusable, NULL, 5));
    AGENT_TEST_ASSERT(agent_sandbox_failed(why, sizeof(why)));
    AGENT_TEST_ASSERT(strstr(why, "byte count"));
    agent_sandbox_stop();
    AGENT_TEST_ASSERT(g_sandbox == NULL);

    /* The environment is deliberately small but usable: the mode marker plus
     * PATH so the command can find things.  The probe goes to stderr, because
     * stdout belongs to the protocol, and the trailing exec keeps the sandbox
     * alive past the startup grace window. */
    SB_START("printf 'sb=[%s] path=[%s]\\n' \"$DS4_SANDBOX\" \"${PATH:-unset}\" >&2; exec cat",
             NULL);
    AGENT_TEST_ASSERT(test_sandbox_wait(test_sandbox_saw_stderr, (void *)"sb=[1]", 5));
    AGENT_TEST_ASSERT(!test_sandbox_saw_stderr((void *)"path=[unset]"));
    char *out = test_sandbox_snapshot_out();
    AGENT_TEST_ASSERT(!strcmp(out, ""));
    free(out);
    agent_sandbox_stop();

    /* stderr never reaches the response channel, but it is kept as a tail and
     * reported when the sandbox goes away. */
    SB_START("printf 'boom-on-stderr\\n' >&2; exec cat", NULL);
    AGENT_TEST_ASSERT(test_sandbox_wait(test_sandbox_saw_stderr,
                                       (void *)"boom-on-stderr", 5));
    out = test_sandbox_snapshot_out();
    AGENT_TEST_ASSERT(strstr(out, "boom-on-stderr") == NULL);
    free(out);
    kill(g_sandbox->pid, SIGKILL);
    AGENT_TEST_ASSERT(test_sandbox_wait(test_sandbox_is_dead, NULL, 5));
    AGENT_TEST_ASSERT(agent_sandbox_failed(why, sizeof(why)));
    AGENT_TEST_ASSERT(strstr(why, "signal 9"));
    AGENT_TEST_ASSERT(strstr(why, "boom-on-stderr"));
    /* The agent must survive a write to the dead sandbox and get an errno it can
     * report, rather than dying on SIGPIPE with the reason unreached. */
    errno = 0;
    AGENT_TEST_ASSERT(write(g_sandbox->in_fd, "x", 1) < 0 && errno == EPIPE);
    agent_sandbox_stop();
    AGENT_TEST_ASSERT(g_sandbox == NULL);

    /* A sandbox that ignores stdin EOF still goes down: the escalation from
     * SIGTERM to SIGKILL bounds the wait. */
    SB_START("sleep 30", NULL);
    double start = now_sec();
    agent_sandbox_stop();
    AGENT_TEST_ASSERT(now_sec() - start < 4.0);
    AGENT_TEST_ASSERT(g_sandbox == NULL);

    /* With --trace the diagnostics land in a sibling file instead of the
     * conversation. */
    char dir[] = "/tmp/ds4-agent-sandbox-XXXXXX";
    AGENT_TEST_ASSERT(mkdtemp(dir) != NULL);
    char trace[PATH_MAX], log[PATH_MAX];
    snprintf(trace, sizeof(trace), "%s/trace", dir);
    snprintf(log, sizeof(log), "%s/trace.sandbox.log", dir);
    SB_START("printf 'to-trace-log\\n' >&2; exec cat", trace);
    AGENT_TEST_ASSERT(test_sandbox_wait(test_sandbox_saw_stderr,
                                       (void *)"to-trace-log", 5));
    agent_sandbox_stop();
    FILE *fp = fopen(log, "r");
    AGENT_TEST_ASSERT(fp != NULL);
    if (fp) {
        char buf[256] = {0};
        AGENT_TEST_ASSERT(fread(buf, 1, sizeof(buf) - 1, fp) > 0);
        fclose(fp);
        AGENT_TEST_ASSERT(strstr(buf, "to-trace-log"));
    }
    unlink(log);
    unlink(trace);
    rmdir(dir);
#undef SB_START

    /* An empty command is a usage error, not a child that dies instantly. */
    pid_t child = fork();
    AGENT_TEST_ASSERT(child >= 0);
    if (child == 0) {
        char *opts[] = {"ds4-agent", "--sandbox", "   "};
        parse_options(3, opts);
        _exit(0);
    }
    int status = 0;
    if (child > 0) waitpid(child, &status, 0);
    AGENT_TEST_ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 2);

    child = fork();
    AGENT_TEST_ASSERT(child >= 0);
    if (child == 0) {
        char *opts[] = {"ds4-agent", "--sandbox", "cat", "--sandbox", "cat"};
        parse_options(5, opts);
        _exit(0);
    }
    if (child > 0) waitpid(child, &status, 0);
    AGENT_TEST_ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 2);

    char *opts[] = {"ds4-agent", "--sandbox", "cat", "--non-interactive"};
    agent_config cfg = parse_options(4, opts);
    AGENT_TEST_ASSERT(cfg.sandbox_cmd && !strcmp(cfg.sandbox_cmd, "cat"));
}

/* --- framed requests and responses -------------------------------------- */

/* A sandbox that answers every request without understanding it: it reads the
 * header, then exactly that many payload bytes, and replies with the payload's
 * byte count.  It also sends a notice frame and a response for an id nobody asked
 * about, neither of which may complete a request.  /bin/sh on macOS is bash 3.2,
 * where ${#v} counts characters, so these frames stay ASCII. */
static const char *SANDBOX_ECHO =
    "while IFS= read -r n; do "
    "p=$(dd bs=1 count=\"$n\" 2>/dev/null); "
    "i=$(printf %s \"$p\" | sed -n 's/^{\"id\":\\([0-9]*\\).*/\\1/p'); "
    "a=$(printf %s \"$p\" | wc -c | tr -d ' '); "
    "z=$(printf '{\"id\":0,\"type\":\"log\",\"text\":\"notice-%s\"}' \"$i\"); "
    "s=$(printf '{\"id\":999,\"ok\":true,\"result\":\"stale\"}'); "
    "r=$(printf '{\"id\":%s,\"ok\":true,\"result\":\"len-%s\"}' \"$i\" \"$a\"); "
    "printf '%s\\n%s%s\\n%s%s\\n%s' \"${#z}\" \"$z\" \"${#s}\" \"$s\" \"${#r}\" \"$r\"; "
    "done";

/* The first answer is one byte over the response ceiling, every later one is
 * normal: the oversized frame must cost that one request and nothing else. */
static const char *SANDBOX_BIG =
    "c=0; while IFS= read -r n; do p=$(dd bs=1 count=\"$n\" 2>/dev/null); "
    "i=$(printf %s \"$p\" | sed -n 's/^{\"id\":\\([0-9]*\\).*/\\1/p'); c=$((c+1)); "
    "if [ \"$c\" = 1 ]; then printf '4194305\\n'; yes x | head -c 4194305; "
    "else r=$(printf '{\"id\":%s,\"ok\":true,\"result\":\"second\"}' \"$i\"); "
    "printf '%s\\n%s' \"${#r}\" \"$r\"; fi; done";

static void test_sandbox_protocol(void) {
    char dir[64], trace[96], log[96], err[256], why[512];
    char *text = NULL;
    snprintf(dir, sizeof(dir), "/tmp/ds4sbp.%d", (int)getpid());
    snprintf(trace, sizeof(trace), "%s/model.jsonl", dir);
    snprintf(log, sizeof(log), "%s.sandbox.log", trace);
    unlink(log);
    unlink(trace);
    rmdir(dir);
    AGENT_TEST_ASSERT(mkdir(dir, 0755) == 0);

    AGENT_TEST_ASSERT(agent_sandbox_start(SANDBOX_ECHO, trace, err, sizeof(err)));
    AGENT_TEST_ASSERT(agent_sandbox_request("read", "{\"path\":\"a\"}", NULL, &text));
    AGENT_TEST_ASSERT(text && !strncmp(text, "len-", 4));
    long len1 = strtoll(text + 4, NULL, 10);
    free(text);
    text = NULL;
    AGENT_TEST_ASSERT(len1 > 0);

    /* The same call with args that are 13 bytes longer reads 13 bytes more: the
     * payload arrives whole and nothing of it leaks into the next frame. */
    AGENT_TEST_ASSERT(agent_sandbox_request("read", "{\"path\":\"a\",\"extra\":\"bb\"}",
                                            NULL, &text));
    AGENT_TEST_ASSERT(text && !strncmp(text, "len-", 4));
    long len2 = strtoll(text + 4, NULL, 10);
    free(text);
    text = NULL;
    AGENT_TEST_ASSERT(len2 == len1 + 13);
    /* Stopping joins the reader and closes the trace log. */
    agent_sandbox_stop();

    /* The notices travelled on the same stream as the answers, were written to
     * the trace sibling and not to the caller, and their ids show that requests
     * are numbered from one. */
    FILE *fp = fopen(log, "r");
    AGENT_TEST_ASSERT(fp != NULL);
    if (fp) {
        char buf[512] = {0};
        size_t got = fread(buf, 1, sizeof(buf) - 1, fp);
        fclose(fp);
        AGENT_TEST_ASSERT(got > 0);
        AGENT_TEST_ASSERT(strstr(buf, "sandbox log: notice-1"));
        AGENT_TEST_ASSERT(strstr(buf, "sandbox log: notice-2"));
    }
    fp = fopen(trace, "r");
    if (fp) {
        char buf[512] = {0};
        size_t got = fread(buf, 1, sizeof(buf) - 1, fp);
        fclose(fp);
        /* The model trace stays what it was: no sandbox chatter. */
        AGENT_TEST_ASSERT(!got || strstr(buf, "notice-") == NULL);
    }

    AGENT_TEST_ASSERT(agent_sandbox_start(SANDBOX_BIG, NULL, err, sizeof(err)));
    AGENT_TEST_ASSERT(!agent_sandbox_request("bash", "{\"command\":\"ls\"}",
                                             NULL, &text));
    AGENT_TEST_ASSERT(text && strstr(text, "too large"));
    free(text);
    text = NULL;
    AGENT_TEST_ASSERT(agent_sandbox_request("bash", "{\"command\":\"ls\"}",
                                            NULL, &text));
    AGENT_TEST_ASSERT(text && !strcmp(text, "second"));
    free(text);
    text = NULL;
    AGENT_TEST_ASSERT(!agent_sandbox_failed(why, sizeof(why)));
    agent_sandbox_stop();

    /* A frame that is not a response object ends the session: there is no way to
     * find the next frame, and guessing would invent a tool result. */
    AGENT_TEST_ASSERT(agent_sandbox_start("printf '2\\n{}'; exec cat", NULL,
                                          err, sizeof(err)));
    AGENT_TEST_ASSERT(test_sandbox_wait(test_sandbox_unusable, NULL, 5));
    AGENT_TEST_ASSERT(agent_sandbox_failed(why, sizeof(why)));
    AGENT_TEST_ASSERT(strstr(why, "id"));
    AGENT_TEST_ASSERT(!agent_sandbox_request("read", "{}", NULL, &text));
    free(text);
    text = NULL;
    agent_sandbox_stop();

    /* A request over the ceiling is refused before anything is written, and the
     * abandoned request leaves the session usable. */
    AGENT_TEST_ASSERT(agent_sandbox_start(SANDBOX_ECHO, NULL, err, sizeof(err)));
    size_t want = AGENT_SANDBOX_REQ_MAX + 1024;
    char *args = xmalloc(want + 1);
    memcpy(args, "{\"k\":\"", 6);
    memset(args + 6, 'x', want - 9);
    memcpy(args + want - 3, "\"}", 3);
    args[want] = '\0';
    AGENT_TEST_ASSERT(!agent_sandbox_request("write", args, NULL, &text));
    free(args);
    AGENT_TEST_ASSERT(text && strstr(text, "too large"));
    free(text);
    text = NULL;
    AGENT_TEST_ASSERT(agent_sandbox_request("read", "{}", NULL, &text));
    AGENT_TEST_ASSERT(text && !strncmp(text, "len-", 4));
    free(text);
    text = NULL;
    AGENT_TEST_ASSERT(!agent_sandbox_failed(why, sizeof(why)));
    agent_sandbox_stop();

    unlink(log);
    unlink(trace);
    rmdir(dir);
}

/* The reference helper in ../ds4-sandbox-helper is a different language and a
 * different runtime answering the same frames.  Only agreeing on the bytes makes
 * that worth having, so when the binary is around, drive it with the agent's own
 * client and check that real tools answer through real framing:
 * DS4_SANDBOX_HELPER=../ds4-sandbox-helper/target/release/ds4-sandbox-helper.
 * It is opt-in because building it needs cargo, which the C build does not assume. */
static void test_sandbox_helper(void) {
    const char *cmd = getenv("DS4_SANDBOX_HELPER");
    if (!cmd || !*cmd) {
        puts("ds4-agent tests: DS4_SANDBOX_HELPER unset, helper interop skipped");
        return;
    }

    char dir[64], path[128], args[512], err[256], why[512];
    char *text = NULL;
    snprintf(dir, sizeof(dir), "/tmp/ds4sbh.%d", (int)getpid());
    snprintf(path, sizeof(path), "%s/main.c", dir);
    unlink(path);
    rmdir(dir);
    AGENT_TEST_ASSERT(mkdir(dir, 0755) == 0);

    AGENT_TEST_ASSERT(agent_sandbox_start(cmd, NULL, err, sizeof(err)));

    snprintf(args, sizeof(args),
             "{\"path\":\"%s\",\"content\":\"int main(void) { return 0; }\\n\"}", path);
    AGENT_TEST_ASSERT(agent_sandbox_request("write", args, NULL, &text));
    AGENT_TEST_ASSERT(text && strstr(text, "Wrote 29 bytes"));
    free(text);
    text = NULL;

    snprintf(args, sizeof(args), "{\"path\":\"%s\"}", path);
    AGENT_TEST_ASSERT(agent_sandbox_request("read", args, NULL, &text));
    AGENT_TEST_ASSERT(text && strstr(text, "1 int main(void) { return 0; }"));
    free(text);
    text = NULL;

    snprintf(args, sizeof(args),
             "{\"path\":\"%s\",\"old\":\"return 0;\",\"new\":\"return 3;\"}", path);
    AGENT_TEST_ASSERT(agent_sandbox_request("edit", args, NULL, &text));
    AGENT_TEST_ASSERT(text && strstr(text, "Touched old lines 1-1"));
    free(text);
    text = NULL;

    snprintf(args, sizeof(args), "{\"path\":\"%s\"}", dir);
    AGENT_TEST_ASSERT(agent_sandbox_request("list", args, NULL, &text));
    AGENT_TEST_ASSERT(text && strstr(text, "main.c"));
    free(text);
    text = NULL;

    snprintf(args, sizeof(args), "{\"query\":\"return 3\",\"path\":\"%s\"}", dir);
    AGENT_TEST_ASSERT(agent_sandbox_request("search", args, NULL, &text));
    AGENT_TEST_ASSERT(text && strstr(text, "return 3;"));
    free(text);
    text = NULL;

    AGENT_TEST_ASSERT(agent_sandbox_request("bash",
                                            "{\"command\":\"printf 'a\\\\nb\\\\n'\"}",
                                            NULL, &text));
    AGENT_TEST_ASSERT(text && strstr(text, "exit_status=0") && strstr(text, "<output>"));
    AGENT_TEST_ASSERT(text && strstr(text, "a\nb"));
    free(text);
    text = NULL;

    AGENT_TEST_ASSERT(!agent_sandbox_failed(why, sizeof(why)));
    agent_sandbox_stop();

    unlink(path);
    rmdir(dir);
}

/* --- routing tool calls to the sandbox ---------------------------------- */

/* Answers a request with its own payload, so a test can read back the exact
 * frame the agent sent, and refuses "write" with ok:false so the shape of a
 * sandbox-reported failure can be checked too. */
static const char *SANDBOX_REPLAY =
    "while IFS= read -r n; do "
    "p=$(dd bs=1 count=\"$n\" 2>/dev/null); "
    "i=$(printf %s \"$p\" | sed -n 's/^{\"id\":\\([0-9]*\\).*/\\1/p'); "
    "e=$(printf %s \"$p\" | sed 's/\\\\/\\\\\\\\/g; s/\"/\\\\\"/g'); "
    "case \"$p\" in *'\"tool\":\"write\"'*) "
    "r=$(printf '{\"id\":%s,\"ok\":false,\"error\":\"read-only path\"}' \"$i\");; "
    "*) r=$(printf '{\"id\":%s,\"ok\":true,\"result\":\"%s\"}' \"$i\" \"$e\");; "
    "esac; printf '%s\\n%s' \"${#r}\" \"$r\"; done";

/* Answers every request with 200 KiB of text: legal under the response ceiling
 * and over the agent's own tool byte limit. */
static const char *SANDBOX_VOLUBILE =
    "while IFS= read -r n; do "
    "p=$(dd bs=1 count=\"$n\" 2>/dev/null); "
    "i=$(printf %s \"$p\" | sed -n 's/^{\"id\":\\([0-9]*\\).*/\\1/p'); "
    "y=$(awk 'BEGIN{s=\"\";for(i=0;i<200000;i++)s=s \"y\";print s}'); "
    "r=$(printf '{\"id\":%s,\"ok\":true,\"result\":\"%s\"}' \"$i\" \"$y\"); "
    "printf '%s\\n%s' \"${#r}\" \"$r\"; done";

static char *test_observation_text(agent_tool_observation *obs) {
    size_t total = 1;
    for (size_t i = 0; i < obs->part_count; i++) total += obs->parts[i].len;
    char *joined = xmalloc(total);
    size_t off = 0;
    for (size_t i = 0; i < obs->part_count; i++) {
        if (obs->parts[i].text) {
            memcpy(joined + off, obs->parts[i].text, obs->parts[i].len);
            off += obs->parts[i].len;
        }
    }
    joined[off] = '\0';
    return joined;
}

static void test_sandbox_routing(void) {
    /* The args object is what the sandbox is promised: every value a string,
     * because a parsed call keeps every parameter as text. */
    agent_tool_call call = {0};
    call.name = xstrdup("bash");
    const char *cmd = "printf 'hi\n' > /tmp/x";
    agent_tool_call_add_arg(&call, "command", cmd, strlen(cmd), false, NULL);
    agent_tool_call_add_arg(&call, "timeout_sec", "30", 2, false, NULL);
    agent_tool_call_add_arg(&call, "path", "a\"b", 3, false, NULL);
    agent_tool_call_add_arg(&call, "path", "ignored", 7, false, NULL);
    char *args_json = agent_sandbox_args_json(&call);
    AGENT_TEST_ASSERT(!strcmp(args_json,
        "{\"command\":\"printf 'hi\\n' > /tmp/x\",\"timeout_sec\":\"30\","
        "\"path\":\"a\\\"b\"}"));
    free(args_json);
    agent_tool_call_free(&call);

    /* A call with no parameters is still an object, not an absent field. */
    agent_tool_call bare = {0};
    bare.name = xstrdup("list");
    args_json = agent_sandbox_args_json(&bare);
    AGENT_TEST_ASSERT(!strcmp(args_json, "{}"));
    free(args_json);

    AGENT_TEST_ASSERT(agent_sandbox_tool_is_routed("bash_status"));
    AGENT_TEST_ASSERT(agent_sandbox_tool_is_routed("search"));
    AGENT_TEST_ASSERT(!agent_sandbox_tool_is_routed("google_search"));
    AGENT_TEST_ASSERT(!agent_sandbox_tool_is_routed("view_image"));
    AGENT_TEST_ASSERT(!agent_sandbox_tool_is_routed("frobnicate"));
    AGENT_TEST_ASSERT(agent_sandbox_tool_is_blocked("visit_page"));
    AGENT_TEST_ASSERT(!agent_sandbox_tool_is_blocked("list"));

    char err[256] = {0};
    AGENT_TEST_ASSERT(agent_sandbox_start(SANDBOX_REPLAY, NULL, err, sizeof(err)));
    agent_worker w = {0};
    pthread_mutex_init(&w.mu, NULL);
    w.wake_fd[0] = w.wake_fd[1] = -1;

    agent_tool_call read = {0};
    read.name = xstrdup("read");
    agent_tool_call_add_arg(&read, "path", "/x", 2, false, NULL);
    agent_tool_call_add_arg(&read, "max_lines", "40", 2, false, NULL);
    char *res = agent_execute_tool_call(&w, &read);
    /* The exact frame the sandbox is promised: an id, the tool's name, the size a
     * bare read should return for this model, and args whose every value is a
     * string.  A worker with no context size to work from gets the largest default
     * rather than the smallest. */
    AGENT_TEST_ASSERT(res && !strcmp(res,
        "{\"id\":1,\"tool\":\"read\",\"limits\":{\"read_lines\":500},"
        "\"args\":{\"path\":\"/x\",\"max_lines\":\"40\"}}"));
    free(res);

    res = agent_execute_tool_call(&w, &read);
    AGENT_TEST_ASSERT(res && !strncmp(res, "{\"id\":2,", 8));
    free(res);

    agent_tool_call_free(&read);

    /* The tools the sandbox cannot serve are answered where they are refused, and
     * they cost no request: view_image never even reaches the dispatch. */
    agent_tool_call blocked[3] = {
        { (char *)"google_search", NULL, 0, 0 },
        { (char *)"visit_page", NULL, 0, 0 },
        { (char *)"view_image", NULL, 0, 0 },
    };
    agent_tool_calls calls = { .v = blocked, .len = 3, .cap = 3 };
    agent_tool_observation obs = agent_execute_tool_observation(&w, &calls);
    char *joined = test_observation_text(&obs);
    AGENT_TEST_ASSERT(strstr(joined, "Tool result 1 (google_search):"));
    AGENT_TEST_ASSERT(strstr(joined,
                            "google_search is not available in sandbox mode"));
    AGENT_TEST_ASSERT(strstr(joined,
                            "visit_page is not available in sandbox mode"));
    AGENT_TEST_ASSERT(strstr(joined, "Tool result 3 (view_image):"));
    AGENT_TEST_ASSERT(strstr(joined,
                            "view_image is not available in sandbox mode"));
    free(joined);
    agent_tool_observation_free(&obs);

    /* An unknown name is unknown to the sandbox too, so it is reported without a
     * request rather than letting the sandbox invent an answer. */
    agent_tool_call bogus = {0};
    bogus.name = xstrdup("frobnicate");
    res = agent_execute_tool_call(&w, &bogus);
    AGENT_TEST_ASSERT(res && strstr(res, "Tool error: unknown tool: frobnicate"));
    free(res);
    AGENT_TEST_ASSERT(strstr(w.out, "[tool:frobnicate] unknown tool"));
    agent_tool_call_free(&bogus);

    /* Neither the refusals above nor the unknown tool consumed an id. */
    agent_tool_call again = {0};
    again.name = xstrdup("read");
    agent_tool_call_add_arg(&again, "path", "/y", 2, false, NULL);
    res = agent_execute_tool_call(&w, &again);
    AGENT_TEST_ASSERT(res && !strncmp(res, "{\"id\":3,", 8));
    free(res);
    agent_tool_call_free(&again);

    /* A sandbox that says no is reported as a failed tool, the way a local tool
     * failure is. */
    agent_tool_call wr = {0};
    wr.name = xstrdup("write");
    agent_tool_call_add_arg(&wr, "path", "/y", 2, false, NULL);
    res = agent_execute_tool_call(&w, &wr);
    AGENT_TEST_ASSERT(res && !strcmp(res, "Tool error: read-only path"));
    free(res);
    agent_tool_call_free(&wr);

    /* A small model gets a small read.  The sandbox cannot see how full this
     * session's context is, so the size a bare read should return travels with the
     * request instead of being guessed at the other end.  It comes last because it
     * costs an id of its own. */
    agent_config tiny = {0};
    tiny.gen.ctx_size = 8192;
    agent_worker small = {0};
    pthread_mutex_init(&small.mu, NULL);
    small.wake_fd[0] = small.wake_fd[1] = -1;
    small.cfg = &tiny;
    agent_tool_call small_read = {0};
    small_read.name = xstrdup("read");
    agent_tool_call_add_arg(&small_read, "path", "/x", 2, false, NULL);
    res = agent_execute_tool_call(&small, &small_read);
    AGENT_TEST_ASSERT(res && strstr(res, "\"limits\":{\"read_lines\":120}"));
    free(res);
    agent_tool_call_free(&small_read);

    agent_sandbox_stop();

    /* An answer over the tool byte limit is cut down here: a sandbox that
     * over-serves does not get to put a bigger observation in front of the model
     * than the agent would have written itself. */
    AGENT_TEST_ASSERT(agent_sandbox_start(SANDBOX_VOLUBILE, NULL, err,
                                          sizeof(err)));
    agent_tool_call big = {0};
    big.name = xstrdup("read");
    agent_tool_call_add_arg(&big, "path", "/z", 2, false, NULL);
    res = agent_execute_tool_call(&w, &big);
    AGENT_TEST_ASSERT(res && strlen(res) < 200000 && strlen(res) > AGENT_TOOL_MAX_BYTES);
    AGENT_TEST_ASSERT(res && res[0] == 'y');
    AGENT_TEST_ASSERT(res && strstr(res, "Output truncated at the tool byte limit"));
    free(res);
    agent_tool_call_free(&big);
    agent_sandbox_stop();
    free(w.out);
}

static void test_completion(const char *text, linenoiseCompletions *completions) {
    (void)text;
    linenoiseAddCompletion(completions, "example");
}

static void test_fragmented_terminal_input(void) {
    int input[2];
    AGENT_TEST_ASSERT(pipe(input) == 0);
    fcntl(input[0], F_SETFL, O_NONBLOCK);
    FILE *sink = tmpfile();
    AGENT_TEST_ASSERT(sink != NULL);
    if (!sink) { close(input[0]); close(input[1]); return; }
    setenv("LINENOISE_ASSUME_TTY", "1", 1);
    struct linenoiseState l = {0};
    char buffer[1024] = "";
    l.ifd = input[0]; l.ofd = fileno(sink);
    l.buf = buffer; l.buflen = sizeof(buffer) - 1;
    l.cols = 80; l.prompt = "";
    const char *samples[] = {"\xc3\xa9", "\xe4\xb8\xad", "\xf0\x9f\x98\x80"};
    for (size_t s = 0; s < sizeof(samples)/sizeof(samples[0]); s++) {
        const char *sample = samples[s];
        for (size_t split = 1; split < strlen(sample); split++) {
            linenoiseEditClear(&l);
            for (size_t i = 0; i < split; i++)
                AGENT_TEST_ASSERT(linenoiseEditFeedByte(&l, sample[i]) == linenoiseEditMore);
            AGENT_TEST_ASSERT(l.len == 0);
            AGENT_TEST_ASSERT(linenoiseEditFeed(&l) == linenoiseEditMore);
            AGENT_TEST_ASSERT(l.len == 0);
            for (size_t i = split; i < strlen(sample); i++)
                AGENT_TEST_ASSERT(linenoiseEditFeedByte(&l, sample[i]) == linenoiseEditMore);
            AGENT_TEST_ASSERT(!strcmp(l.buf, sample));
        }
    }
    linenoiseEditClear(&l);
    const char sequence[] = "ab\x1b[DZ\x1b[3~\x1b[H!";
    for (size_t i = 0; i < sizeof(sequence) - 1; i++) {
        AGENT_TEST_ASSERT(linenoiseEditFeedByte(&l, sequence[i]) == linenoiseEditMore);
        AGENT_TEST_ASSERT(linenoiseEditFeed(&l) == linenoiseEditMore);
    }
    AGENT_TEST_ASSERT(!strcmp(l.buf, "!aZ"));
    linenoiseEditClear(&l);
    const char paste[] = "\x1b[200~one\r\n\xe4\xb8\xad\nthree\x1b[201~";
    for (size_t i = 0; i < sizeof(paste) - 1; i++) {
        AGENT_TEST_ASSERT(linenoiseEditFeedByte(&l, paste[i]) == linenoiseEditMore);
        AGENT_TEST_ASSERT(linenoiseEditFeed(&l) == linenoiseEditMore);
        if (i < sizeof(paste) - 2) AGENT_TEST_ASSERT(l.len == 0);
    }
    AGENT_TEST_ASSERT(!strcmp(l.buf, "one\n\xe4\xb8\xad\nthree"));
    linenoiseEditClear(&l);
    linenoiseEditFeedByte(&l, '\xe4');
    linenoiseEditFeedByte(&l, 'X');
    AGENT_TEST_ASSERT(!strcmp(l.buf, "\xef\xbf\xbdX"));
    linenoiseEditClear(&l);
    const char invalid_paste[] = "\x1b[200~bad\xe4\x1b[201~";
    for (size_t i = 0; i < sizeof(invalid_paste) - 1; i++)
        AGENT_TEST_ASSERT(linenoiseEditFeedByte(&l, invalid_paste[i]) == linenoiseEditMore);
    AGENT_TEST_ASSERT(l.len == 0 && !l.paste_active);
    linenoiseSetCompletionCallback(test_completion);
    linenoiseEditFeedByte(&l, 'e');
    linenoiseEditFeedByte(&l, '\t');
    AGENT_TEST_ASSERT(l.in_completion);
    linenoiseEditFeedByte(&l, '\x1b');
    AGENT_TEST_ASSERT(!l.in_completion && !strcmp(l.buf, "e"));
    linenoiseEditFeedByte(&l, '[');
    linenoiseEditFeedByte(&l, 'D');
    AGENT_TEST_ASSERT(l.pos == 0);
    linenoiseEditClear(&l);
    linenoiseEditFeedByte(&l, '\t');
    const char unicode[] = "\xe4\xb8\xad";
    for (size_t i = 0; i < sizeof(unicode) - 1; i++) linenoiseEditFeedByte(&l, unicode[i]);
    AGENT_TEST_ASSERT(!l.in_completion && !strcmp(l.buf, "example\xe4\xb8\xad"));
    linenoiseEditClear(&l);
    l.buflen = 3;
    linenoiseEditFeedByte(&l, 'e');
    linenoiseEditFeedByte(&l, '\t');
    linenoiseEditFeedByte(&l, ' ');
    AGENT_TEST_ASSERT(!l.in_completion && !strcmp(l.buf, "e") && l.len == 1);
    l.buflen = sizeof(buffer) - 1;
    linenoiseSetCompletionCallback(NULL);
    free(l.queued_input);
    free(l.paste_buf);
    close(input[0]); close(input[1]); fclose(sink);
    unsetenv("LINENOISE_ASSUME_TTY");
}

static void test_shell_terminal_controls(void) {
    const char malicious[] = "before\x1b[2Jafter\x1b[H!\x1b]52;c;secret\a"
                             "\x1bPdata\x1b\\\x1b[31mred\x1b[0m\b\n";
    char *safe = agent_terminal_safe_text(malicious, sizeof(malicious) - 1);
    AGENT_TEST_ASSERT(!strcmp(safe, "beforeafter!\x1b[31mred\x1b[0m\\x08\n"));
    free(safe);
    const char c1[] = "\xe4\xb8\xad\xc2\x9b" "2J\x9b" "2J";
    safe = agent_terminal_safe_text(c1, sizeof(c1) - 1);
    AGENT_TEST_ASSERT(!strcmp(safe, "\xe4\xb8\xad\\xc2\\x9b2J\\x9b2J"));
    free(safe);
    for (size_t i = 0; i < sizeof(malicious); i++) {
        safe = agent_terminal_safe_text(malicious, i);
        AGENT_TEST_ASSERT(!strstr(safe, "\x1b[2J") && !strstr(safe, "\x1b]52"));
        free(safe);
    }
}

static void test_markdown_literals(void) {
    const char *input[] = {"Use *.c files.", "The literal is \\*.", "An unmatched `tick",
                          "**bold** and *italic* and `code`.", "``a ` b``", "*unclosed",
                          "* list item\n", "trailing \\", "**unclosed", "`a``", "\\`literal\\`",
                          "> **Hint:** Check `errno`.\n"};
    const char *expected[] = {"Use *.c files.", "The literal is *.", "An unmatched `tick",
                             "bold and italic and code.", "a ` b", "*unclosed",
                             "* list item\n", "trailing \\", "**unclosed", "`a``", "`literal`",
                             "> Hint: Check errno.\n"};
    for (size_t i = 0; i < sizeof(input)/sizeof(input[0]); i++) {
        agent_tail_capture capture = {.cap = 16384};
        agent_token_renderer r = {.capture = &capture, .format_markdown = true};
        for (size_t j = 0; j < strlen(input[i]); j++) renderer_markdown_feed(&r, input[i][j]);
        renderer_markdown_finish(&r);
        renderer_flush_utf8(&r);
        size_t len;
        char *out = agent_tail_capture_take(&capture, &len);
        AGENT_TEST_ASSERT(!strcmp(out, expected[i]));
        if (strcmp(out, expected[i])) fprintf(stderr, "markdown: %s => %s\n", input[i], out);
        free(out);
    }
    agent_tail_capture capture = {.cap = 20000};
    agent_token_renderer r = {.capture = &capture, .format_markdown = true};
    renderer_markdown_feed(&r, '*');
    for (int i = 0; i < 8192; i++) renderer_markdown_feed(&r, 'x');
    renderer_markdown_finish(&r);
    size_t len;
    char *out = agent_tail_capture_take(&capture, &len);
    AGENT_TEST_ASSERT(len == 8193 && out[0] == '*');
    free(out);
}

static char *test_hint_capture(const char *text, size_t split, bool markdown,
                               bool color, agent_tool_syntax syntax, int *calls) {
    agent_tail_capture capture = {.cap = 32768};
    agent_token_renderer renderer = {.capture = &capture, .format_thinking = true,
        .format_markdown = markdown, .use_color = color, .last_output_newline = true};
    agent_dsml_parser parser = {.syntax = syntax, .state = AGENT_DSML_SEARCH};
    agent_stream_renderer stream = {.renderer = &renderer, .parser = &parser, .syntax = syntax};
    size_t n = strlen(text);
    if (split <= n) {
        agent_stream_text(&stream, text, split, false);
        /* A prompt redraw resets terminal colors between generated fragments. */
        if (color && renderer.wrote_visible_output) renderer_write(&renderer, "\x1b[0m", 4);
        agent_stream_text(&stream, text + split, n - split, false);
    } else {
        for (size_t i = 0; i < n; i++) {
            agent_stream_text(&stream, text + i, 1, false);
            if (color && renderer.wrote_visible_output) renderer_write(&renderer, "\x1b[0m", 4);
        }
    }
    agent_stream_text(&stream, NULL, 0, true);
    renderer_finish(&renderer);
    AGENT_TEST_ASSERT(!renderer.md_hint && !renderer.md_hint_prefix_len && !renderer.color_open);
    if (calls) {
        *calls = (int)parser.calls.len;
        AGENT_TEST_ASSERT(parser.calls.len == 1);
        if (parser.calls.len == 1) {
            AGENT_TEST_ASSERT(!strcmp(parser.calls.v[0].name, "bash"));
            AGENT_TEST_ASSERT(parser.calls.v[0].argc == 1);
            if (parser.calls.v[0].argc == 1)
                AGENT_TEST_ASSERT(!strcmp(parser.calls.v[0].args[0].value, "printf HINT_OK"));
        }
    } else AGENT_TEST_ASSERT(parser.calls.len == 0);
    agent_dsml_parser_free(&parser);
    return agent_tail_capture_take(&capture, NULL);
}

static void test_hint_rendering(void) {
    const char *badge = "\x1b[1;97;48;5;23m Hint \x1b[0m";
    const char *sample =
        "The error path is fixed.\n\n"
        "> **Hint:** Save `errno` before **cleanup**; calls can overwrite it.\n"
        "> Keep the original error for reporting.\n"
        "> Unicode stays intact: caf\xc3\xa9, \xe4\xb8\xad.\n"
        "Normal prose resumes here.\n\n"
        "```text\n> **Hint:** This is literal code.\n```\n"
        "Another normal line.\n";
    for (size_t split = 0; split <= strlen(sample) + 1; split++) {
        char *out = test_hint_capture(sample, split, true, true, AGENT_TOOL_SYNTAX_DSML, NULL);
        const char *label = strstr(out, badge);
        AGENT_TEST_ASSERT(label && !strstr(label + strlen(badge), badge));
        AGENT_TEST_ASSERT(strstr(out, "\x1b[1mcleanup") || split > strlen(sample));
        if (split == strlen(sample)) test_fixture("hints.ansi", out, strlen(out));
        if (split > strlen(sample)) test_fixture("hints-fragmented.ansi", out, strlen(out));
        free(out);
    }
    const char *literal[] = {"> quoted text", "> **Hinting:** not a hint",
        "Inline > **Hint:** not an aside", "`> **Hint:** literal`",
        "\\> **Hint:** escaped", "<think>> **Hint:** hidden reasoning</think>Normal."};
    for (size_t i = 0; i < sizeof(literal) / sizeof(literal[0]); i++) {
        char *out = test_hint_capture(literal[i], strlen(literal[i]), true, true,
                                      AGENT_TOOL_SYNTAX_DSML, NULL);
        AGENT_TEST_ASSERT(!strstr(out, badge));
        free(out);
    }
    const char marker[] = "> **Hint:**";
    for (size_t i = 0; i < sizeof(marker) - 1; i++) {
        char partial[sizeof(marker)];
        memcpy(partial, marker, i);
        partial[i] = 0;
        char *out = test_hint_capture(partial, i, true, true, AGENT_TOOL_SYNTAX_GLM, NULL);
        AGENT_TEST_ASSERT(!strstr(out, badge) && !strncmp(out, partial, i));
        free(out);
    }
    char *out = test_hint_capture(sample, strlen(sample), false, false, AGENT_TOOL_SYNTAX_DSML, NULL);
    AGENT_TEST_ASSERT(!strncmp(out, sample, strlen(sample)) && !strchr(out, '\x1b'));
    free(out);
    out = test_hint_capture("> **Hint:** Check `errno`.\n", 0, true, false, AGENT_TOOL_SYNTAX_DSML, NULL);
    AGENT_TEST_ASSERT(!strcmp(out, "> Hint: Check errno.\n\n"));
    free(out);
    const char *tool[] = {
        "> **Hint:** Keep shell checks reproducible.\n"
        "<｜DSML｜tool_calls><｜DSML｜invoke name=\"bash\">"
        "<｜DSML｜parameter name=\"command\" string=\"true\">printf HINT_OK"
        "</｜DSML｜parameter></｜DSML｜invoke></｜DSML｜tool_calls>",
        "> **Hint:** Keep shell checks reproducible.\n"
        "<tool_call>bash<arg_key>command</arg_key><arg_value>printf HINT_OK</arg_value></tool_call>"
    };
    for (int glm = 0; glm < 2; glm++) {
        for (size_t split = 0; split <= strlen(tool[glm]); split++) {
            int calls = 0;
            out = test_hint_capture(tool[glm], split, true, true,
                glm ? AGENT_TOOL_SYNTAX_GLM : AGENT_TOOL_SYNTAX_DSML, &calls);
            AGENT_TEST_ASSERT(calls == 1 && strstr(out, badge));
            if (split == strlen(tool[glm]))
                test_fixture(glm ? "hints-glm-tool.ansi" : "hints-dsml-tool.ansi", out, strlen(out));
            free(out);
        }
    }
}

static void test_unicode_output_and_footer(void) {
    agent_editor ed = {0};
    ed.edit.cols = 80;
    char ascii[78];
    memset(ascii, 'a', sizeof(ascii));
    editor_note_output(&ed, ascii, sizeof(ascii));
    editor_note_output(&ed, "\xe4", 1);
    AGENT_TEST_ASSERT(ed.output_col == 78 && ed.output_utf8_len == 1);
    editor_note_output(&ed, "\xb8\xad", 2);
    AGENT_TEST_ASSERT(ed.output_col == 0 && ed.output_pending_wrap);
    editor_note_output(&ed, "\xcc\x81", 2);
    AGENT_TEST_ASSERT(ed.output_col == 0 && ed.output_pending_wrap);
    editor_note_output(&ed, "x", 1);
    AGENT_TEST_ASSERT(ed.output_col == 1 && !ed.output_pending_wrap);
    editor_note_output(&ed, "\r", 1);
    AGENT_TEST_ASSERT(ed.output_col == 0 && !ed.output_pending_wrap);
    const char family[] = "\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb";
    for (size_t i = 0; i < sizeof(family) - 1; i++) editor_note_output(&ed, family + i, 1);
    AGENT_TEST_ASSERT(ed.output_col == 2);
    int width;
    AGENT_TEST_ASSERT(linenoiseNextGrapheme(family, sizeof(family) - 1, &width) == sizeof(family) - 1 && width == 2);
    const char flag[] = "\xf0\x9f\x87\xae\xf0\x9f\x87\xb9";
    AGENT_TEST_ASSERT(linenoiseNextGrapheme(flag, sizeof(flag) - 1, &width) == sizeof(flag) - 1 && width == 2);
    editor_note_output(&ed, flag, sizeof(flag) - 1);
    AGENT_TEST_ASSERT(ed.output_col == 4);
    editor_note_output(&ed, "\x1b[", 2);
    AGENT_TEST_ASSERT(ed.output_escape == 2);
    editor_note_output(&ed, "31mX", 4);
    AGENT_TEST_ASSERT(ed.output_col == 5 && !ed.output_escape);

    agent_prompt_queue q = {0};
    char queued[181];
    for (int i = 0; i < 60; i++) memcpy(queued + i * 3, "\xe4\xb8\xad", 3);
    queued[180] = 0;
    agent_prompt_queue_push(&q, queued);
    agent_status st = {0};
    char footer[4096];
    build_footer_text(&st, &q, 40, footer, sizeof(footer));
    test_fixture("queue-footer.txt", footer, strlen(footer));
    for (size_t pos = 0; pos < strlen(footer);) {
        uint32_t cp;
        size_t n = linenoiseUtf8Decode(footer + pos, strlen(footer) - pos, &cp);
        AGENT_TEST_ASSERT(n && cp != 0xfffd);
        if (!n) break;
        pos += n;
    }
    agent_prompt_queue_free(&q);
}

static void test_footer_only_updates(void) {
    FILE *sink = tmpfile();
    AGENT_TEST_ASSERT(sink != NULL);
    if (!sink) return;
    int saved = dup(STDOUT_FILENO);
    dup2(fileno(sink), STDOUT_FILENO);
    agent_editor ed = {.active = true, .scroll_region = true, .term_rows = 24,
                       .term_cols = 80, .output_bottom = 22, .prompt_row = 23};
    snprintf(ed.prompt, sizeof(ed.prompt), "ds4-agent> ");
    snprintf(ed.status, sizeof(ed.status), "generation 0");
    char buffer[] = "draft";
    ed.edit = (struct linenoiseState){.ifd = -1, .ofd = STDOUT_FILENO,
        .buf = buffer, .buflen = sizeof(buffer), .len = 5, .pos = 5, .oldpos = 5,
        .prompt = ed.prompt, .plen = strlen(ed.prompt), .cols = 80,
        .oldrows = 1, .oldstatusrows = 1, .oldrpos = 1,
        .screen_cursor_row = 23, .screen_cursor_col = 17};
    linenoiseEditSetStatus(&ed.edit, ed.status, "", "");
    const char initial[] = "\x1b[23;1Hds4-agent> draft\r\ngeneration 0\x1b[23;17H";
    write_all(STDOUT_FILENO, initial, sizeof(initial) - 1);
    editor_set_prompt_status(&ed, ed.prompt, "generation 1");
    off_t first = lseek(STDOUT_FILENO, 0, SEEK_CUR);
    editor_set_prompt_status(&ed, ed.prompt, "generation 2");
    AGENT_TEST_ASSERT(lseek(STDOUT_FILENO, 0, SEEK_CUR) == first && ed.status_dirty);
    ed.last_prompt_redraw_time -= 1;
    editor_set_prompt_status(&ed, ed.prompt, "generation 2");
    AGENT_TEST_ASSERT(!ed.status_dirty);
    editor_set_prompt_status(&ed, ed.prompt, "done");
    editor_flush_prompt_status(&ed, true);
    AGENT_TEST_ASSERT(!ed.status_dirty && !strcmp(buffer, "draft"));
    dup2(saved, STDOUT_FILENO);
    close(saved);
    fseek(sink, 0, SEEK_END);
    size_t len = (size_t)ftell(sink);
    rewind(sink);
    char *text = xmalloc(len + 1);
    AGENT_TEST_ASSERT(fread(text, 1, len, sink) == len);
    text[len] = 0;
    AGENT_TEST_ASSERT(strstr(text, "\x1b[?2026h") && !strstr(text, "\x1b[0K"));
    test_fixture("status.ansi", text, len);
    free(text);
    free(ed.edit.status); free(ed.edit.status_start); free(ed.edit.status_end);
    fclose(sink);
}

static void test_tool_contracts(void) {
    for (int glm = 0; glm < 3; glm++) {
        for (int vision = 0; vision < 2; vision++) {
            char *prompt = glm == 2 ? agent_build_qwen_tools_prompt(false, vision) :
                           glm ? agent_build_glm_tools_prompt(false, vision) :
                                 agent_build_dsml_tools_prompt(false, vision);
            AGENT_TEST_ASSERT((strstr(prompt, "view_image") != NULL) == vision);
            AGENT_TEST_ASSERT(strstr(prompt, "POSIX extended") && strstr(prompt, "128 KiB"));
            AGENT_TEST_ASSERT(strstr(prompt, "&amp;lt;/"));
            char name[64];
            snprintf(name, sizeof(name), "prompt-%s-%d.txt", glm == 2 ? "qwen" : glm ? "glm" : "dsml", vision);
            test_fixture(name, prompt, strlen(prompt));
            free(prompt);
        }
    }
}

/* Model-free real-PTY driver for tests/ds4_agent_terminal_test.py. */
static int test_terminal_driver(void) {
    agent_editor ed = {0};
    linenoiseSetMultiLine(1);
    if (editor_start(&ed, "ds4-agent> ", "ready", NULL)) return 2;
    double start = now_sec(), next = start;
    char *answer = NULL;
    unsigned tick = 0;
    while (now_sec() - start < 10 && !answer) {
        struct pollfd pfd = {.fd = STDIN_FILENO, .events = POLLIN};
        poll(&pfd, 1, 10);
        if (pfd.revents & POLLIN) editor_read_stdin(&ed);
        while (linenoiseEditQueuedInput(&ed.edit)) {
            char *line = linenoiseEditFeed(&ed.edit);
            if (line == linenoiseEditMore) continue;
            if (line) answer = line;
            else answer = xstrdup("<input error>");
            break;
        }
        if (now_sec() >= next) {
            char status[80];
            snprintf(status, sizeof(status), "generation %u", ++tick);
            if (tick % 4 == 0) {
                const char output[] = "model output \xe4\xb8\xad\n";
                editor_write_async(&ed, output, sizeof(output) - 1, "ds4-agent> ", status, false);
            } else editor_set_prompt_status(&ed, "ds4-agent> ", status);
            next = now_sec() + 0.05;
        }
    }
    editor_stop(&ed);
    editor_restore_terminal_layout(&ed);
    if (!answer) return 3;
    printf("\nRESULT:");
    for (size_t i = 0; i < strlen(answer); i++) printf("%02x", (unsigned char)answer[i]);
    puts("");
    free(answer);
    return 0;
}

static void test_observation_error_is_not_context_exhaustion(void) {
    agent_worker worker = {0};
    ds4_tokens_push(&worker.transcript, 42);
    agent_tool_observation observation;
    agent_tool_observation_init(&observation);
    agent_tool_observation_puts(&observation, "image result");
    ds4_vision_embedding invalid = {0};
    agent_tool_observation_add_image(&observation, &invalid);
    char err[160] = {0};
    int count = -1;
    AGENT_TEST_ASSERT(agent_tool_observation_fits(&worker, &observation, 16,
                                                 &count, err, sizeof(err)) == -1);
    AGENT_TEST_ASSERT(strstr(err, "invalid image observation") != NULL);
    AGENT_TEST_ASSERT(count == -1);
    AGENT_TEST_ASSERT(worker.transcript.len == 1 && worker.transcript.v[0] == 42);
    agent_tool_observation_free(&observation);
    ds4_tokens_free(&worker.transcript);
}

static void test_compaction_boundaries(void) {
    agent_dsml_parser parser = {.state = AGENT_DSML_SEARCH};
    agent_stream_renderer stream = {.parser = &parser};
    AGENT_TEST_ASSERT(!agent_stream_compaction_needs_lookahead(&stream));
    stream.pending_len = 1;
    AGENT_TEST_ASSERT(agent_stream_compaction_needs_lookahead(&stream));
    stream.pending_len = 0;
    stream.dsml_start_len = 1;
    AGENT_TEST_ASSERT(agent_stream_compaction_needs_lookahead(&stream));
    stream.dsml_active = true;
    AGENT_TEST_ASSERT(!agent_stream_compaction_needs_lookahead(&stream));
    stream.dsml_active = false;
    parser.state = AGENT_DSML_PARAM_VALUE;
    AGENT_TEST_ASSERT(!agent_stream_compaction_needs_lookahead(&stream));
    int data[1000] = {0};
    ds4_tokens tokens = {.v = data, .len = 1000};
    AGENT_TEST_ASSERT(agent_compact_tail_boundary(&tokens, 1000, 100, 100, 42) == 900);
    data[850] = 42;
    AGENT_TEST_ASSERT(agent_compact_tail_boundary(&tokens, 1000, 100, 100, 42) == 850);
    data[950] = 42;
    AGENT_TEST_ASSERT(agent_compact_tail_boundary(&tokens, 1000, 100, 100, 42) == 950);
    data[850] = data[950] = 0;
    data[799] = 42;
    AGENT_TEST_ASSERT(agent_compact_tail_boundary(&tokens, 1000, 100, 100, 42) == 900);
    AGENT_TEST_ASSERT(agent_compact_tail_boundary(&tokens, 150, 100, 100, 42) == 100);
    AGENT_TEST_ASSERT(agent_compact_tail_boundary(&tokens, 1000, 100, 100, -1) == 900);
    AGENT_TEST_ASSERT(agent_compact_summary_budget(4096) == 512);
    AGENT_TEST_ASSERT(agent_compact_summary_budget(100000) == 4096);
    AGENT_TEST_ASSERT(agent_compact_summary_budget(1024) == 256);
    ds4_vision_span spans[2] = {
        {.token_start = 100, .embedding = {.token_count = 50}},
        {.token_start = 200, .embedding = {.token_count = 30}},
    };
    for (int glm = 0; glm <= 1; glm++) {
        for (int pos = 0; pos < 260; pos++) {
            int expected = pos;
            for (size_t i = 0; i < 2; i++) {
                int start = (int)spans[i].token_start - glm;
                int end = (int)(spans[i].token_start + spans[i].embedding.token_count) + glm;
                if (pos > start && pos < end) expected = start;
            }
            AGENT_TEST_ASSERT(agent_compact_image_boundary(spans, 2, glm, pos) == expected);
        }
    }
    AGENT_TEST_ASSERT(agent_compact_image_boundary(NULL, 0, false, 77) == 77);
}

static int test_v41_thinking(const char *model) {
    ds4_engine_options opt = {.model_path = model, .backend = DS4_BACKEND_METAL,
        .ssd_streaming = true, .ssd_streaming_cache_experts = 512,
        .context_size = 256, .power_percent = 100};
    agent_config cfg = {.gen = {.ctx_size = 256, .think_mode = DS4_THINK_HIGH}};
    agent_worker w = {.cfg = &cfg, .initialized = true,
        .wake_fd = {-1, -1}, .status = {.state = AGENT_WORKER_IDLE}};
    pthread_mutex_init(&w.mu, NULL);
    AGENT_TEST_ASSERT(ds4_engine_open(&w.engine, &opt) == 0);
    if (!w.engine) return 1;
    AGENT_TEST_ASSERT(ds4_session_create(&w.session, w.engine, 256) == 0);
    if (!w.session) { ds4_engine_close(w.engine); return 1; }
    /* Simulate a restored session whose effort differs from the CLI default. */
    agent_numeric_think_prefix(w.engine, DS4_THINK_MAX, &w.transcript);
    ds4_tokens suffix = {0};
    ds4_tokenize_text(w.engine, "System text.\n\n", &suffix);
    ds4_chat_append_message(w.engine, &suffix, "user", "Hello");
    ds4_chat_append_message(w.engine, &suffix, "assistant", "Hello again.");
    const int first_image_offset = w.transcript.len;
    for (int i = 0; i < suffix.len; i++) ds4_tokens_push(&w.transcript, suffix.v[i]);
    w.images = calloc(1, sizeof(*w.images));
    w.image_count = 1;
    w.images[0].token_start = (uint32_t)first_image_offset;
    const int levels[] = {25, 0, 100, 1, 75, 75, 0};
    for (size_t i = 0; i < sizeof(levels) / sizeof(*levels); i++) {
        char err[160] = {0};
        AGENT_TEST_ASSERT(ds4_session_sync(w.session, &w.transcript, err, sizeof(err)) == 0);
        const int before = w.transcript.len;
        w.requested_think = (ds4_think_mode)(DS4_THINK_LEVEL_BASE + levels[i]);
        const bool changed = effective_think_mode(&cfg) != w.requested_think;
        w.think_requested = true;
        AGENT_TEST_ASSERT(!worker_is_idle(&w));
        AGENT_TEST_ASSERT(!worker_submit(&w, "must wait"));
        worker_apply_requested_think(&w);
        AGENT_TEST_ASSERT(worker_is_idle(&w));
        AGENT_TEST_ASSERT(ds4_session_pos(w.session) == (changed ? 0 : before));
        ds4_tokens expected = {0};
        agent_numeric_think_prefix(w.engine, w.requested_think, &expected);
        AGENT_TEST_ASSERT(w.images[0].token_start == (uint32_t)expected.len);
        for (int j = 0; j < suffix.len; j++) ds4_tokens_push(&expected, suffix.v[j]);
        AGENT_TEST_ASSERT(w.transcript.len == expected.len &&
                           ds4_tokens_starts_with(&w.transcript, &expected));
        ds4_tokens_free(&expected);
    }
    cfg.gen.raw_prompt = true;
    w.requested_think = DS4_THINK_MAX;
    worker_apply_requested_think(&w);
    AGENT_TEST_ASSERT(ds4_think_mode_level(cfg.gen.think_mode) == 0);
    AGENT_TEST_ASSERT(strstr(w.out, "requires a V4.1 chat session"));
    cfg.gen.raw_prompt = false;
    while (w.transcript.len < 250) ds4_tokens_push(&w.transcript, ds4_token_eos(w.engine));
    worker_apply_requested_think(&w);
    AGENT_TEST_ASSERT(ds4_think_mode_level(cfg.gen.think_mode) == 0 && w.transcript.len == 250);
    AGENT_TEST_ASSERT(strstr(w.out, "no context room"));
    free(w.out); free(w.images);
    ds4_tokens_free(&suffix); ds4_tokens_free(&w.transcript);
    ds4_session_free(w.session); ds4_engine_close(w.engine);
    pthread_mutex_destroy(&w.mu);
    puts("V4.1 agent thinking levels, restored prefix, cache invalidation: done");
    return agent_test_failures ? 1 : 0;
}

static void test_qwen_tool_syntax(void) {
    const char text[] =
        "<think>Plan.</think>\n<tool_call>\n<function=list>\n"
        "<parameter=path>\n.\n</parameter>\n</function>\n</tool_call>";
    for (size_t split = 0; split < sizeof(text); split++) {
        char *first = xstrndup(text, split);
        const char *chunks[] = {first, text + split};
        agent_dsml_parser p;
        char *out = agent_test_stream_capture(AGENT_TOOL_SYNTAX_QWEN, chunks, 2, &p, NULL);
        AGENT_TEST_ASSERT(p.state == AGENT_DSML_DONE && p.calls.len == 1);
        if (p.calls.len == 1) AGENT_TEST_ASSERT(!strcmp(p.calls.v[0].name, "list"));
        free(first);
        free(out);
        agent_dsml_parser_free(&p);
    }
    AGENT_TEST_ASSERT(agent_syntax_is_xml_tool_call(AGENT_TOOL_SYNTAX_QWEN));
    AGENT_TEST_ASSERT(agent_syntax_is_xml_tool_call(AGENT_TOOL_SYNTAX_GLM));
    AGENT_TEST_ASSERT(!agent_syntax_is_xml_tool_call(AGENT_TOOL_SYNTAX_DSML41));
    AGENT_TEST_ASSERT(!agent_syntax_is_xml_tool_call(AGENT_TOOL_SYNTAX_DSML));
}

static void test_v41_tool_syntax(void) {
    const char text[] =
        "<think>Plan.</think>\n\n<｜DSML｜ calls>\n"
        "<｜DSML｜ invoke name=\"write\">\n"
        "<｜DSML｜ parameter name=\"path\" string=\"true\">a.txt</｜DSML｜ parameter>\n"
        "<｜DSML｜ parameter name=\"content\" string=\"true\">x </think> "
        "&lt;/｜DSML｜ parameter> &amp;lt;/｜DSML｜ parameter></｜DSML｜ parameter>\n"
        "</｜DSML｜ invoke>\n<｜DSML｜ invoke name=\"list\">\n"
        "<｜DSML｜ parameter name=\"path\" string=\"true\">.</｜DSML｜ parameter>\n"
        "</｜DSML｜ invoke>\n</｜DSML｜ calls>";
    const char expected[] = "x </think> </｜DSML｜ parameter> &lt;/｜DSML｜ parameter>";
    for (size_t split = 0; split < sizeof(text); split++) {
        char *first = xstrndup(text, split);
        const char *chunks[] = {first, text + split};
        agent_dsml_parser p;
        char *out = agent_test_stream_capture(AGENT_TOOL_SYNTAX_DSML41, chunks, 2, &p, NULL);
        AGENT_TEST_ASSERT(p.state == AGENT_DSML_DONE && p.calls.len == 2);
        if (p.state != AGENT_DSML_DONE || p.calls.len != 2) {
            fprintf(stderr, "V4.1 split=%zu state=%d calls=%d error=%s raw=%s\n",
                    split, p.state, p.calls.len, p.error, p.raw ? p.raw : "(none)");
            free(first); free(out); agent_dsml_parser_free(&p);
            break;
        }
        if (p.calls.len == 2) {
            AGENT_TEST_ASSERT(!strcmp(p.calls.v[0].name, "write"));
            AGENT_TEST_ASSERT(!strcmp(agent_tool_arg_value(&p.calls.v[0], "path"), "a.txt"));
            AGENT_TEST_ASSERT(!strcmp(agent_tool_arg_value(&p.calls.v[0], "content"), expected));
            AGENT_TEST_ASSERT(!strcmp(p.calls.v[1].name, "list"));
        }
        AGENT_TEST_ASSERT(!strstr(out, "<｜DSML｜ calls>"));
        AGENT_TEST_ASSERT(p.raw && strstr(p.raw, "<｜DSML｜ calls>") == p.raw);
        free(first); free(out); agent_dsml_parser_free(&p);
    }
    const char *inside[] = {"<think><｜DSML｜ calls><｜DSML｜ invoke name=\"list\">"
        "</｜DSML｜ invoke></｜DSML｜ calls></think>Done"};
    agent_dsml_parser p;
    bool early = false;
    char *out = agent_test_stream_capture(AGENT_TOOL_SYNTAX_DSML41, inside, 1, &p, &early);
    AGENT_TEST_ASSERT(early && p.calls.len == 0 && strstr(out, "tool call ignored"));
    free(out); agent_dsml_parser_free(&p);
    for (int upto = 0; upto < 2; upto++) {
        char *old = agent_build_dsml_tools_prompt(upto, false);
        char *prompt = agent_dsml41_tools_prompt(old);
        AGENT_TEST_ASSERT(strstr(prompt, "<｜DSML｜ calls>"));
        AGENT_TEST_ASSERT(strstr(prompt, "&amp;lt;/｜DSML｜ parameter>"));
        AGENT_TEST_ASSERT(!strstr(prompt, "｜DSML｜tool_calls") &&
                           !strstr(prompt, "｜DSML｜invoke") &&
                           !strstr(prompt, "｜DSML｜parameter"));
        free(old); free(prompt);
    }
}

/* Model-backed regression: save an unsynchronizable transcript, then rebuild
 * it in a larger context using the normal stripped-session loader. */
static int test_full_context_save(const char *model) {
    ds4_engine_options opt = {.model_path = model, .backend = default_backend(),
        .context_size = 512, .power_percent = 100};
    agent_config cfg = {.gen = {.ctx_size = 256}, .non_interactive = true};
    agent_worker w = {.cfg = &cfg, .initialized = true, .user_activity = true,
        .wake_fd = {-1, -1}, .status = {.state = AGENT_WORKER_IDLE}};
    pthread_mutex_init(&w.mu, NULL);
    char dir[] = "/tmp/ds4-agent-full-save-XXXXXX";
    AGENT_TEST_ASSERT(mkdtemp(dir) != NULL);
    w.cache_dir = dir;
    w.session_title = xstrdup("Full transcript recovery");
    w.session_created_at = 123456;
    AGENT_TEST_ASSERT(ds4_engine_open(&w.engine, &opt) == 0);
    if (!w.engine) return 1;
    ds4_tokens complete = {0};
    ds4_chat_begin(w.engine, &complete);
    for (int i = 0; i < 60; i++)
        ds4_chat_append_message(w.engine, &complete, "user", "Explain the colors of a rainbow.");
    AGENT_TEST_ASSERT(complete.len > 257);
    for (int length = 256; length <= 257; length++) {
        AGENT_TEST_ASSERT(ds4_session_create(&w.session, w.engine, 256) == 0);
        if (!w.session) break;
        ds4_tokens slice = complete;
        slice.len = length;
        ds4_tokens_copy(&w.transcript, &slice);
        w.session_dirty = true;
        char err[256] = {0}, sha[41];
        int saved_tokens = 0;
        AGENT_TEST_ASSERT(agent_worker_save_session_now(&w, sha, &saved_tokens, err, sizeof(err)));
        AGENT_TEST_ASSERT(saved_tokens == length && !w.session_dirty);
        AGENT_TEST_ASSERT(ds4_session_pos(w.session) == 0);
        char *path = agent_kv_path_for_sha(dir, sha);
        FILE *fp = fopen(path, "rb");
        AGENT_TEST_ASSERT(fp != NULL);
        if (!fp) { free(path); ds4_session_free(w.session); w.session = NULL; break; }
        ds4_kvstore_entry hdr = {0};
        uint32_t text_bytes = 0;
        AGENT_TEST_ASSERT(ds4_kvstore_read_header(fp, &hdr, &text_bytes));
        AGENT_TEST_ASSERT(hdr.payload_bytes == 0 && hdr.tokens == (uint32_t)length);
        char *text = NULL, *title = NULL;
        AGENT_TEST_ASSERT(agent_kv_read_text(fp, text_bytes, &text, err, sizeof(err)));
        AGENT_TEST_ASSERT(agent_kv_read_title_trailer(fp, &hdr, &title, err, sizeof(err)));
        AGENT_TEST_ASSERT(title && !strcmp(title, w.session_title));
        fclose(fp);
        size_t expected_len = 0;
        char *expected = ds4_kvstore_render_tokens_text(w.engine, &w.transcript, &expected_len);
        AGENT_TEST_ASSERT(text && expected && text_bytes == expected_len && !strcmp(text, expected));
        free(expected); free(text); free(title);
        ds4_session_free(w.session);
        w.session = NULL;
        AGENT_TEST_ASSERT(ds4_session_create(&w.session, w.engine, 512) == 0);
        cfg.gen.ctx_size = 512;
        ds4_tokens restored = {0};
        agent_kv_session_meta meta = {0};
        AGENT_TEST_ASSERT(agent_kv_load_path(&w, path, sha, NULL, 0, &restored, &meta, err, sizeof(err)));
        AGENT_TEST_ASSERT(agent_tokens_equal(&w.transcript, &restored));
        AGENT_TEST_ASSERT(meta.created_at == w.session_created_at && meta.title && !strcmp(meta.title, w.session_title));
        ds4_token_score score;
        AGENT_TEST_ASSERT(ds4_session_top_logprobs(w.session, &score, 1) == 1);
        agent_kv_session_meta_free(&meta);
        ds4_tokens_free(&restored);
        ds4_tokens_free(&w.transcript);
        ds4_session_free(w.session); w.session = NULL;
        cfg.gen.ctx_size = 256;
        unlink(path); free(path);
    }
    ds4_tokens_free(&complete);
    ds4_engine_close(w.engine);
    free(w.session_title);
    pthread_mutex_destroy(&w.mu);
    rmdir(dir);
    return agent_test_failures ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--full-context-save")) return test_full_context_save(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "--think-fixture")) return test_v41_thinking(argv[2]);
    if (argc == 2 && !strcmp(argv[1], "--terminal-driver")) return test_terminal_driver();
    if (argc == 3 && !strcmp(argv[1], "--terminal-fixtures")) test_output_dir = argv[2];
    char *options[] = {"ds4-agent", "--model", "qwen.gguf",
                    "--vision", "mmproj.gguf", "--non-interactive", "-p", "test"};
    agent_config cfg = parse_options((int)(sizeof(options) / sizeof(options[0])), options);
    AGENT_TEST_ASSERT(cfg.engine.vision_path && !strcmp(cfg.engine.vision_path, "mmproj.gguf"));
    AGENT_TEST_ASSERT(cfg.engine.model_path && !strcmp(cfg.engine.model_path, "qwen.gguf"));
    ds4_agent_unit_tests_run();
    test_v41_tool_syntax();
    test_qwen_tool_syntax();
    test_compaction_boundaries();
    test_observation_error_is_not_context_exhaustion();
    test_atomic_file_tools();
    test_streaming_file_tools();
    test_shell_spawn();
    test_background_jobs();
    test_sandbox_lifecycle();
    test_sandbox_protocol();
    test_sandbox_helper();
    test_sandbox_routing();
    test_fragmented_terminal_input();
    test_shell_terminal_controls();
    test_markdown_literals();
    test_hint_rendering();
    test_unicode_output_and_footer();
    test_footer_only_updates();
    test_tool_contracts();
    if (agent_test_failures) {
        fprintf(stderr, "ds4-agent tests: %d failure(s)\n",
                agent_test_failures);
        return 1;
    }
    puts("ds4-agent tests: ok");
    return 0;
}
