#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <argp.h>

#include <pulse/pulseaudio.h>

#define clean_errno() (errno == 0 ? "None" : strerror(errno))

#define log_err(M, ...) fprintf(stderr, "[ERROR] (%s:%d: errno: %s) " M "\n", __FILE__, __LINE__, clean_errno(), ##__VA_ARGS__)
#define log_warn(M, ...) fprintf(stderr, "[WARN] (%s:%d: errno: %s) " M "\n", __FILE__, __LINE__, clean_errno(), ##__VA_ARGS__)
#define log_info(M, ...) fprintf(stderr, "[INFO] (%s:%d) " M "\n", __FILE__, __LINE__, ##__VA_ARGS__)

#define check(A, M, ...) if(!(A)) { log_err(M, ##__VA_ARGS__); errno=0; goto error; }

#define sentinel(M, ...)  { log_err(M, ##__VA_ARGS__); errno=0; goto error; }

#define check_mem(A) check((A), "Out of memory.")

typedef enum {
    CONNECTING,
    CONNECTED,
    ERROR,
} state_t;

state_t state;

struct sink_info {
    uint32_t index;
    int mute;
    pa_cvolume volume;
};

struct pmixer_priv {
    pa_mainloop *mainloop;
    pa_mainloop_api *mainloop_api;
    pa_context *context;
};

void state_cb(pa_context *context, void* raw)
{
    switch(pa_context_get_state(context)) {
        case PA_CONTEXT_READY:
            state = CONNECTED;
            break;
        case PA_CONTEXT_FAILED:
            state = ERROR;
            break;
        case PA_CONTEXT_UNCONNECTED:
        case PA_CONTEXT_AUTHORIZING:
        case PA_CONTEXT_SETTING_NAME:
        case PA_CONTEXT_CONNECTING:
        case PA_CONTEXT_TERMINATED:
            break;
        default:
            sentinel("pa_context in unexpected state.");
            break;
    }
error:
    return;
}

void sink_info_cb(pa_context *c, const pa_sink_info *info, int eol, void *raw)
{
    if (eol != 0) return;

    struct sink_info *i = *(struct sink_info **)raw;

    i->index = info->index;
    i->mute = info->mute;
    i->volume = info->volume;
}

void server_info_cb(pa_context *c, const pa_server_info *info, void *raw)
{
    char **name = (char **)raw;
    *name = strdup(info->default_sink_name);
}

void success_cb(pa_context *c, int success, void *raw)
{
}

void iterate(struct pmixer_priv *priv, pa_operation *op)
{
    int retval;

    while (pa_operation_get_state(op) == PA_OPERATION_RUNNING) {
        pa_mainloop_iterate(priv->mainloop, 1, &retval);
    }
}

struct sink_info *get_sink(struct pmixer_priv *priv, const char *name)
{
    struct sink_info *sink_info = malloc(sizeof(struct sink_info));
    check_mem(sink_info);

    pa_operation *op = pa_context_get_sink_info_by_name(priv->context, name, sink_info_cb, &sink_info);
    iterate(priv, op);
    pa_operation_unref(op);

    check(sink_info, "Unable to get info for sink: %s", name);

    return sink_info;

error:
    if (sink_info)
        free(sink_info);
    return NULL;
}

struct sink_info *get_default_sink(struct pmixer_priv *priv)
{
    char *sink_name = NULL;
    struct sink_info *sink = NULL;

    pa_operation *op = pa_context_get_server_info(priv->context, server_info_cb, &sink_name);
    iterate(priv, op);
    pa_operation_unref(op);

    check(sink_name, "Unable to retrive default sink name");
    log_info("Default sink name %s", sink_name);
    sink = get_sink(priv, sink_name);
    check(sink, "Unable to retrive info for default sink named %s", sink_name);
    free(sink_name);
    return sink;

error:
    if (sink_name)
        free(sink_name);
    if (sink)
        free(sink);
    return NULL;
}

