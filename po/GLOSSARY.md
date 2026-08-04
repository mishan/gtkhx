# GtkHx translation glossary

Binding for all four catalogs. Pick the listed term every time; do not vary it for
stylistic relief. If a string forces a departure, it is worth flagging rather than
improvising.

## What GtkHx is

A client for **Hotline**, a 1990s Mac chat-and-file-sharing protocol. A user connects to a
*server*, which offers public chat, private chat rooms, private messages, a news board, a
file library and user administration. A *tracker* is a directory service that lists
servers. Terminology should feel like a native desktop app of the GNOME era, not like a
literal gloss of the English.

## Product name

**GtkHx** is never translated, never declined, never split. Same for **Hotline**,
**HOPE**, **TLS**, **SOCKS**, **GIF**, **PNG**, **URL**, **MIME**, **Blowfish**,
**ChaCha20**, **RC4**, **zlib**.

## Core domain terms

| English | de | es | fr | pt |
|---|---|---|---|---|
| server | Server | servidor | serveur | servidor |
| tracker | Tracker | tracker | tracker | tracker |
| bookmark | Lesezeichen | marcador | signet | marcador |
| connection | Verbindung | conexión | connexion | ligação |
| to connect | verbinden | conectar | se connecter | ligar |
| to disconnect | trennen | desconectar | se déconnecter | desligar |
| cipher (algorithm) | Chiffre | cifrado | chiffrement | cifra |
| encryption | Verschlüsselung | cifrado | chiffrement | encriptação |
| link (hyperlink) | Link | enlace | lien | hiperligação |
| login | Anmeldung | inicio de sesión | connexion | início de sessão |
| account | Konto | cuenta | compte | conta |
| nickname | Spitzname | apodo | pseudonyme | alcunha |
| user | Benutzer | usuario | utilisateur | utilizador |
| guest | Gast | invitado | invité | convidado |
| agreement | Nutzungsbedingungen | términos de uso | conditions d'utilisation | termos de utilização |
| banner | Banner | banner | bannière | banner |
| chat | Chat | chat | discussion | conversa |
| private chat | Privatchat | chat privado | discussion privée | conversa privada |
| message (private) | Nachricht | mensaje | message | mensagem |
| broadcast | Rundnachricht | difusión | diffusion | difusão |
| news | News | noticias | actualités | notícias |
| post (noun) | Beitrag | publicación | message | mensagem |
| thread | Thread | hilo | fil | tópico |
| category (news) | Kategorie | categoría | catégorie | categoria |
| bundle (news) | Sammlung | conjunto | ensemble | conjunto |
| file | Datei | archivo | fichier | ficheiro |
| folder | Ordner | carpeta | dossier | pasta |
| upload | hochladen | subir | téléverser | enviar |
| download | herunterladen | descargar | télécharger | transferir |
| transfer | Übertragung | transferencia | transfert | transferência |
| queue | Warteschlange | cola | file d'attente | fila |
| task | Aufgabe | tarea | tâche | tarefa |
| panel | Panel | panel | panneau | painel |
| layout | Layout | disposición | disposition | disposição |
| icon | Symbol | icono | icône | ícone |
| avatar | Avatar | avatar | avatar | avatar |
| inline media | Inline-Medien | medios en línea | médias intégrés | multimédia integrado |
| voice chat | Sprachchat | chat de voz | chat vocal | conversa de voz |
| push-to-talk | Push-to-Talk | pulsar para hablar | alternat | premir para falar |
| mute / muted | stummschalten / stumm | silenciar / silenciado | couper le micro / muet | silenciar / silenciado |
| settings / preferences | Einstellungen | preferencias | préférences | preferências |
| kick | rauswerfen | expulsar | expulser | expulsar |
| ban | sperren | vetar | bannir | banir |
| fingerprint (TLS) | Fingerabdruck | huella digital | empreinte | impressão digital |
| certificate | Zertifikat | certificado | certificat | certificado |

