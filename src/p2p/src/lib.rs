// SPDX-License-Identifier: MPL-2.0
#![allow(non_camel_case_types)]
//! e3p2p: moves bytes between e3mail devices over Iroh (ADR 0010).
//!
//! The C++ side owns every rule. This crate dials and accepts QUIC
//! connections by public key, refuses peers it was not told about, and
//! carries length-prefixed frames on one bidirectional stream per
//! connection. Everything is non-blocking: calls return at once, and what
//! happens arrives as events that the caller polls after the waker fires.
//! The C interface is in `e3p2p.h`.

use std::collections::{HashMap, HashSet, VecDeque};
use std::ffi::{CStr, CString, c_char, c_void};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};

use iroh::endpoint::{Connection, ConnectionError, RecvStream, SendStream, presets};
use iroh::{Endpoint, EndpointAddr, EndpointId, RelayMap, RelayMode, RelayUrl, SecretKey};
use tokio::sync::mpsc;

/// How a stranger's connection is closed, before anything is read.
const CLOSE_NOT_PAIRED: u32 = 1;
const NOT_PAIRED: &str = "not paired";

/// The protocol every e3mail device speaks; a peer offering another is refused.
pub const ALPN: &[u8] = b"e3mail/sync/1";
/// The largest frame accepted: one raw message, or one part of a snapshot.
pub const MAX_FRAME: usize = 64 << 20;

pub const EVENT_CONNECTED: i32 = 1;
pub const EVENT_CLOSED: i32 = 2;
pub const EVENT_FRAME: i32 = 3;

pub const RELAY_DEFAULT: i32 = 0;
pub const RELAY_CUSTOM: i32 = 1;
pub const RELAY_OFF: i32 = 2;

/// `e3p2p_start`'s `network`: every path, this machine only, or the relay only.
pub const NETWORK_ANY: i32 = 0;
pub const NETWORK_LOOPBACK: i32 = 1;
pub const NETWORK_RELAY_ONLY: i32 = 2;

/// `e3p2p_conn_path`: the path a connection's data takes now.
pub const PATH_UNKNOWN: i32 = 0;
pub const PATH_DIRECT: i32 = 1;
pub const PATH_RELAY: i32 = 2;

/// One thing that happened, for `e3p2p_poll`.
#[repr(C)]
pub struct e3p2p_event {
    pub kind: i32,
    pub conn: u64,
    /// The peer's endpoint id, NUL-terminated.
    pub peer: [c_char; 65],
    /// Connected: 1 if the peer dialed us.
    pub incoming: i32,
    /// Frame: its kind byte.
    pub frame: u8,
    /// Frame: its bytes; Closed: why, as text. Freed by `e3p2p_event_free`.
    pub data: *mut u8,
    pub len: usize,
}

struct Event {
    kind: i32,
    conn: u64,
    peer: String,
    incoming: bool,
    frame: u8,
    data: Vec<u8>,
}

type Waker = extern "C" fn(*mut c_void);

struct Shared {
    events: Mutex<VecDeque<Event>>,
    // Cleared by e3p2p_stop under the lock: once it returns, the waker is
    // never called again, even by a task still finishing on another thread.
    waker: Mutex<Option<(Waker, usize)>>,
    conns: Mutex<HashMap<u64, mpsc::UnboundedSender<Vec<u8>>>>,
    live: Mutex<HashMap<u64, Connection>>,
    next: AtomicU64,
    allowed: Mutex<HashSet<EndpointId>>,
    accept_any: AtomicBool,
}

impl Shared {
    fn push(&self, e: Event) {
        self.events.lock().unwrap().push_back(e);
        let waker = self.waker.lock().unwrap();
        if let Some((w, ctx)) = *waker {
            w(ctx as *mut c_void);
        }
    }

    fn closed(&self, conn: u64, peer: &str, why: String) {
        self.conns.lock().unwrap().remove(&conn);
        self.live.lock().unwrap().remove(&conn);
        self.push(Event { kind: EVENT_CLOSED, conn, peer: peer.to_owned(), incoming: false, frame: 0, data: why.into_bytes() });
    }

