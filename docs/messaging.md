# Friends and instant messages

GtkHx's side of the friend family of fogWraith's messaging extension
(`Capabilities-Messaging.md`), as hxd-ng serves it on the classic wire
([instant-messaging.md](https://github.com/mishan/hxd-ng/blob/main/docs/instant-messaging.md)
§7): a roster of friends made by request and answer, presence, instant
messages with delivery and read receipts and typing, finding an account by
its Login, an alias for a friend, and the message requests draft (§4
there) by which someone who is not a friend writes first. The wire is
hx-libs' `hxproto::messaging`; everything GtkHx adds over it is Rust.

The bit is not yet offered by a default build: only a build with
`gtkhx-ffi/messaging` asks for it.

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

A server **serves message requests** where it confirmed bit 6 and its
login reply advertised `DATA_MAX_REQUEST_MESSAGES` among the extension's
limits (`hxsession::ServerInfo::messaging`). The requests' senders check
that too, so a server that confirmed bit 6 without the bound is sent
nothing of the draft, and its Message Requests Changed, should it send
one, is dropped.

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
A message's delivery only moves forward (sending, sent, queued or requested,
delivered, read), and only by a receipt from its recipient; once read or
failed it is let go of. The roster goes when its connection closes or logs
in again, and a message still unanswered then has failed.

Get Roster goes out once the login settles, on every connection that
confirmed bit 6; hxd-ng hands over the messages that waited, as IM Deliver
(811), after its reply. Where requests are served, the first page of Get
Message Requests goes out before it: hxd-ng hands over what waited under
an approval only to a session that understands requests, which listing
them makes it, so the order is what delivers those messages at login.
That page alone: the login has already spent most of what hxd-ng lets a
session spend in five seconds before it bans it, so a longer listing
waits there until something asks for the requests. Nothing at login sets
whether our account takes requests, but for the one time the interface
turns it on (below).

**Requests.** The roster's `requests` (`gtkhx_core::messaging::requests`)
holds whether the server serves them, how many senders wait, the senders
oldest first with their messages, the Logins we approved, how many the
server dropped, added up over the connection's listings since each first
page resets the server's count, and our account's setting as the last
listing gave it. A listing is gathered page by page, its senders merged
by Login, since a cursor is a place in a listing the server reads again
for each page (and a sender approved between pages is the later page's
approval), and replaces what is held only at its last page; a page
refused or unreadable drops it and keeps the one before. Message Requests
Changed (832) moves the count, which a listing under way when it arrives
does not replace, and owes another listing where one is being read. The listing is paced, since hxd-ng
charges a page a tenth of what a session may spend in five seconds before
it is banned: one listing at a time on a connection, each page a second
after the one before arrived, two listings five seconds apart at least, and a listing asked for
meanwhile owed once and run at the earliest moment allowed, by a GLib
timer that `send::friends` sets. The login's first page, left waiting,
is read again from the start when a listing is asked for. A cursor that never ends is cut off at a
bound on the pages, and what the listing gathered stands.

A message to someone who is not a friend is answered `Requested`
(`Delivery::Requested`) where it is held as a request. No receipt follows
one, so it stays so until the connection goes, and is not failed then.

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
| `Requests` | the requests were listed again |
| `RequestsChanged` | Message Requests Changed said how many wait |
| `Cleared { trans, requests, friend_requests }` | Clear Message Requests deleted and declined so many |
| `Im(Im)` | an instant message: its sender, id, text and when it was sent |
| `Delivery { to, guid, delivery }` | a message we sent moved on |
| `Typing { from, typing }` | someone started or stopped typing to us |
| `Found { trans, friend }` | Find User or Get User Info answered, `None` for nobody |
| `Refused { trans, request, reason, text }` | a request was refused, or answered with a reply that could not be read |

## Sending