Notes on the harder ones:

- **tracker** stays English in all four. It is a Hotline proper noun; every surviving
  server community uses the English word, and "rastreador"/"traqueur" would read as a
  mistranslation.
- **agreement** is the server's terms-of-service text shown at login. Translate the
  *function*, not the word — "Vereinbarung"/"acuerdo" reads like a contract negotiation.
- **news** here is a forum/message board, not current affairs. French "actualités" is the
  established Hotline-client rendering; keep it.
- **pt** is European Portuguese: *ficheiro* not *arquivo*, *transferir* not *baixar*,
  *ecrã* not *tela*, *utilizador* not *usuário*. **connection → *ligação***, which is the
  GNOME/KDE pt-PT term and the one that pairs with *ligar* / *desligar*; *conexão* reads
  pt-BR here. Because *ligação* then carries a lot of weight, use *hiperligação* wherever
  the referent is a clickable hyperlink rather than a network connection.
- **cipher vs encryption** are distinct and must not collapse into one word. A *cipher* is
  the named algorithm picked from a dropdown (Blowfish, ChaCha20) — it is what a bookmark
  stores a byte for. *Encryption* is the property of the connection. German especially
  should keep *Chiffre* for the first and reserve *Verschlüsselung* for the second, so that
  "Connect without encryption" and the "No cipher" option in the same dialog stay distinct.

## Japanese (ja)

Japanese joined later and does not fit the four-column table, so it gets its own section.

| English | ja | note |
|---|---|---|
| server | サーバー | long vowel mark, per MS/GNOME style |
| tracker | トラッカー | |
| bookmark | ブックマーク | |
| connection | 接続 | |
| to connect / disconnect | 接続する / 切断する | |
| login (verb) | ログイン | |
| login (account name field) | ログイン名 | |
| account | アカウント | |
| nickname | ニックネーム | |
| user | ユーザー | |
| guest | ゲスト | |
| agreement | 利用規約 | not 同意/契約 |
| banner | バナー | |
| chat | チャット | |
| private chat | プライベートチャット | |
| private message | プライベートメッセージ | |
| broadcast | 一斉通知 | |
| news | ニュース | the message board |
| post (noun) | 投稿 | |
| thread | スレッド | |
| category | カテゴリ | no long vowel mark, GNOME style |
| file | ファイル | |
| folder | フォルダー | |
| upload / download | アップロード / ダウンロード | |
| transfer | 転送 | |
| queue | キュー | "queued" → キューに追加 |
| task | タスク | |
| panel | パネル | |
| layout | レイアウト | |
| icon | アイコン | |
| avatar | アバター | |
| inline media | インラインメディア | |
| voice chat | ボイスチャット | |
| push-to-talk | プッシュトゥトーク | abbreviate as PTT where the source does |
| mute / muted | ミュート / ミュート中 | |
| settings / preferences | 設定 | both, they are the same dialog |
| kick | キック | |
| ban | アクセス禁止 | |
| cipher | 暗号方式 | the named algorithm |
| encryption | 暗号化 | the property |
| fingerprint | フィンガープリント | |
| certificate | 証明書 | |

Japanese conventions that matter here:

- **`nplurals=1`.** There is one plural form. For a `msgid_plural` entry, supply
  `msgstr[0]` only — one string that has to work for every count.
- **No spaces between Japanese words.** Do keep a space around embedded Latin script,
  format specifiers and numbers where it aids legibility (`%s を削除しますか？`).
- **Punctuation**: `。` for full stop, `、` for comma, `？` and `！` full-width. Brackets
  around UI literals are `「…」`. Where the msgid has `“%s”` use `「%s」`.
- **Register**: です・ます form for anything addressed to the user; plain noun phrases for
  buttons, labels and column headers (`削除`, `キャンセル`, not `削除します`).
- **Mnemonics**: GTK convention for CJK is to append the Latin letter in parentheses with
  the underscore inside — `_Delete` → `削除(_D)`. Keep the same letter the English used
  wherever possible.
