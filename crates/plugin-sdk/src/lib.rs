//! Write Foxglove plugins in Rust (docs/local-api.md).
//!
//! A Plugin is a program Foxglove starts; it talks to Foxglove on its stdin/stdout. [`Plugin::connect`]
//! says hello (`initialize`) and keeps the plugin alive with a `ping` every 10 s, so the runner
//! doesn't restart it. Calls are typed where it helps ([`Plugin::set_fact`], [`Plugin::set_lighting`],
//! ...) and generic otherwise ([`Plugin::call`]). Fact changes you subscribe to arrive on
//! [`Plugin::events`].
//!
//! ```no_run
//! use foxglove_plugin::{Plugin, Fact};
//! let plugin = Plugin::connect("weather", "1.0.0").unwrap();
//! plugin.set_fact("weather.temp", Some(Fact::Text("14°".into()))).unwrap();
//! ```

use std::collections::HashMap;
use std::io::{BufRead, BufReader, Read, Write};
use std::sync::atomic::{AtomicI64, Ordering};
use std::sync::mpsc::{self, Receiver, Sender};
use std::sync::{Arc, Mutex};
use std::time::Duration;

pub use foxglove_plugin_api::methods::{
    Device, Devices, Fact, FactChanged, FactList, Hsb, InitializeResult, LightingState, SetLightingParams,
};
pub use foxglove_plugin_api::methods::NetKind;
use foxglove_plugin_api::rpc::{Error, Id, Outcome, Request, Response};

mod net;
pub use net::{NetEvent, TcpStream};
use serde::de::DeserializeOwned;
use serde_json::{json, Value};

/// A call that failed: Foxglove's error (code and message), or the connection.
#[derive(Debug)]
pub enum PluginError {
    Api(Error),
    Closed,
    Decode(String),
}

impl std::fmt::Display for PluginError {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        match self {
            PluginError::Api(e) => write!(f, "{} (code {})", e.message, e.code),
            PluginError::Closed => write!(f, "Foxglove closed the connection"),
            PluginError::Decode(e) => write!(f, "unexpected reply: {e}"),
        }
    }
}

impl std::error::Error for PluginError {}

pub type Result<T> = std::result::Result<T, PluginError>;

type Pending = Arc<Mutex<HashMap<i64, Sender<Outcome>>>>;

/// A connection to Foxglove.
pub struct Plugin {
    write: Mutex<Box<dyn Write + Send>>,
    next: AtomicI64,
    pending: Pending,
    events: Mutex<Option<Receiver<FactChanged>>>,
    actions: Mutex<Option<Receiver<String>>>,
    closed: Arc<std::sync::atomic::AtomicBool>,
    routes: net::Routes,
    /// What Foxglove granted this plugin.
    pub granted: Vec<String>,
}

impl Plugin {
    /// Connects over this process's stdin/stdout, as Foxglove starts plugins.
    pub fn connect(name: &str, version: &str) -> Result<Arc<Plugin>> {
        Plugin::connect_over(std::io::stdin(), std::io::stdout(), name, version)
    }

