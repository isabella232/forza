extern crate linux_stats;
extern crate numbat;

use forza;

pub struct Processes<'a> {
  emitter: numbat::Emitter<'a>
}

impl<'a> Processes<'a> {
  pub fn new(emitter: numbat::Emitter<'a>) -> Processes {
    Processes {
      emitter: emitter
    }
  }

  fn send(&mut self) {
    let stats = linux_stats::stat().unwrap();
    self.emitter.emit("processes", stats.processes);
    self.emitter.emit("processes.running", stats.procs_running);
    self.emitter.emit("processes.blocked", stats.procs_blocked);
  }
}

impl<'a> forza::ForzaPlugin for Processes<'a> {
  fn start(&mut self) {
    println!("starting processes plugin");
    forza::schedule_repeating(move || {
      self.send();
    }, 10);
  }
}