- **Log lines** (lowercase, `\n`-terminated) stay terse; use plain form (だ/である is not
  needed — a bare noun phrase or plain verb is right), no です・ます.

## Contexts (`msgctxt`)

A few English words carry unrelated senses that no other language collapses the
same way. Those entries take a `msgctxt`, so each sense is a separate catalog
entry, plus a `#.` note saying which is which. Four exist today:

| msgctxt | msgid | which sense |
|---|---|---|
| `sound event` | Login | the chime that plays on logging in — *not* the account-name field, which keeps the uncontexted entry |
| `maturity rating` | General | an age rating: the mildest of Teen/Mature/Adult, meaning no age restriction |
| `server category` | General | a server with no particular specialism, alongside Development, Gaming, Media |
| `server details` | Identity | heading over the server's software, country, region, language and tags — *not* the user's own name and icon |

If you meet another overloaded msgid, don't paper over it in one language: add a
`trc("context", "Msgid")` at the call site (`g_dpgettext2` on the C side) so every
language gets the choice. Write the reason as a `TRANSLATORS:` comment directly
above the call — `--add-comments=TRANSLATORS:` carries those into the catalog as
`#.` notes, and a context that says two strings differ without saying how is only
half a fix.

## Mechanics — these are correctness issues, not style

1. **Format specifiers.** `%s`, `%u`, `%d`, `%1$s` must appear in the translation exactly
   as often as in the msgid. If the msgid uses positional specifiers (`%1$s`, `%2$u`),
   the translation must use positional ones too — and may reorder them, which is the
   whole point of their being there. If the msgid does *not* use them, the translation
   must not introduce them.
2. **Trailing newline.** A msgid ending in `\n` needs a msgstr ending in `\n`. Same for a
   leading `\n`. msgfmt rejects a mismatch outright.
3. **Mnemonics.** An underscore before a letter (`_Delete`, `C_reate`) marks the keyboard
   accelerator. Keep exactly one underscore, placed before a sensible letter of the
   *translated* word. Avoid colliding with other mnemonics in the same dialog where you
   can tell, but don't agonise — a collision is a minor annoyance, a missing underscore
   shows up as a literal stray character.
4. **Trailing/leading spaces and colons.** Preserve them.
5. **Ellipsis.** The sources mix `...` and `…`. Match whatever the msgid uses.
6. **Escapes.** `\"` and `\\` stay escaped. A literal `%` in a c-format string is `%%`.

## Typography per language

| | quotes | before `?` `!` `:` `;` | decimal | ellipsis |
|---|---|---|---|---|
| de | „…“ | no space | comma | … |
| es | «…» (or "…") + opening `¿` `¡` | no space | comma | … |
| fr | « … » with U+202F narrow no-break space inside | U+202F narrow no-break space | comma | … |
| pt | «…» or "…" | no space | comma | … |

Where the msgid already uses curly quotes `“%s”`, mirror them with the target language's
convention rather than copying the English marks.

## Register and voice

- **de**: address the user with *Sie*, but prefer impersonal infinitive constructions for
  instructions ("Datei auswählen" over "Wählen Sie eine Datei"). Nouns for buttons where
  natural.
- **es**: impersonal / infinitive for buttons ("Guardar", "Eliminar"); *usted* implied,
  never *tú*.
- **fr**: infinitive for buttons ("Enregistrer", "Supprimer"); *vous* elsewhere.
- **pt**: infinitive for buttons ("Guardar", "Eliminar"); *você* avoided, use verb forms
  directly.

Follow each language's GNOME/KDE localisation conventions where this glossary is silent —
those are the terms a Linux desktop user already knows.

## Error and log strings

Many msgids are lowercase, newline-terminated log lines printed into the chat pane
(`"connecting to %s\n"`). Keep them lowercase and terse in the target language too; they
are not sentences and should not be capitalised or given a full stop that the English
lacks.
