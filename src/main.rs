extern crate libc;
extern crate numbat;

mod forza;
mod plugins;

use std::thread;
use forza::ForzaPlugin;

fn start_plugin<T: ForzaPlugin>(mut plugin: T) {
  plugin.start();
}

fn main() {
  let mut emitter = numbat::Emitter::for_app("host");
  emitter.connect(&match std::env::var_os("METRICS") {
    Some(url) => url.into_string().expect("expected METRICS to be valid UTF-8"),
    None => String::from("tcp://127.0.0.1:1337")
  });

  emitter.emit_name("forza.start");

  let heartbeat = plugins::heartbeat::new(emitter.clone());
  thread::spawn(|| {
    start_plugin(heartbeat);
  });

  let memory = plugins::memory::Memory::new(emitter.clone());
  thread::spawn(|| {
    start_plugin(memory);
  });

  let load_average = plugins::load_average::LoadAverage::new(emitter.clone());
  thread::spawn(|| {
    start_plugin(load_average);
  }).join();
}