`hxhandlers::send::friends` is the view's API, one function a request:
`roster`, `add`, `remove`, `respond`, `block`, `unblock`, `set_presence`,
`im`, `read`, `typing`, `find`, `profile` and `set_nickname`, and for
message requests `requests`, `answer_request`, `clear_requests` and
`set_takes_requests`. Each returns the trans its answer will name, and
`None` where nothing was sent, except `im`, which returns the message's
id, `typing`, whose trans is never answered, and `requests`, which asks
for a paced listing and is answered by the `Requests` event.
`set_takes_requests` goes as Set Presence, with the presence the caller
last told the server, or with none, which a server that wants one
refuses.

Ending an approval is listed again once the server takes it, since
hxd-ng pushes no Message Requests Changed for it as it does for an
approval, a deletion, a clearing and a block that ends a request. The view lists the requests
again on a Message Requests Changed where it shows them; nothing else
does. A Login is bytes as the wire carries it, from
an entry or from `login`, which encodes one the user typed; text is UTF-8,
sent as the connection's encoding has it (Mac Roman unless UTF-8 was
negotiated). The requests themselves are `hxrequest::messaging`, which the
end-to-end suite sends too.

## The interface

All of it is in `gtkhx-ui`, over `hxmodel::friends`, and none of it is there
on a connection that did not agree to bit 6: there the users panel, the user
menu and the conversations are what they always were.

- **The users panel.** Below the user list, a Friends section lists the
  roster (`friends::refresh`, after every `messaging` event and connection
  state change): a Message Requests row with how many wait, where the
  server serves requests and any do; then friend requests, with Accept and
  Decline; then friends, present ones first and absent ones dimmed; then
  the senders we approved, as Approved, and the requests we sent and the
  accounts we blocked, all dimmed. Each row's menu (right-click, Menu or
  Shift+F10) offers what fits its state: Message, Remove Friend, Block,
  Cancel Request, Unblock, End Approval, asking first before a removal, a
  block or the end of an approval. Rows are kept across changes, so an
  open menu or a focused button outlives a change to another entry. Add
  Friend… asks for a Login. Where the server serves requests, the
  section's menu offers Message…, which asks for a Login and opens a
  conversation with it, and Accept Message Requests.
- **Message requests.** The Message Requests row opens the connection's
  requests window (`requests`), one a connection, closed with it: each
  sender, oldest first, with their messages and when each was sent, and
  Approve and Reply, and Add Friend…, Delete… and Block… under a menu.
  Approving opens the sender's conversation, and so does Reply; hxd-ng
  takes a message written back to a requester as approving them. Clear…
  deletes those from about the last hour or day, or all of them, asking
  first, and says how many requests and friend requests went; the span is
  this computer's clock, so the question says "about". A line under the list
  says how many the server dropped. The requests are listed when the
  window opens and again on each `RequestsChanged` while it is open, and
  a request itself notifies nothing: the row's count is the news.
- **Text from strangers.** A sender's Login and messages are plain text in
  labels that wrap: never markup, never a link, and without the control,
  bidi and invisible formatting characters that would change how a name
  reads, make two names that look alike differ, or turn a window's title
  around; the joiners U+200C and U+200D stay (`hxmodel::friends::
  plain_name` and `plain_text`). Every name the friend family shows goes
  through it, and so do a friend request's note and every instant message
  received, a friend's too.
- **Taking requests.** Accept Message Requests shows our account's setting
  as the server's last listing gave it, and changing it sends Set Presence
  with the presence the user list's away state last told the server,
  which every server takes. It is on by default: the first time GtkHx
  lists an account's requests and finds it off, it turns it on, at most
  once a connect (`hxmodel::friends::turn_requests_on`); once the
  server's answer or a later listing says it is on, GtkHx remembers it
  did, by server address and Login, in the settings
  (`messaging.requests_set`). Once remembered, GtkHx never turns it on
  again, since the user may have turned it off. A change the user makes
  is remembered as it is sent. The automatic one carries no presence, so
  it leaves ours alone where the server allows that; where it does not,
  its refusal is told to nobody and it is tried again at the next
  connect.
- **The user menu.** Add Friend… and Block… on a user of the user list ask
  for the user's Login: the classic list carries none, and the roster no
  uid, so a user is never matched to a friend, by nick or otherwise.