    /// Connects over any pair of streams (tests, other transports).
    pub fn connect_over(
        read: impl Read + Send + 'static,
        write: impl Write + Send + 'static,
        name: &str,
        version: &str,
    ) -> Result<Arc<Plugin>> {
        let pending: Pending = Arc::default();
        let (events_tx, events_rx) = mpsc::channel();
        let (actions_tx, actions_rx) = mpsc::channel::<String>();
        let reader_pending = pending.clone();
        let closed = Arc::new(std::sync::atomic::AtomicBool::new(false));
        let reader_closed = closed.clone();
        let routes = net::Routes::default();
        let reader_routes = routes.clone();
        std::thread::spawn(move || {
            for line in BufReader::new(read).lines() {
                let Ok(line) = line else { break };
                if let Ok(r) = serde_json::from_str::<Response>(&line) {
                    if let Id::Number(n) = r.id {
                        if let Some(tx) = reader_pending.lock().unwrap_or_else(|e| e.into_inner()).remove(&n) {
                            let _ = tx.send(r.outcome);
                        }
                    }
                } else if let Ok(n) = serde_json::from_str::<Request>(&line) {
                    if n.method == "facts.changed" {
                        if let Ok(ev) = serde_json::from_value(n.params) {
                            let _ = events_tx.send(ev);
                        }
                    } else if n.method == "action.invoked" {
                        if let Some(a) = n.params.get("action").and_then(Value::as_str) {
                            let _ = actions_tx.send(a.to_string());
                        }
                    } else {
                        reader_routes.handle(&n.method, n.params);
                    }
                }
            }
            // Fail every waiting call once Foxglove is gone.
            reader_closed.store(true, Ordering::Relaxed);
            reader_pending.lock().unwrap_or_else(|e| e.into_inner()).clear();
        });
        let mut plugin = Plugin {
            write: Mutex::new(Box::new(write)),
            next: AtomicI64::new(0),
            pending,
            events: Mutex::new(Some(events_rx)),
            actions: Mutex::new(Some(actions_rx)),
            closed,
            routes,
            granted: vec![],
        };
        let init: InitializeResult = plugin.call_as(
            "initialize",
            json!({"api": foxglove_plugin_api::API_VERSION.to_string(), "client": {"name": name, "version": version}}),
        )?;
        plugin.granted = init.granted.into_iter().map(String::from).collect();
        let plugin = Arc::new(plugin);
        let beat = Arc::downgrade(&plugin);
        std::thread::spawn(move || loop {
            std::thread::sleep(Duration::from_secs(10));
            match beat.upgrade() {
                Some(plugin) if plugin.call("ping", json!({})).is_ok() => {}
                _ => break,
            }
        });
        Ok(plugin)
    }

    /// Calls `method` and returns its result.
    pub fn call(&self, method: &str, params: Value) -> Result<Value> {
        let id = self.next.fetch_add(1, Ordering::Relaxed) + 1;
        let (tx, rx) = mpsc::channel();
        self.pending.lock().unwrap_or_else(|e| e.into_inner()).insert(id, tx);
        let line = serde_json::to_string(&Request::call(id, method, params)).expect("serializable");
        {
            let mut w = self.write.lock().unwrap_or_else(|e| e.into_inner());
            writeln!(w, "{line}").and_then(|()| w.flush()).map_err(|_| PluginError::Closed)?;
        }
        match rx.recv().map_err(|_| PluginError::Closed)? {
            Outcome::Result(v) => Ok(v),
            Outcome::Error(e) => Err(PluginError::Api(e)),
        }
    }

    fn call_as<T: DeserializeOwned>(&self, method: &str, params: Value) -> Result<T> {
        serde_json::from_value(self.call(method, params)?).map_err(|e| PluginError::Decode(e.to_string()))
    }

    pub fn fact(&self, key: &str) -> Result<Option<Fact>> {
        let r: foxglove_plugin_api::methods::FactResult = self.call_as("facts.get", json!({ "key": key }))?;
        Ok(r.value)
    }

    /// Publishes a fact (`None` removes it). Needs `facts.write` covering the key.
    pub fn set_fact(&self, key: &str, value: Option<Fact>) -> Result<()> {
        self.call("facts.set", json!({ "key": key, "value": value })).map(|_| ())
    }

    /// Subscribes to fact changes; they arrive on [`Plugin::events`].
    pub fn subscribe(&self, pattern: &str) -> Result<u64> {
        let s: foxglove_plugin_api::methods::Subscription = self.call_as("facts.subscribe", json!({ "pattern": pattern }))?;
        Ok(s.subscription)
    }

    /// The fact changes of every subscription (take it once).
    pub fn events(&self) -> Option<Receiver<FactChanged>> {
        self.events.lock().unwrap_or_else(|e| e.into_inner()).take()
    }

    /// Whether Foxglove has gone (its end of the connection closed): a plugin that holds
    /// something, such as a port, should stop.
    pub fn is_closed(&self) -> bool {
        self.closed.load(Ordering::Relaxed)
    }

