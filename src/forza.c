#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <uv.h>
#include <env.h>
#include <saneopt.h>

#include <forza.h>
#include <forza-private/plugins.h>

static uv_loop_t *loop;
extern char** environ;

static forza_plugin_t plugins[PLUGIN_COUNT];
static char** arguments;

//
// Pipes for child process I/O.
//
static uv_pipe_t child_stdout;
static uv_pipe_t child_stderr;
static uv_pipe_t child_ipc;

static uv_process_t* child = NULL;

static saneopt_t* opt;

void on_process_exit(uv_process_t* process, int exit_status, int term_signal) {
  int i;

  for (i = 0; i < PLUGIN_COUNT; i++) {
    if (plugins[i].process_exit_cb) {
      plugins[i].process_exit_cb(exit_status, term_signal);
    }
  }

  forza_close();
  uv_close((uv_handle_t*) process, NULL);
}

static uv_buf_t forza__on_alloc(uv_handle_t* handle, size_t suggested_size) {
  return uv_buf_init((char*) malloc(suggested_size), suggested_size);
}

void forza__on_read(uv_stream_t* tcp, ssize_t nread, uv_buf_t rdbuf, forza__stdio_type_t type) {
  int i;
  char* data;

  if (nread == -1) {
    //
    // XXX(mmalecki): EOF. We should tell someone.
    //
    return;
  }

  data = malloc((nread + 1) * sizeof(char));
  memcpy(data, rdbuf.base, nread);
  data[nread] = '\0';

  for (i = 0; i < PLUGIN_COUNT; i++) {
    if (type == STDIO_STDOUT && plugins[i].stdout_data_cb) {
      plugins[i].stdout_data_cb(data);
    }
    else if (type == STDIO_STDERR && plugins[i].stderr_data_cb) {
      plugins[i].stderr_data_cb(data);
    }
    else if (type == STDIO_IPC && plugins[i].ipc_data_cb) {
      plugins[i].ipc_data_cb(data);
    }
  }

  free(rdbuf.base);
  free(data);
}

void forza__on_stdout_read(uv_stream_t* tcp, ssize_t nread, uv_buf_t rdbuf) {
  forza__on_read(tcp, nread, rdbuf, STDIO_STDOUT);
}

void forza__on_stderr_read(uv_stream_t* tcp, ssize_t nread, uv_buf_t rdbuf) {
  forza__on_read(tcp, nread, rdbuf, STDIO_STDERR);
}

void forza__on_ipc_read(uv_stream_t* tcp, ssize_t nread, uv_buf_t rdbuf) {
  forza__on_read(tcp, nread, rdbuf, STDIO_IPC);
}

void spawn() {
  int i;
  char** env = NULL;

  env = env_copy(environ, env);

  child = malloc(sizeof(uv_process_t));
  uv_stdio_container_t stdio[4];
  uv_process_options_t options;

  uv_pipe_init(loop, &child_stdout, 0);
  uv_pipe_init(loop, &child_stderr, 0);
  uv_pipe_init(loop, &child_ipc, 0);

  //
  // Setup child's stdio. stdout and stderr are pipes so that we can read
  // child process' output.
  // FD 3 is a pipe used for IPC.
  //
  options.stdio_count = 4;
  stdio[0].flags = UV_INHERIT_FD;
  stdio[0].data.fd = 0;
  stdio[1].flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE;
  stdio[1].data.stream = (uv_stream_t*) &child_stdout;
  stdio[2].flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE;
  stdio[2].data.stream = (uv_stream_t*) &child_stderr;
  stdio[3].flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE;
  stdio[3].data.stream = (uv_stream_t*) &child_ipc;

  options.env = env;
  options.cwd = NULL;
  options.file = arguments[0];
  options.args = arguments;
  options.flags = 0;
  options.stdio = stdio;
  options.exit_cb = on_process_exit;

  for (i = 0; i < PLUGIN_COUNT; i++) {
    if (plugins[i].process_options_cb) {
      plugins[i].process_options_cb(&options);
    }
  }

  if (uv_spawn(loop, child, options)) {
    fprintf(stderr, "uv_spawn: %s\n", uv_err_name(uv_last_error(loop)));
    return;
  }

  for (i = 0; i < PLUGIN_COUNT; i++) {
    if (plugins[i].process_spawned_cb) {
      plugins[i].process_spawned_cb(child, &options);
    }
  }

  uv_read_start(options.stdio[1].data.stream, forza__on_alloc, forza__on_stdout_read);
  uv_read_start(options.stdio[2].data.stream, forza__on_alloc, forza__on_stderr_read);
  uv_read_start(options.stdio[3].data.stream, forza__on_alloc, forza__on_ipc_read);

  // Switch to using `options->env` here instead of `env`, since plugins can
  // override `options->env` using, for example `env_set`, like in the start
  // plugin.
  env_free(options.env);
}

void on_connect(int status) {
  int i;
  if (status) {
    fprintf(stderr, "on_connect: %s\n", uv_strerror(uv_last_error(loop)));
    return;
  }

  for (i = 0; i < PLUGIN_COUNT; i++) {
    plugins[i].saneopt = opt;
    if (_plugin_init_calls[i](&plugins[i]) != 0) {
      fprintf(stderr, "error initializing plugin %i\n", i);
    }
  }

  //
  // We are connected to the monitoring server. If a command was supplied,
  // spawn a child process.
  //
  if (arguments != NULL) {
    spawn();
  }
}

void forza__kill() {
  if (child != NULL) {
    printf("killing child...\n");
    uv_process_kill(child, SIGKILL);
  }
}

void forza__on_sigterm() {
  //
  // If `exit` is called explicitely, `atexit` handler is invoked.
  //
  exit(1);
}

int main(int argc, char *argv[]) {
  char* host;
  char* hostname;
  char* user;
  char* name;
  char* port_str;
  int port;
  int v = 0;

  srand(time(NULL));

  atexit(forza__kill);
  signal(SIGTERM, forza__on_sigterm);

  loop = uv_default_loop();

#ifdef FORZA_VERSION_HASH
  printf("forza %s\n", FORZA_VERSION_HASH);
  // saneopt doesn't allow options without input
  for (v = 0; v < argc; v++) {
    if (strcmp(argv[v], "-v") == 0 || strcmp(argv[v], "--version") == 0) {
      return 0;
    }
  }
#else
  printf("forza\n");
#endif

  opt = saneopt_init(argc - 1, argv + 1);

  saneopt_alias(opt, "host", "h");
  saneopt_alias(opt, "port", "p");

  host = saneopt_get(opt, "host");
  port_str = saneopt_get(opt, "port");
  hostname = saneopt_get(opt, "hostname");
  user = saneopt_get(opt, "app-user");
  name = saneopt_get(opt, "app-name");
  arguments = saneopt_arguments(opt);

  if (host == NULL || port_str == NULL) {
    fprintf(stderr, "Host and port required\n");
    return 2;
  }

  sscanf(port_str, "%d", &port);
  if (port <= 0 || port > 65535) {
    fprintf(stderr, "Port has to be <= 0 and > 65535\n");
    return 3;
  }

  if (hostname == NULL) {
    hostname = malloc((HOST_NAME_MAX + 1) * sizeof(*hostname));
    if (gethostname(hostname, HOST_NAME_MAX) != 0) {
      fprintf(stderr, "Failed to get a host name, provide one explicitely with --hostname");
      return 4;
    }
  }

  forza_connect(host, port, hostname, user, name, on_connect);

  uv_run(loop, UV_RUN_DEFAULT);

  free(hostname);
  free(opt);

  return 0;
}