void set_volume(struct pmixer_priv *priv, uint32_t index, pa_cvolume *new_volume)
{
    pa_operation *op;
    op = pa_context_set_sink_volume_by_index(priv->context, index, new_volume, success_cb, NULL);
    iterate(priv, op);
    pa_operation_unref(op);
}

void set_mute(struct pmixer_priv * priv, uint32_t index, int mute)
{
    pa_operation *op;
    op = pa_context_set_sink_mute_by_index(priv->context, index, mute, success_cb, NULL);
    iterate(priv, op);
    pa_operation_unref(op);
}

const char *argp_program_version = "pmixer 0.1";
const char *argp_program_bug_address = "phil@dixon.gen.nz";

static char doc[] =
        "pmixer -- Pulse Audio volume control from the shell.";

static char args_doc[] = "<command>";

static struct argp_option options[] = {
    {"verbose", 'v', 0, 0, "Enable detailed output"},
    { 0 }
};

enum commands {
    CMD_NOP,
    CMD_INC,
    CMD_DEC,
    CMD_MUTE
};

struct cmd_map {
    enum commands cmd;
    char *text;
};

struct cmd_map cmd_map[] = {
    {CMD_INC, "inc"},
    {CMD_DEC, "dec"},
    {CMD_MUTE, "mute"},
    { 0 }
};

struct arguments {
    enum commands command;
    int verbose;
};

static error_t parse_opt(int key, char *arg, struct argp_state *state)
{
    struct arguments *arguments = state->input;

    switch(key) {
        case 'v':
            arguments->verbose = 1;
            break;
        case ARGP_KEY_ARG:
            if (state->arg_num >= 1)
                argp_usage(state);
            for(int i = 0; cmd_map[i].cmd != 0; i++) {
                if (strcmp(arg, cmd_map[i].text) == 0) {
                    arguments->command = cmd_map[i].cmd;
                    break;
                }
            }
            break;
        case ARGP_KEY_END:
            if (state->arg_num < 1)
                argp_usage(state);
            break;
        default:
            return ARGP_ERR_UNKNOWN;
    }

    return 0;
}

static struct argp argp = {options, parse_opt, args_doc, doc};

int main(int argc, char *argv[])
{
    struct pmixer_priv priv;
    struct arguments arguments;
    int retval;
    struct sink_info *info = NULL;

    argp_parse(&argp, argc, argv, 0, 0, &arguments);

    check_mem(priv.mainloop = pa_mainloop_new());
    priv.mainloop_api = pa_mainloop_get_api(priv.mainloop);
    check_mem(priv.mainloop_api);
    priv.context = pa_context_new(priv.mainloop_api, "pmixer");
    check_mem(priv.context);
    pa_context_set_state_callback(priv.context, state_cb, NULL);

    state = CONNECTING;
    pa_context_connect(priv.context, NULL, PA_CONTEXT_NOFLAGS, NULL);

    while (state == CONNECTING) {
        pa_mainloop_iterate(priv.mainloop, 1, &retval);
    }
    check(state == CONNECTED, "Can't connect.");

    check(info = get_default_sink(&priv), "Can't get default sink.");
    log_info("got sink %d, volume %u", info->index, pa_cvolume_avg(&info->volume));

    switch (arguments.command) {
        case CMD_MUTE:
            set_mute(&priv, info->index, !info->mute);
            break;
        case CMD_INC:
            set_volume(&priv, info->index, pa_cvolume_inc_clamp(&info->volume, PA_VOLUME_NORM/20, PA_VOLUME_UI_MAX));
            break;
        case CMD_DEC:
            set_volume(&priv, info->index, pa_cvolume_dec(&info->volume, PA_VOLUME_NORM/20));
            break;
    }

    if (state == CONNECTED)
        pa_context_disconnect(priv.context);
    pa_context_unref(priv.context);
    pa_mainloop_free(priv.mainloop);
    return 0;

error:
    if (info)
        free(info);
    if (state == CONNECTED)
        pa_context_disconnect(priv.context);
    if (priv.context)
        pa_context_unref(priv.context);
    if (priv.mainloop)
        pa_mainloop_free(priv.mainloop);
    return -1;
}