    fn is_allowed(&self, id: &EndpointId) -> bool {
        self.accept_any.load(Ordering::SeqCst) || self.allowed.lock().unwrap().contains(id)
    }
}

/// A running endpoint.
pub struct e3p2p_node {
    rt: tokio::runtime::Runtime,
    endpoint: Endpoint,
    shared: Arc<Shared>,
}

fn frame_bytes(kind: u8, data: &[u8]) -> Vec<u8> {
    let mut buf = Vec::with_capacity(5 + data.len());
    buf.push(kind);
    buf.extend_from_slice(&(data.len() as u32).to_be_bytes());
    buf.extend_from_slice(data);
    buf
}

/// Reads one frame, or None at the end of the stream.
async fn read_frame(recv: &mut RecvStream) -> Result<Option<(u8, Vec<u8>)>, String> {
    let mut head = [0u8; 5];
    match recv.read_exact(&mut head).await {
        Ok(()) => {}
        Err(iroh::endpoint::ReadExactError::FinishedEarly(0)) => return Ok(None),
        Err(e) => return Err(e.to_string()),
    }
    let len = u32::from_be_bytes([head[1], head[2], head[3], head[4]]) as usize;
    if len > MAX_FRAME {
        return Err(format!("frame of {len} bytes is too large"));
    }
    let mut data = vec![0u8; len];
    recv.read_exact(&mut data).await.map_err(|e| e.to_string())?;
    Ok(Some((head[0], data)))
}

/// Carries one connection: writes what is queued, reads frames until it ends.
async fn run_connection(
    shared: Arc<Shared>,
    id: u64,
    conn: Connection,
    mut send: SendStream,
    mut recv: RecvStream,
    mut queue: mpsc::UnboundedReceiver<Vec<u8>>,
) {
    let peer = conn.remote_id().to_string();
    let writer = tokio::spawn(async move {
        while let Some(buf) = queue.recv().await {
            if send.write_all(&buf).await.is_err() {
                break;
            }
        }
        let _ = send.finish();
    });
    let why = loop {
        match read_frame(&mut recv).await {
            Ok(Some((kind, data))) => shared.push(Event {
                kind: EVENT_FRAME,
                conn: id,
                peer: peer.clone(),
                incoming: false,
                frame: kind,
                data,
            }),
            Ok(None) => break "closed by the peer".to_owned(),
            Err(e) => break e,
        }
    };
    writer.abort();
    // A refusal says so, whatever the read reported: the other side uses it
    // to learn that it was removed.
    let why = match conn.close_reason() {
        Some(ConnectionError::ApplicationClosed(f)) if f.error_code == CLOSE_NOT_PAIRED.into() => NOT_PAIRED.to_owned(),
        _ => why,
    };
    conn.close(0u32.into(), b"bye");
    shared.closed(id, &peer, why);
}

fn register(shared: &Arc<Shared>, conn: &Connection, incoming: bool) -> (u64, mpsc::UnboundedReceiver<Vec<u8>>) {
    let id = shared.next.fetch_add(1, Ordering::SeqCst);
    let (tx, rx) = mpsc::unbounded_channel();
    shared.conns.lock().unwrap().insert(id, tx);
    shared.live.lock().unwrap().insert(id, conn.clone());
    shared.push(Event {
        kind: EVENT_CONNECTED,
        conn: id,
        peer: conn.remote_id().to_string(),
        incoming,
        frame: 0,
        data: Vec::new(),
    });
    (id, rx)
}

async fn accept_loop(endpoint: Endpoint, shared: Arc<Shared>) {
    while let Some(incoming) = endpoint.accept().await {
        let shared = shared.clone();
        tokio::spawn(async move {
            let Ok(conn) = incoming.await else { return };
            // Strangers are refused before any stream is read.
            if !shared.is_allowed(&conn.remote_id()) {
                conn.close(CLOSE_NOT_PAIRED.into(), NOT_PAIRED.as_bytes());
                return;
            }
            let Ok((send, recv)) = conn.accept_bi().await else { return };
            let (id, rx) = register(&shared, &conn, true);
            run_connection(shared, id, conn, send, recv, rx).await;
        });
    }
}

