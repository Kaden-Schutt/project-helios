//! helios_ble — minimal BLE transport that bypasses bleak's 23-byte MTU bug
//! =========================================================================
//! Problem: `bleak` on BlueZ 5.66+ never negotiates ATT MTU above the default
//! 23 bytes, even though the peripheral (ESP32-S3) advertises 512. This
//! bottlenecks JPEG/Opus transfer so badly that the Helios pipeline is unusable.
//!
//! Fix: Skip bleak entirely. Use `bluer` (official BlueZ Rust bindings) to
//! call `AcquireWrite` / `AcquireNotify` on each GATT characteristic, which
//! forces BlueZ to run the ATT MTU Exchange and return a dedicated socket
//! whose `mtu()` is the real negotiated value. If the result is below 512,
//! fail loudly — the caller should not attempt to proceed.
//!
//! Exposed as a Python extension via PyO3 so `server_ble.py` can drop bleak
//! for the transport layer without touching the rest of the pipeline.

use std::collections::HashMap;
use std::str::FromStr;
use std::sync::Arc;
use std::time::Duration;

use bluer::{
    gatt::remote::{Characteristic, CharacteristicReader, CharacteristicWriter},
    Address, Session, Uuid,
};
use pyo3::exceptions::{PyConnectionError, PyRuntimeError, PyValueError};
use pyo3::prelude::*;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::runtime::Runtime;
use tokio::sync::Mutex;
use tokio::task::JoinHandle;

/// Minimum MTU we require. Below this, Helios's JPEG/Opus transfer degrades
/// so badly that the pipeline stops working end-to-end. Fail loudly.
const MIN_MTU: u16 = 512;

/// Everything bluer-related lives behind this mutex so the Python side
/// sees a synchronous API while tokio drives the underlying async runtime.
struct Inner {
    /// Hold the session to keep the D-Bus connection alive.
    _session: Option<Session>,
    /// The connected device. Kept so disconnect() can call device.disconnect().
    device: Option<bluer::Device>,
    /// All discovered characteristics keyed by UUID — used by start_notify()
    /// to look up the target characteristic without re-discovering.
    chars: HashMap<Uuid, Characteristic>,
    /// Writers acquired at connect(). Each holds a SOCK_SEQPACKET to BlueZ
    /// with the negotiated MTU baked in — every write() syscall is one ATT
    /// packet at up to `mtu` bytes, no chunking overhead on the D-Bus layer.
    writers: HashMap<Uuid, CharacteristicWriter>,
    /// Background tasks reading from notify sockets. Aborted on disconnect
    /// or stop_notify. Keeping the JoinHandle here means we can cancel them
    /// without racing the socket close.
    reader_tasks: HashMap<Uuid, JoinHandle<()>>,
    /// The highest MTU observed across all writers/readers — should be 512
    /// or greater for Helios to work correctly.
    mtu: u16,
}

impl Inner {
    fn new() -> Self {
        Self {
            _session: None,
            device: None,
            chars: HashMap::new(),
            writers: HashMap::new(),
            reader_tasks: HashMap::new(),
            mtu: 0,
        }
    }
}

/// Python-visible handle. One per connection. Owns a tokio multi-threaded
/// runtime so every method call can `block_on` without fighting the GIL.
#[pyclass]
struct HeliosBle {
    runtime: Arc<Runtime>,
    inner: Arc<Mutex<Inner>>,
}

#[pymethods]
impl HeliosBle {
    #[new]
    fn new() -> PyResult<Self> {
        // Multi-threaded runtime so notify reader tasks and the main
        // block_on caller don't deadlock each other.
        let runtime = Runtime::new()
            .map_err(|e| PyRuntimeError::new_err(format!("tokio runtime: {e}")))?;
        Ok(Self {
            runtime: Arc::new(runtime),
            inner: Arc::new(Mutex::new(Inner::new())),
        })
    }