    /// The ids of this plugin's actions (`[[actions]]` in `plugin.toml`) as the user presses
    /// their keys (#212). Taken once.
    pub fn actions(&self) -> Option<Receiver<String>> {
        self.actions.lock().unwrap_or_else(|e| e.into_inner()).take()
    }

    pub fn devices(&self) -> Result<Vec<Device>> {
        Ok(self.call_as::<Devices>("devices.list", json!({}))?.devices)
    }

    /// A device's lights as they are. Needs `devices.read`.
    pub fn lighting_state(&self, device: &str) -> Result<LightingState> {
        self.call_as("lighting.state", json!({ "device": device }))
    }

    /// Changes a device's lights (only what's given). Needs `lighting.write`.
    pub fn set_lighting(&self, params: &SetLightingParams) -> Result<()> {
        self.call("lighting.set", serde_json::to_value(params).expect("serializable")).map(|_| ())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::mpsc::Receiver as Rx;

    /// A pipe for tests: what one side writes, the other reads.
    struct PipeRead(Rx<Vec<u8>>, Vec<u8>);
    impl Read for PipeRead {
        fn read(&mut self, buf: &mut [u8]) -> std::io::Result<usize> {
            if self.1.is_empty() {
                match self.0.recv() {
                    Ok(b) => self.1 = b,
                    Err(_) => return Ok(0),
                }
            }
            let n = buf.len().min(self.1.len());
            buf[..n].copy_from_slice(&self.1[..n]);
            self.1.drain(..n);
            Ok(n)
        }
    }
    struct PipeWrite(Sender<Vec<u8>>);
    impl Write for PipeWrite {
        fn write(&mut self, b: &[u8]) -> std::io::Result<usize> {
            self.0.send(b.to_vec()).map_err(|_| std::io::ErrorKind::BrokenPipe)?;
            Ok(b.len())
        }
        fn flush(&mut self) -> std::io::Result<()> {
            Ok(())
        }
    }

    /// A fake Foxglove: answers initialize, facts.set and facts.subscribe, then sends an event.
    fn fake() -> (PipeRead, PipeWrite) {
        let (to_plugin, plugin_reads) = mpsc::channel::<Vec<u8>>();
        let (plugin_writes, from_plugin) = mpsc::channel::<Vec<u8>>();
        std::thread::spawn(move || {
            let mut buf = Vec::new();
            for chunk in from_plugin {
                buf.extend(chunk);
                while let Some(i) = buf.iter().position(|&b| b == b'\n') {
                    let line: Vec<u8> = buf.drain(..=i).collect();
                    let req: Request = serde_json::from_slice(&line).unwrap();
                    let id = req.id.clone().unwrap();
                    let result = match req.method.as_str() {
                        "initialize" => json!({"api":"1.1","app":"test","granted":["facts.write:focus.*"]}),
                        "facts.subscribe" => json!({"subscription": 7}),
                        _ => json!({}),
                    };
                    let mut out = serde_json::to_vec(&Response::ok(id, result)).unwrap();
                    out.push(b'\n');
                    to_plugin.send(out).unwrap();
                    if req.method == "facts.subscribe" {
                        let ev = Request::notification("facts.changed", json!({"subscription":7,"key":"clock.time","value":"12:00"}));
                        let mut out = serde_json::to_vec(&ev).unwrap();
                        out.push(b'\n');
                        to_plugin.send(out).unwrap();
                    }
                }
            }
        });
        (PipeRead(plugin_reads, Vec::new()), PipeWrite(plugin_writes))
    }

    #[test]
    fn connects_calls_and_receives_events() {
        let (r, w) = fake();
        let plugin = Plugin::connect_over(r, w, "test", "1").unwrap();
        assert_eq!(plugin.granted, vec!["facts.write:focus.*".to_string()]);
        plugin.set_fact("focus.state", Some(Fact::Text("focus".into()))).unwrap();
        let events = plugin.events().unwrap();
        assert_eq!(plugin.subscribe("clock.*").unwrap(), 7);
        let ev = events.recv_timeout(Duration::from_secs(2)).unwrap();
        assert_eq!((ev.key.as_str(), ev.value), ("clock.time", Some(Fact::Text("12:00".into()))));
    }
}