fn parse_addr(text: &str) -> Option<EndpointAddr> {
    if let Ok(addr) = serde_json::from_str::<EndpointAddr>(text) {
        return Some(addr);
    }
    // A bare id: found through the address lookup and the relay.
    text.parse::<EndpointId>().ok().map(EndpointAddr::new)
}

unsafe fn str_arg<'a>(p: *const c_char) -> Option<&'a str> {
    if p.is_null() { None } else { unsafe { CStr::from_ptr(p) }.to_str().ok() }
}

/// Android only: hands Iroh the Java VM and the application context, which
/// its DNS resolver reads the system's nameservers through. Without them the
/// resolver panics, and under `panic = "abort"` that ends the app. Call it
/// before the first `e3p2p_start`; later calls do nothing. Both pointers must
/// stay valid until the process exits (a global reference for the context).
#[cfg(target_os = "android")]
#[unsafe(no_mangle)]
pub extern "C" fn e3p2p_android_init(java_vm: *mut c_void, context: *mut c_void) {
    static ONCE: std::sync::Once = std::sync::Once::new();
    ONCE.call_once(|| unsafe { iroh::dns::install_android_jni_context(java_vm, context) });
}

/// Fills `out` with a new secret key.
#[unsafe(no_mangle)]
pub extern "C" fn e3p2p_generate_secret(out: *mut u8) {
    let key = SecretKey::generate();
    unsafe { std::ptr::copy_nonoverlapping(key.to_bytes().as_ptr(), out, 32) };
}

/// Starts an endpoint with this secret key. `relay`: RELAY_DEFAULT (n0's),
/// RELAY_CUSTOM (`relay_url`) or RELAY_OFF. `port`: a fixed UDP port, or 0
/// for any. `network`: NETWORK_ANY; NETWORK_LOOPBACK, listening on this
/// machine only (tests and screenshots, which must neither reach out nor show
/// real addresses); or NETWORK_RELAY_ONLY, no direct paths at all (trying a
/// relay between two processes on one machine). `waker` is called, from
/// another thread, whenever events are waiting. Null on failure.
#[unsafe(no_mangle)]
pub extern "C" fn e3p2p_start(
    secret: *const u8,
    relay: i32,
    relay_url: *const c_char,
    port: u16,
    network: i32,
    waker: Option<Waker>,
    waker_ctx: *mut c_void,
) -> *mut e3p2p_node {
    let mut bytes = [0u8; 32];
    unsafe { std::ptr::copy_nonoverlapping(secret, bytes.as_mut_ptr(), 32) };
    let mode = match relay {
        RELAY_OFF => RelayMode::Disabled,
        RELAY_CUSTOM => match unsafe { str_arg(relay_url) }.and_then(|u| u.parse::<RelayUrl>().ok()) {
            Some(url) => RelayMode::Custom(RelayMap::from(url)),
            None => return std::ptr::null_mut(),
        },
        _ => RelayMode::Default,
    };
    let Ok(rt) = tokio::runtime::Builder::new_multi_thread().worker_threads(2).enable_all().build() else {
        return std::ptr::null_mut();
    };
    let shared = Arc::new(Shared {
        events: Mutex::new(VecDeque::new()),
        waker: Mutex::new(waker.map(|w| (w, waker_ctx as usize))),
        conns: Mutex::new(HashMap::new()),
        live: Mutex::new(HashMap::new()),
        next: AtomicU64::new(1),
        allowed: Mutex::new(HashSet::new()),
        accept_any: AtomicBool::new(false),
    });
    let endpoint = rt.block_on(async {
        let mut builder = Endpoint::builder(presets::N0)
            .secret_key(SecretKey::from_bytes(&bytes))
            .relay_mode(mode)
            .alpns(vec![ALPN.to_vec()]);
        if network == NETWORK_RELAY_ONLY {
            builder = builder.clear_ip_transports();
        } else if network == NETWORK_LOOPBACK {
            builder = builder
                .clear_ip_transports()
                .bind_addr(format!("127.0.0.1:{port}").as_str())
                .map_err(|e| e.to_string())?;
        } else if port != 0 {
            builder = builder
                .clear_ip_transports()
                .bind_addr(format!("0.0.0.0:{port}").as_str())
                .map_err(|e| e.to_string())?
                .bind_addr(format!("[::]:{port}").as_str())
                .map_err(|e| e.to_string())?;
        }
        builder.bind().await.map_err(|e| e.to_string())
    });
    let Ok(endpoint) = endpoint else { return std::ptr::null_mut() };
    rt.spawn(accept_loop(endpoint.clone(), shared.clone()));
    Box::into_raw(Box::new(e3p2p_node { rt, endpoint, shared }))
}

