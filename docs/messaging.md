# Friends and instant messages

GtkHx's side of the friend family of fogWraith's messaging extension
(`Capabilities-Messaging.md`), as hxd-ng serves it on the classic wire
([instant-messaging.md](https://github.com/mishan/hxd-ng/blob/main/docs/instant-messaging.md)
§7): a roster of friends made by request and answer, presence, instant
messages with delivery and read receipts and typing, finding an account by
its Login, and an alias for a friend. The wire is hx-libs'
`hxproto::messaging`; everything GtkHx adds over it is Rust.

What is built is everything beneath the user interface. The bit is not yet
offered by a default build, since nothing shows what it would bring.

## Negotiation

The friend family is capability bit 6 (`CAP_MESSAGING`). hxnet offers it at
login (`offered` in `hxnet::ffi`) in a build with hxnet's `messaging`
feature, which `gtkhx-ffi/messaging` turns on; no meson option does yet.
C's own offer in `network.c` is unchanged. Bit 8, the messenger-only
session, is never offered: GtkHx is a whole client.

A server confirms bit 6 only to an account that takes part, so a period
server, or an account without the access bit (`ACCESS_MESSAGING`, 58), sees
the login it always saw. Every sender in `hxhandlers::send::friends` checks
the confirmed bit and sends nothing without it, and every push is dropped
on a connection that did not confirm it.

## Routing

Nothing in `rcv.c` knows the extension: its opcodes route nowhere there
(`hxproto::dispatch` has no kind for them), and C would show each as an
unknown header. So hxnet's session actor (`hxnet::session`) hands them over
as the session's own events rather than as frames:

- what the server pushes (801, 804, 809, 811, 812, 813, and the rest of
  800–834) as `hxsession::Event::Unhandled`, where the server agreed to bit
  6; a server that did not is still shown what it sends as any unknown
  transaction;
- the reply to a request the consumer sent with an opcode in 800–834 as
  `hxsession::Event::Reply`. The actor notes each such request's trans as
  it is written, all but typing (813), which is never answered.

Both reach `hx_recv_session_event`, which passes them to
`hxhandlers::recv::friends`. A reply is matched by its trans to what asked
for it, as news replies are.

## The model

`gtkhx_core::messaging` holds, per connection, a `Roster`: every entry the
server listed or pushed, keyed by Login, our own presence as the server
last took it, and how far each message we sent has got. It is the
server's record, not a guess: Get Roster's reply replaces it, a Roster
Entry (801) replaces the one entry whole (or, `Removed`, ends it), a
Friend Request (804) is held as `PendingIn` with its note, and a Presence
Changed (809) moves only the presence and status of an entry already held.
A message's delivery only moves forward (sending, sent or queued,
delivered, read), and only by a receipt from its recipient; once read or
failed it is let go of. The roster goes when its connection closes or logs
in again, and a message still unanswered then has failed.

Get Roster goes out once the login settles, on every connection that
confirmed bit 6; hxd-ng hands over the messages that waited, as IM Deliver
(811), after its reply.

An instant message that arrives is acknowledged as delivered at once, a
receipt nobody asked for whose refusal is never shown; read is the view's
to say, once it is shown.

hxd-ng answers Get Roster between the events it hands the session, never
during them, so what it pushed before the reply is no newer than the
reply's snapshot, and the reply replaces it.

## The signal

`GtkhxSession`'s `messaging` signal carries the connection and a
`gtkhx_core::messaging::Event`, a Rust value valid for the emit, so it is
heard from Rust through `gtkhx_core::session::connect_messaging` and never
from C:

| Event | When |
|---|---|
| `Roster` | the roster was read again |
| `Changed { login }` | one entry arrived, changed or went, or a friend's presence moved |
| `Request { login }` | someone asks to be a friend |
| `Presence` | the server took our presence |
| `Im(Im)` | an instant message: its sender, id, text and when it was sent |
| `Delivery { to, guid, delivery }` | a message we sent moved on |
| `Typing { from, typing }` | someone started or stopped typing to us |
| `Found { trans, friend }` | Find User or Get User Info answered, `None` for nobody |
| `Refused { trans, request, reason, text }` | a request was refused, or answered with a reply that could not be read |

## Sending

`hxhandlers::send::friends` is the view's API, one function a request:
`roster`, `add`, `remove`, `respond`, `block`, `unblock`, `set_presence`,
`im` (which returns the message's id), `read`, `typing`, `find`, `profile`
and `set_nickname`. Each returns the trans its answer will name, and
`None` where nothing was sent. A Login is bytes as the wire carries it, from
an entry or from `login`, which encodes one the user typed; text is UTF-8,
sent as the connection's encoding has it (Mac Roman unless UTF-8 was
negotiated). The requests themselves are `hxrequest::messaging`, which the
end-to-end suite sends too.

## What hxd-ng does that a client sees

- Find User answers only an exact Login, and an account that blocked the
  caller reads as one nobody holds (`AccountNotFound`), on every request
  that names it.
- User Search (823) is not served, so search is Find User.
- A message's id is taken once: the server drops a second send under it.
- Message requests (829–832) reach only a client that sent
  `DATA_TAKES_REQUESTS` or listed them. GtkHx does neither, so an instant
  message to someone who is not a friend is refused `NotFriends`; a push of
  that draft that arrives anyway is dropped.

## Tests

The model's rules are unit tests in `gtkhx-core`; what the pushes and replies
make of the model and the signal, and what each sender writes, are
`hxhandlers`' (`recv::friends`, `send::friends`), against a real connection
struct. `hxnet`'s session tests check the routing. `hx-e2e`'s `friends`
suite runs against the rig's hxd-ng, whose `friend-*` accounts take part
(`tests/COMPOSE.md`): two accounts becoming friends and seeing each other's
presence and alias, a message with both receipts and typing, a block and
its lifting, Find User, and every server in the rig taking the offer with
only a taking-part account confirmed.

## Open

- The user interface: the users panel, the private-message conversation,
  the context menus and notifications, and turning the offer on.
- The login reply's messaging limits (`hxproto::messaging::Limits`) are
  not read: `hxsession::ServerInfo` carries none, so a message past the
  server's bound is refused by it rather than held back here.
