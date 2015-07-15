#include <uv.h>
#include <forza.h>

static uv_timer_t memory_timer;

void memory__send_usage(uv_timer_t *timer, int status) {
  double mempct;
  uint64_t freemem = uv_get_free_memory();
  uint64_t totalmem = uv_get_total_memory();

#ifdef DEBUG
  printf("memory usage timer fired, status %d\n", status);
#endif
  mempct = (double)(totalmem - freemem) / (double)totalmem;

  forza_metric_t* metric = forza_new_metric();
  metric->service = "memory";
  metric->metric = mempct;
  forza_send(metric);
  forza_free_metric(metric);
}

void memory__process_exit_cb(int exit_status, int term_singal) {
  uv_timer_stop(&memory_timer);
}

int memory_init(forza_plugin_t* plugin) {
  plugin->process_exit_cb = memory__process_exit_cb;

  uv_timer_init(uv_default_loop(), &memory_timer);
  uv_timer_start(&memory_timer, memory__send_usage, 0, 5000);

  return 0;
}