/// Closes every connection and frees the node.
#[unsafe(no_mangle)]
pub extern "C" fn e3p2p_stop(node: *mut e3p2p_node) {
    if node.is_null() {
        return;
    }
    let node = unsafe { Box::from_raw(node) };
    *node.shared.waker.lock().unwrap() = None;
    let endpoint = node.endpoint.clone();
    let _ = node.rt.block_on(async {
        tokio::time::timeout(std::time::Duration::from_secs(2), endpoint.close()).await
    });
    node.rt.shutdown_background();
}

/// This endpoint's id (its public key), NUL-terminated, into 65 bytes.
#[unsafe(no_mangle)]
pub extern "C" fn e3p2p_node_id(node: *const e3p2p_node, out: *mut c_char) {
    let id = unsafe { &*node }.endpoint.id().to_string();
    let c = CString::new(id).unwrap_or_default();
    let b = c.as_bytes_with_nul();
    unsafe { std::ptr::copy_nonoverlapping(b.as_ptr() as *const c_char, out, b.len().min(65)) };
}

/// Where this endpoint can be reached now, as text for `e3p2p_connect`.
/// Free with `e3p2p_string_free`.
#[unsafe(no_mangle)]
pub extern "C" fn e3p2p_addr(node: *const e3p2p_node) -> *mut c_char {
    let addr = unsafe { &*node }.endpoint.addr();
    CString::new(serde_json::to_string(&addr).unwrap_or_default()).unwrap_or_default().into_raw()
}

/// Waits up to `ms` for a relay connection or a direct address. 1 if online.
#[unsafe(no_mangle)]
pub extern "C" fn e3p2p_wait_online(node: *const e3p2p_node, ms: u64) -> i32 {
    let node = unsafe { &*node };
    let endpoint = node.endpoint.clone();
    node.rt
        .block_on(async { tokio::time::timeout(std::time::Duration::from_millis(ms), endpoint.online()).await })
        .is_ok() as i32
}

#[unsafe(no_mangle)]
pub extern "C" fn e3p2p_string_free(s: *mut c_char) {
    if !s.is_null() {
        drop(unsafe { CString::from_raw(s) });
    }
}

/// Lets a peer connect to us (or stops letting it).
#[unsafe(no_mangle)]
pub extern "C" fn e3p2p_allow(node: *const e3p2p_node, peer: *const c_char, allowed: i32) -> i32 {
    let node = unsafe { &*node };
    let Some(id) = unsafe { str_arg(peer) }.and_then(|p| p.parse::<EndpointId>().ok()) else { return 0 };
    let mut set = node.shared.allowed.lock().unwrap();
    if allowed != 0 { set.insert(id) } else { set.remove(&id) };
    1
}

/// While on, any peer may connect: for pairing, where the caller checks
/// the token in the first frame and closes the connection otherwise.
#[unsafe(no_mangle)]
pub extern "C" fn e3p2p_accept_any(node: *const e3p2p_node, on: i32) {
    unsafe { &*node }.shared.accept_any.store(on != 0, Ordering::SeqCst);
}