    /// Connect to the device at `mac`, discover its GATT tree, and acquire
    /// a write socket for every UUID in `write_chars`. Acquiring a write
    /// socket is what triggers the BlueZ-side ATT MTU Exchange — the
    /// returned socket's `mtu()` reports the real, negotiated size.
    ///
    /// Returns the highest negotiated MTU observed across the sockets.
    /// Raises `ConnectionError` if the negotiated MTU is below `MIN_MTU`
    /// (512), or if any step of the connection/discovery fails.
    ///
    /// `mac`: device address, e.g. "90:70:69:13:01:C2"
    /// `write_chars`: list of characteristic UUID strings we plan to write to
    fn connect(&self, mac: String, write_chars: Vec<String>) -> PyResult<u16> {
        // Parse UUIDs and address up front so we fail fast on bad input
        // before touching the BlueZ D-Bus layer.
        let write_uuids: Vec<Uuid> = write_chars
            .iter()
            .map(|s| Uuid::from_str(s))
            .collect::<Result<Vec<_>, _>>()
            .map_err(|e| PyValueError::new_err(format!("invalid write UUID: {e}")))?;
        let address = Address::from_str(&mac)
            .map_err(|e| PyValueError::new_err(format!("invalid MAC address: {e}")))?;

        let inner = self.inner.clone();
        let runtime = self.runtime.clone();

        runtime.block_on(async move {
            let session = Session::new().await
                .map_err(|e| PyConnectionError::new_err(format!("session: {e}")))?;
            let adapter = session.default_adapter().await
                .map_err(|e| PyConnectionError::new_err(format!("adapter: {e}")))?;
            adapter.set_powered(true).await
                .map_err(|e| PyConnectionError::new_err(format!("power on adapter: {e}")))?;

            // BlueZ needs to have discovered the device recently for `device()`
            // to return a usable proxy. Kick off a short discovery cycle and
            // drop the handle — we don't need to iterate events, we just need
            // the BlueZ cache populated.
            {
                let _discovery = adapter.discover_devices().await
                    .map_err(|e| PyConnectionError::new_err(format!("discover: {e}")))?;
                tokio::time::sleep(Duration::from_secs(3)).await;
            }

            let device = adapter.device(address)
                .map_err(|e| PyConnectionError::new_err(format!("device lookup: {e}")))?;

            if !device.is_connected().await.unwrap_or(false) {
                device.connect().await
                    .map_err(|e| PyConnectionError::new_err(format!("connect: {e}")))?;
            }

            // Wait up to 10s for the GATT tree to be resolved. bluer's
            // `services()` call will happily return an empty vec if we ask
            // too early, so we need to poll `is_services_resolved()`.
            let mut ready = false;
            for _ in 0..100 {
                if device.is_services_resolved().await.unwrap_or(false) {
                    ready = true;
                    break;
                }
                tokio::time::sleep(Duration::from_millis(100)).await;
            }
            if !ready {
                return Err(PyConnectionError::new_err(
                    "GATT services were not resolved within 10s",
                ));
            }

            // Walk every service/characteristic and build a UUID-keyed map.
            // We keep the whole map (not just write_chars) so start_notify()
            // can find characteristics added later without re-discovering.
            let services = device.services().await
                .map_err(|e| PyConnectionError::new_err(format!("services: {e}")))?;
            let mut all_chars: HashMap<Uuid, Characteristic> = HashMap::new();
            for service in services {
                let chars = service.characteristics().await
                    .map_err(|e| PyConnectionError::new_err(format!("characteristics: {e}")))?;
                for c in chars {
                    let uuid = c.uuid().await
                        .map_err(|e| PyConnectionError::new_err(format!("uuid: {e}")))?;
                    all_chars.insert(uuid, c);
                }
            }

            // Acquire write sockets. Each `write_io().await` call:
            //   1. Sends D-Bus AcquireWrite to BlueZ
            //   2. BlueZ triggers ATT MTU Exchange on the link (if not done)
            //   3. BlueZ returns a SOCK_SEQPACKET fd and the negotiated MTU
            // If we don't call this, BlueZ keeps the default 23-byte MTU and
            // every D-Bus WriteValue gets truncated — which is exactly what
            // bleak does, and why we're writing this library.
            let mut writers: HashMap<Uuid, CharacteristicWriter> = HashMap::new();
            let mut mtu: u16 = 0;
            for uuid in &write_uuids {
                let c = all_chars.get(uuid).ok_or_else(|| {
                    PyConnectionError::new_err(format!(
                        "characteristic {uuid} not present on device"
                    ))
                })?;
                let writer = c.write_io().await
                    .map_err(|e| PyConnectionError::new_err(format!("write_io: {e}")))?;
                let m = writer.mtu() as u16;
                if m > mtu {
                    mtu = m;
                }
                writers.insert(*uuid, writer);
            }

            // Fail loudly. Do not try to "degrade gracefully" — the whole
            // point of this library is that 23-byte MTU breaks the pipeline.
            if mtu < MIN_MTU {
                return Err(PyConnectionError::new_err(format!(
                    "negotiated MTU {mtu} is below the required {MIN_MTU}. \
                     Check that BlueZ is up to date and the peripheral \
                     advertises a higher MTU preference."
                )));
            }

            let mut guard = inner.lock().await;
            guard._session = Some(session);
            guard.device = Some(device);
            guard.chars = all_chars;
            guard.writers = writers;
            guard.mtu = mtu;

            Ok(mtu)
        })
    }

