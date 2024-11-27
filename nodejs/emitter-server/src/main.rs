use std::collections::{HashMap, LinkedList};
use std::net::TcpListener;
use std::ops::Drop;

// stripped down version of EventEmitter and Server

trait EventEmitter {
    fn add_listener(&mut self, event: String, callback: fn() -> ());
    fn emit(&mut self, event: String);
}

struct EventEmitterImpl {
    // event_map: HashMap<String, LinkedList<Box<dyn Fn() -> ()>>>,
    event_map: HashMap<String, LinkedList<fn() -> ()>>,
}

impl EventEmitterImpl {
    pub fn new() -> Self {
        Self {
            event_map: HashMap::new(),
        }
    }
}

impl EventEmitter for EventEmitterImpl {
    // pub fn add_listener(&mut self, event: String, callback: Box<dyn Fn() -> ()>) {
    fn add_listener(&mut self, event: String, callback: fn() -> ()) {
        let listeners = self.event_map.get_mut(&event);
        match listeners {
            Some(list) => {
                list.push_back(callback);
            }
            None => {
                let mut list = LinkedList::new();
                list.push_back(callback);
                self.event_map.insert(event, list);
            }
        }
    }

    fn emit(&mut self, event: String) {
        let Some(listeners) = self.event_map.get_mut(&event) else {
            return;
        };

        for callback in listeners {
            callback();
        }
    }
}

struct Server {
    emitter: EventEmitterImpl,
    socket: TcpListener,
}

impl Server {
    fn new() -> Self {
        Self {
            socket: TcpListener::bind("127.0.0.1:8080").unwrap(),
            emitter: EventEmitterImpl::new(),
        }
    }

    fn listen(&mut self) {
        let mut count: i32 = 5;
        self.emitter.emit("listening".to_string());
        for stream in self.socket.incoming() {
            // loop 5 times
            if count == 0 { return };
            count -= 1;

            match stream {
                Ok(_) => {
                    self.emitter.emit("connection".to_string());
                }
                Err(e) if e.kind() == std::io::ErrorKind::OutOfMemory => {
                    // TODO: find a better place for this
                    self.emitter.emit("drop".to_string());
                }
                Err(_) => {
                    self.emitter.emit("error".to_string());
                }
            }
        }
    }
}

impl Drop for Server {
    fn drop(&mut self) {
        self.emit("close".to_string());
    }
}

impl EventEmitter for Server {
    fn add_listener(&mut self, event: String, callback: fn() -> ()) {
        self.emitter.add_listener(event, callback);
    }

    fn emit(&mut self, event: String) {
        self.emitter.emit(event);
    }
}

fn main() {
    let mut server = Server::new();
    server.add_listener("connection".to_string(), || {
        println!("Server Connection");
    });

    server.add_listener("close".to_string(), || {
        println!("Server Close");
    });

    server.add_listener("error".to_string(), || {
        println!("Server Error");
    });

    server.listen();
}