/// Dials a peer: `addr` from `e3p2p_addr`, or a bare endpoint id. Returns
/// at once; a Connected or Closed event follows. 0 if `addr` is unreadable.
#[unsafe(no_mangle)]
pub extern "C" fn e3p2p_connect(node: *const e3p2p_node, addr: *const c_char) -> u64 {
    let node = unsafe { &*node };
    let Some(addr) = unsafe { str_arg(addr) }.and_then(parse_addr) else { return 0 };
    // The id is handed out now, so a failed dial still has one to report.
    let pending = node.shared.next.fetch_add(1, Ordering::SeqCst);
    let endpoint = node.endpoint.clone();
    let shared = node.shared.clone();
    node.rt.spawn(async move {
        let peer = addr.id.to_string();
        let conn = match endpoint.connect(addr, ALPN).await {
            Ok(c) => c,
            Err(e) => return shared.closed(pending, &peer, e.to_string()),
        };
        let (send, recv) = match conn.open_bi().await {
            Ok(s) => s,
            Err(e) => return shared.closed(pending, &peer, e.to_string()),
        };
        let (tx, rx) = mpsc::unbounded_channel();
        shared.conns.lock().unwrap().insert(pending, tx);
        shared.live.lock().unwrap().insert(pending, conn.clone());
        shared.push(Event { kind: EVENT_CONNECTED, conn: pending, peer, incoming: false, frame: 0, data: Vec::new() });
        run_connection(shared, pending, conn, send, recv, rx).await;
    });
    pending
}

/// The path the connection's data takes now: PATH_DIRECT, PATH_RELAY, or
/// PATH_UNKNOWN if none is selected yet or the connection is gone.
#[unsafe(no_mangle)]
pub extern "C" fn e3p2p_conn_path(node: *const e3p2p_node, conn: u64) -> i32 {
    let node = unsafe { &*node };
    let live = node.shared.live.lock().unwrap();
    let Some(c) = live.get(&conn) else { return PATH_UNKNOWN };
    let paths = c.paths();
    match paths.iter().find(|p| p.is_selected()) {
        Some(p) if p.is_relay() => PATH_RELAY,
        Some(_) => PATH_DIRECT,
        None => PATH_UNKNOWN,
    }
}

/// Queues a frame. 0 if the connection is gone or the frame too large.
#[unsafe(no_mangle)]
pub extern "C" fn e3p2p_send(node: *const e3p2p_node, conn: u64, kind: u8, data: *const u8, len: usize) -> i32 {
    if len > MAX_FRAME {
        return 0;
    }
    let node = unsafe { &*node };
    let bytes = if len == 0 { &[][..] } else { unsafe { std::slice::from_raw_parts(data, len) } };
    match node.shared.conns.lock().unwrap().get(&conn) {
        Some(tx) => tx.send(frame_bytes(kind, bytes)).is_ok() as i32,
        None => 0,
    }
}

/// Closes a connection; a Closed event follows.
#[unsafe(no_mangle)]
pub extern "C" fn e3p2p_close(node: *const e3p2p_node, conn: u64) {
    let node = unsafe { &*node };
    if let Some(c) = node.shared.live.lock().unwrap().get(&conn) {
        c.close(0u32.into(), b"closed");
    }
}

/// Takes the next event. 1 if `out` was filled; free it with `e3p2p_event_free`.
#[unsafe(no_mangle)]
pub extern "C" fn e3p2p_poll(node: *const e3p2p_node, out: *mut e3p2p_event) -> i32 {
    let node = unsafe { &*node };
    let Some(e) = node.shared.events.lock().unwrap().pop_front() else { return 0 };
    let out = unsafe { &mut *out };
    out.kind = e.kind;
    out.conn = e.conn;
    out.peer = [0; 65];
    for (i, b) in e.peer.bytes().take(64).enumerate() {
        out.peer[i] = b as c_char;
    }
    out.incoming = e.incoming as i32;
    out.frame = e.frame;
    let mut data = e.data.into_boxed_slice();
    out.len = data.len();
    out.data = if out.len == 0 { std::ptr::null_mut() } else { data.as_mut_ptr() };
    std::mem::forget(data);
    1
}

