#include <uv.h>
#include <forza.h>

#ifdef __sun
#include <sys/pset.h>
#include <sys/loadavg.h>
#endif

static uv_timer_t cpu_timer;

void load_average__send(uv_timer_t *timer, int status) {
  double loadinfo[3];
  forza_metric_t* metric = forza_new_metric();

#ifdef DEBUG
  printf("cpu usage timer fired, status %d\n", status);
#endif
#ifdef __sun
  /* On SunOS, if we're not in a global zone, uv_loadavg returns [0, 0, 0] */
  /* This, instead, gets the loadavg for our assigned processor set. */
  pset_getloadavg(PS_MYID, loadinfo, 1);
#else
  uv_loadavg(loadinfo);
#endif

  metric->service = "load-average.1";
  metric->metric = loadinfo[0];
  forza_send(metric);

  metric->service = "load-average.5";
  metric->metric = loadinfo[1];
  forza_send(metric);

  metric->service = "load-average.15";
  metric->metric = loadinfo[2];
  forza_send(metric);

  forza_free_metric(metric);
}

void load_average__process_exit_cb(int exit_status, int term_signal) {
  uv_timer_stop(&cpu_timer);
}

int load_average_init(forza_plugin_t* plugin) {
  plugin->process_exit_cb = load_average__process_exit_cb;

  uv_timer_init(uv_default_loop(), &cpu_timer);
  uv_timer_start(&cpu_timer, load_average__send, 0, 5000);

  return 0;
}
