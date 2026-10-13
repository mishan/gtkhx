//! Just enough of the hotline-ng wire to set up what the classic wire is
//! then checked against: a JSON request and its reply over a WebSocket,
//! and an upload over HTTP with the session's bearer. hxd-ng's
//! `docs/hotline-ng.md` is the wire; this speaks only the plain-text
//! frames and the few requests the suites use.

use std::io::{BufRead, BufReader, Read, Write};
use std::net::TcpStream;
use std::time::Duration;

use serde_json::{json, Value};

pub struct Ng {
    addr: String,
    ws: BufReader<TcpStream>,
    next: u64,
    bearer: String,
}

impl Ng {
    /// Log `login` in with `password` on the ng wire at `addr`.
    pub fn login(addr: &str, login: &str, password: &str) -> Result<Ng, String> {
        let mut s = TcpStream::connect(addr).map_err(|e| format!("{addr}: {e}"))?;
        s.set_read_timeout(Some(Duration::from_secs(10))).ok();
        // A fixed key: nothing here checks the server's answer to it.
        write!(
            s,
            "GET / HTTP/1.1\r\nHost: {addr}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\
             Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n"
        )
        .map_err(|e| e.to_string())?;
        let mut ws = BufReader::new(s);
        let status = head(&mut ws)?;
        if !status.contains(" 101 ") {
            return Err(format!("{addr}: no upgrade: {status}"));
        }
        let mut ng = Ng {
            addr: addr.to_owned(),
            ws,
            next: 1,
            bearer: String::new(),
        };
        let ok = ng.ok("login", json!({ "login": login, "password": password }))?;
        ng.bearer = format!(
            "Bearer {}.{}",
            ok["session"].as_str().unwrap_or_default(),
            ok["token"].as_str().unwrap_or_default()
        );
        Ok(ng)
    }

    /// The `ok` of `req`, or what refused it.
    pub fn ok(&mut self, req: &str, params: Value) -> Result<Value, String> {
        let id = self.next;
        self.next += 1;
        let frame = json!({ "id": id, "req": req, "params": params }).to_string();
        self.send(frame.as_bytes())?;
        loop {
            let v: Value = serde_json::from_slice(&self.text()?).map_err(|e| e.to_string())?;
            if v["reply"] == id {
                return v.get("ok").cloned().ok_or_else(|| format!("{req}: {v}"));
            }
        }
    }

    /// POST `body` to `path` on the ng port with the session's bearer: the
    /// reply's JSON, or its status line.
    pub fn post(&self, path: &str, mime: &str, body: &[u8]) -> Result<Value, String> {
        let mut s = TcpStream::connect(&self.addr).map_err(|e| e.to_string())?;
        s.set_read_timeout(Some(Duration::from_secs(10))).ok();
        write!(
            s,
            "POST {path} HTTP/1.1\r\nHost: {}\r\nAuthorization: {}\r\nContent-Type: {mime}\r\n\
             Content-Length: {}\r\nConnection: close\r\n\r\n",
            self.addr,
            self.bearer,
            body.len()
        )
        .and_then(|_| s.write_all(body))
        .map_err(|e| e.to_string())?;
        let mut r = BufReader::new(s);
        let status = head(&mut r)?;
        let mut rest = Vec::new();
        r.read_to_end(&mut rest).map_err(|e| e.to_string())?;
        if !status.contains(" 201 ") && !status.contains(" 200 ") {
            return Err(format!("{path}: {status}"));
        }
        serde_json::from_slice(&rest).map_err(|e| format!("{path}: {e}"))
    }

    /// Send `text` as one masked text frame, as a client must.
    fn send(&mut self, text: &[u8]) -> Result<(), String> {
        let mask = [0x5a, 0xc3, 0x1e, 0x77];
        let mut f = vec![0x81];
        match text.len() {
            n if n < 126 => f.push(0x80 | n as u8),
            n if n <= 0xffff => {
                f.push(0x80 | 126);
                f.extend_from_slice(&(n as u16).to_be_bytes());
            }
            n => {
                f.push(0x80 | 127);
                f.extend_from_slice(&(n as u64).to_be_bytes());
            }
        }
        f.extend_from_slice(&mask);
        f.extend(text.iter().enumerate().map(|(i, b)| b ^ mask[i % 4]));
        self.ws.get_mut().write_all(&f).map_err(|e| e.to_string())
    }

    /// The next text message, whole; pings are answered on the way.
    fn text(&mut self) -> Result<Vec<u8>, String> {
        let mut message = Vec::new();
        loop {
            let mut h = [0u8; 2];
            self.ws.read_exact(&mut h).map_err(|e| e.to_string())?;
            let len = match h[1] & 0x7f {
                126 => {
                    let mut b = [0u8; 2];
                    self.ws.read_exact(&mut b).map_err(|e| e.to_string())?;
                    u16::from_be_bytes(b) as usize
                }
                127 => {
                    let mut b = [0u8; 8];
                    self.ws.read_exact(&mut b).map_err(|e| e.to_string())?;
                    u64::from_be_bytes(b) as usize
                }
                n => n as usize,
            };
            let mut data = vec![0u8; len];
            self.ws.read_exact(&mut data).map_err(|e| e.to_string())?;
            match h[0] & 0x0f {
                0x8 => return Err("the ng socket closed".into()),
                0x9 => {
                    let pong = [&[0x8a, 0x80 | data.len() as u8, 0, 0, 0, 0][..], &data].concat();
                    self.ws
                        .get_mut()
                        .write_all(&pong)
                        .map_err(|e| e.to_string())?;
                }
                0xa => {}
                _ => {
                    message.extend_from_slice(&data);
                    if h[0] & 0x80 != 0 {
                        return Ok(message);
                    }
                }
            }
        }
    }
}

/// Log out, so the session doesn't stay on the server's user list as a
/// detached one for its grace period.
impl Drop for Ng {
    fn drop(&mut self) {
        let _ = self.ok("logout", serde_json::json!({}));
    }
}

/// Read an HTTP response's head; its status line.
fn head(r: &mut impl BufRead) -> Result<String, String> {
    let mut status = String::new();
    r.read_line(&mut status).map_err(|e| e.to_string())?;
    loop {
        let mut line = String::new();
        r.read_line(&mut line).map_err(|e| e.to_string())?;
        if line.trim().is_empty() {
            return Ok(status.trim().to_owned());
        }
    }
}