- **Conversations.** The private-message tab is the conversation, keyed by
  the friend's Login (`pm::Peer::Login`), so it reaches a friend who is
  offline. What is typed goes as an instant message on a connection that
  agreed to bit 6, to a friend, or to anyone where the server serves
  requests, and is otherwise noted as not sent; a conversation opened
  from the user list is keyed by uid and goes as a classic private
  message, as before. Each message we send is marked as it gets further
  (sending, sent or waiting, waiting for approval, delivered, read, not
  delivered), the friend's typing is shown under the transcript, and
  lapses ten seconds after their last Started, as on Hotline-ng; ours is
  told, to a friend alone, as it starts and stops, and stopped when the
  tab closes. A message
  received is marked read once its conversation is on screen in the window
  that has the focus.
- **Notification.** An instant message, and a friend request, is announced,
  chimed and notified as a private message is, under the same setting; the
  notification of a message opens its conversation. What waited as a
  request is handed over as instant messages too, drawn into the sender's
  conversation, so their messages go unannounced for a few seconds after
  we approve them, write back to them, or become their friend
  (`hxmodel::friends::Hush`), once: the request is let go of then
  (`Requests::released`), whether or not a listing follows.
- **Presence** follows the away state the user list shows of us: when our
  own row's away bit changes, Set Presence says Away or Online.
- **Refusals** are shown as every refusal the server explains is, a toast
  and the error sound, in our words where the extension gives a reason:
  among them, a message to someone who takes no requests (`NotFriends`),
  too many waiting for them (`QueueFull`, as hxd-ng's full request comes),
  too many asked too fast (`RateLimited`), nobody by that Login
  (`AccountNotFound`), and an answer to a request that no longer waits.

## What hxd-ng does that a client sees

- Find User answers only an exact Login, and an account that blocked the
  caller reads as one nobody holds (`AccountNotFound`), on every request
  that names it.
- User Search (823) is not served, so search is Find User.
- A message's id is taken once: the server drops a second send under it.
- Message requests (829–832) reach only a session that understands them:
  one that sent `DATA_TAKES_REQUESTS` or listed them, which GtkHx does at
  every login where they are served. A message such a session sends to
  someone who is not a friend is answered `Requested` where the recipient's
  account takes requests, and refused `NotFriends` where it takes none.
  Whether an account takes them is the account's, on every client: GtkHx
  sends it once by default and otherwise only when the user changes it.
- Requests are answered alike whatever becomes of them: one from a sender
  whose request was deleted, whose approval was ended, or whose friendship
  was removed is answered `Requested` and kept nowhere, for days.
- Under an approval every message is answered `OfflineQueued`, and its
  Delivered receipts are not passed on.

## Tests

The model's rules are unit tests in `gtkhx-core`, and the panel's order,
presence, typing, the requests' row, what is shown of a stranger's text,
the quiet after an approval and when taking requests is turned on in
`hxmodel::friends`; what the pushes and
replies make of the model and the signal, and what each sender writes, are
`hxhandlers`' (`recv::friends`, `send::friends`), against a real connection
struct. `hxnet`'s session tests check the routing. `hx-e2e`'s `friends`
suite runs against the rig's hxd-ng, whose `friend-*` accounts take part
(`tests/COMPOSE.md`): two accounts becoming friends and seeing each other's
presence and alias, a message with both receipts and typing, a block and
its lifting, Find User, and every server in the rig taking the offer with
only a taking-part account confirmed. Its message requests tests make
their accounts fresh each run through the rig's `requests-admin`, since
what a request leaves on hxd-ng outlives a run: the login reply's bound,
a stranger's message answered `Requested`, listed and told by Message
Requests Changed, approved and delivered, an approval's message handed
over at Get Roster to a session that listed first, an approval ended, a
request deleted and one approved by a reply, a clearing's counts, and an
account that takes none answering `NotFriends`.

## Open

- Turning the offer on in a default build.
- An instant message carries text alone: a friend's conversation has no
  attachment button.
- Of the login reply's messaging limits (`hxproto::messaging::Limits`),
  only the request bound is read, as whether requests are served: a
  message past the server's other bounds is refused by it rather than held
  back here.