#[unsafe(no_mangle)]
pub extern "C" fn e3p2p_event_free(e: *mut e3p2p_event) {
    let e = unsafe { &mut *e };
    if !e.data.is_null() {
        drop(unsafe { Box::from_raw(std::ptr::slice_from_raw_parts_mut(e.data, e.len)) });
        e.data = std::ptr::null_mut();
        e.len = 0;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::{Duration, Instant};

    extern "C" fn no_wake(_: *mut c_void) {}

    fn start() -> *mut e3p2p_node {
        let mut secret = [0u8; 32];
        e3p2p_generate_secret(secret.as_mut_ptr());
        let n = e3p2p_start(secret.as_ptr(), RELAY_OFF, std::ptr::null(), 0, 1, Some(no_wake), std::ptr::null_mut());
        assert!(!n.is_null());
        n
    }

    fn id(n: *mut e3p2p_node) -> String {
        let mut buf = [0 as c_char; 65];
        e3p2p_node_id(n, buf.as_mut_ptr());
        unsafe { CStr::from_ptr(buf.as_ptr()) }.to_str().unwrap().to_owned()
    }

    fn addr(n: *mut e3p2p_node) -> CString {
        let p = e3p2p_addr(n);
        let s = unsafe { CStr::from_ptr(p) }.to_owned();
        e3p2p_string_free(p);
        s
    }

    fn next(n: *mut e3p2p_node, kind: i32) -> (u64, String, u8, Vec<u8>) {
        let deadline = Instant::now() + Duration::from_secs(20);
        while Instant::now() < deadline {
            let mut e = e3p2p_event { kind: 0, conn: 0, peer: [0; 65], incoming: 0, frame: 0, data: std::ptr::null_mut(), len: 0 };
            if e3p2p_poll(n, &mut e) == 1 {
                let data = if e.len > 0 { unsafe { std::slice::from_raw_parts(e.data, e.len) }.to_vec() } else { vec![] };
                let peer = unsafe { CStr::from_ptr(e.peer.as_ptr()) }.to_str().unwrap().to_owned();
                let got = (e.kind, e.conn, peer, e.frame, data);
                e3p2p_event_free(&mut e);
                if got.0 == kind {
                    return (got.1, got.2, got.3, got.4);
                }
                continue;
            }
            std::thread::sleep(Duration::from_millis(20));
        }
        panic!("no event of kind {kind}");
    }

    #[test]
    fn frames_travel_both_ways_between_allowed_peers() {
        let a = start();
        let b = start();
        let b_id = CString::new(id(b)).unwrap();
        let a_id = CString::new(id(a)).unwrap();
        e3p2p_allow(b, a_id.as_ptr(), 1);
        let _ = b_id;
        let conn = e3p2p_connect(a, addr(b).as_ptr());
        assert_ne!(conn, 0);
        let (c, peer, _, _) = next(a, EVENT_CONNECTED);
        assert_eq!(c, conn);
        assert_eq!(peer, id(b));
        // The acceptor sees the connection once the first frame opens the stream.
        assert_eq!(e3p2p_send(a, conn, 7, b"hello".as_ptr(), 5), 1);
        let (bc, bpeer, _, _) = next(b, EVENT_CONNECTED);
        assert_eq!(bpeer, id(a));
        let (_, _, kind, data) = next(b, EVENT_FRAME);
        assert_eq!((kind, data.as_slice()), (7, &b"hello"[..]));
        let big = vec![0xabu8; 3 << 20];
        assert_eq!(e3p2p_send(b, bc, 9, big.as_ptr(), big.len()), 1);
        let (_, _, kind, data) = next(a, EVENT_FRAME);
        assert_eq!(kind, 9);
        assert_eq!(data, big);
        e3p2p_close(a, conn);
        next(b, EVENT_CLOSED);
        e3p2p_stop(a);
        e3p2p_stop(b);
    }

    #[test]
    fn strangers_are_refused() {
        let a = start();
        let b = start();
        let conn = e3p2p_connect(a, addr(b).as_ptr());
        e3p2p_send(a, conn, 1, b"x".as_ptr(), 1);
        next(a, EVENT_CLOSED);
        let mut e = e3p2p_event { kind: 0, conn: 0, peer: [0; 65], incoming: 0, frame: 0, data: std::ptr::null_mut(), len: 0 };
        std::thread::sleep(Duration::from_millis(300));
        while e3p2p_poll(b, &mut e) == 1 {
            assert_ne!(e.kind, EVENT_FRAME);
            assert_ne!(e.kind, EVENT_CONNECTED);
            e3p2p_event_free(&mut e);
        }
        e3p2p_stop(a);
        e3p2p_stop(b);
    }

    #[test]
    fn frames_are_length_prefixed() {
        assert_eq!(frame_bytes(3, b"ab"), vec![3, 0, 0, 0, 2, b'a', b'b']);
    }
}