    /// Subscribe to notifications on `char_uuid`. The Python `callback` is
    /// invoked as `callback(char_uuid_str, bytes)` each time a notification
    /// arrives. The callback runs on the tokio reader task but grabs the
    /// GIL via `Python::with_gil` so it's safe to do Python work inside it.
    ///
    /// Internally: calls `notify_io()` on the characteristic, which is
    /// D-Bus `AcquireNotify` — another path that can trigger/confirm the
    /// MTU exchange. We also verify the notify-side MTU meets MIN_MTU.
    fn start_notify(&self, char_uuid: String, callback: PyObject) -> PyResult<()> {
        let uuid = Uuid::from_str(&char_uuid)
            .map_err(|e| PyValueError::new_err(format!("invalid UUID: {e}")))?;
        let inner = self.inner.clone();
        let runtime = self.runtime.clone();
        let uuid_str = char_uuid;

        runtime.block_on(async move {
            // Look up and clone the characteristic so we can drop the lock
            // before the potentially slow notify_io() call.
            let c = {
                let guard = inner.lock().await;
                guard.chars.get(&uuid)
                    .ok_or_else(|| {
                        PyConnectionError::new_err(format!(
                            "characteristic {uuid} not found — did you call connect() first?"
                        ))
                    })?
                    .clone()
            };

            let reader = c.notify_io().await
                .map_err(|e| PyConnectionError::new_err(format!("notify_io: {e}")))?;
            let mtu = reader.mtu() as u16;
            if mtu < MIN_MTU {
                return Err(PyConnectionError::new_err(format!(
                    "notify MTU {mtu} below required {MIN_MTU} for {uuid}"
                )));
            }

            // Spawn the reader loop on the tokio runtime. It holds the
            // CharacteristicReader and the PyObject callback for its lifetime.
            let handle = tokio::spawn(reader_loop(reader, uuid_str, callback));

            // If there's already a task for this UUID, abort it — the caller
            // is re-subscribing and we should not leak readers or double-dispatch.
            let mut guard = inner.lock().await;
            if let Some(old) = guard.reader_tasks.insert(uuid, handle) {
                old.abort();
            }
            Ok(())
        })
    }

    /// Cancel the reader task for `char_uuid` and close the underlying socket.
    fn stop_notify(&self, char_uuid: String) -> PyResult<()> {
        let uuid = Uuid::from_str(&char_uuid)
            .map_err(|e| PyValueError::new_err(format!("invalid UUID: {e}")))?;
        let inner = self.inner.clone();
        let runtime = self.runtime.clone();

        runtime.block_on(async move {
            let mut guard = inner.lock().await;
            if let Some(handle) = guard.reader_tasks.remove(&uuid) {
                handle.abort();
            }
            Ok(())
        })
    }

