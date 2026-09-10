//! C ABI for `hxproto::dispatch`, the receive-side opcode router.

use hxproto::dispatch::route;

/// `hx_recv_handler_kind hx_recv_route (guint32 opcode)` — route an opcode to
/// its `HandlerKind` discriminant, the integer the C `hx_recv_handler_kind`
/// enum (hotline_proto.h) mirrors. C `hx_dispatch_frame` switches on it instead
/// of the in-line opcode `switch`.
#[no_mangle]
pub extern "C" fn hx_recv_route(opcode: u32) -> i32 {
    route(opcode) as i32
}