    /// Write one packet to the characteristic. The packet must fit in the
    /// negotiated MTU minus 3 (ATT header). Caller is responsible for
    /// chunking larger payloads — this matches `server_ble.py`'s existing
    /// BLE_CHUNK_SIZE pattern, and matters because SOCK_SEQPACKET refuses
    /// writes that exceed one packet.
    fn write(&self, char_uuid: String, data: Vec<u8>) -> PyResult<()> {
        let uuid = Uuid::from_str(&char_uuid)
            .map_err(|e| PyValueError::new_err(format!("invalid UUID: {e}")))?;
        let inner = self.inner.clone();
        let runtime = self.runtime.clone();

        runtime.block_on(async move {
            let mut guard = inner.lock().await;
            let writer = guard.writers.get_mut(&uuid).ok_or_else(|| {
                PyValueError::new_err(format!(
                    "characteristic {uuid} was not registered for writing in connect()"
                ))
            })?;

            // One syscall == one ATT packet. We intentionally don't use
            // write_all() because SOCK_SEQPACKET doesn't fragment — a single
            // oversize call returns EMSGSIZE and that's the correct failure.
            let n = writer.write(&data).await
                .map_err(|e| PyConnectionError::new_err(format!("write: {e}")))?;
            if n != data.len() {
                return Err(PyConnectionError::new_err(format!(
                    "short write: {n} of {} bytes (fragmentation not supported)",
                    data.len()
                )));
            }
            Ok(())
        })
    }

    /// Tear down: abort all reader tasks, drop writers (closing their
    /// sockets), disconnect the device, drop the session.
    fn disconnect(&self) -> PyResult<()> {
        let inner = self.inner.clone();
        let runtime = self.runtime.clone();

        runtime.block_on(async move {
            let mut guard = inner.lock().await;
            for (_, h) in guard.reader_tasks.drain() {
                h.abort();
            }
            guard.writers.clear();
            guard.chars.clear();
            if let Some(device) = guard.device.take() {
                let _ = device.disconnect().await;
            }
            guard._session = None;
            guard.mtu = 0;
            Ok(())
        })
    }

    /// The highest MTU observed on any writer/reader socket. 0 if not connected.
    #[getter]
    fn mtu(&self) -> PyResult<u16> {
        let inner = self.inner.clone();
        let runtime = self.runtime.clone();
        runtime.block_on(async move { Ok(inner.lock().await.mtu) })
    }

    /// True if the underlying bluer device reports connected.
    fn is_connected(&self) -> PyResult<bool> {
        let inner = self.inner.clone();
        let runtime = self.runtime.clone();
        runtime.block_on(async move {
            let guard = inner.lock().await;
            if let Some(device) = &guard.device {
                Ok(device.is_connected().await.unwrap_or(false))
            } else {
                Ok(false)
            }
        })
    }
}

/// Background task: drain a CharacteristicReader, dispatching each packet
/// to the Python callback. Exits when the socket returns EOF or an error,
/// or when the task is aborted by stop_notify()/disconnect().
async fn reader_loop(mut reader: CharacteristicReader, uuid: String, callback: PyObject) {
    // Size buffer to the negotiated MTU so we never silently truncate a
    // notification. bluer guarantees one packet per read() call on these
    // sockets, matching ATT semantics.
    let mut buf = vec![0u8; reader.mtu().max(512)];
    loop {
        match reader.read(&mut buf).await {
            Ok(0) => break, // EOF — peripheral unsubscribed or disconnected
            Ok(n) => {
                let data = buf[..n].to_vec();
                // Grab the GIL, call the Python callback. We clone the uuid
                // into the closure so the borrow doesn't cross await points.
                let uuid = uuid.clone();
                Python::with_gil(|py| {
                    if let Err(e) = callback.call1(py, (uuid, data)) {
                        // Don't let a Python exception kill the reader task —
                        // just print it and keep reading. This mirrors bleak's
                        // behavior where callback errors are logged, not fatal.
                        e.print(py);
                    }
                });
            }
            Err(_) => break, // Socket error — bail and let disconnect() clean up
        }
    }
}

/// Module entry point. Called once when Python imports `helios_ble`.
#[pymodule]
fn helios_ble(_py: Python, m: &PyModule) -> PyResult<()> {
    m.add_class::<HeliosBle>()?;
    m.add("MIN_MTU", MIN_MTU)?;
    Ok(())
}
