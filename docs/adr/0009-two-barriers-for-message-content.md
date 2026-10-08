# 0009 — Two independent barriers between message content and the network

**Status:** Accepted — 2026-10-05 · carries over eeemail ADR 0013

A remote image is a read receipt the sender gets without consent, plus the
reader's IP address. So:

1. The sanitizer re-emits a fixed set of tags and attributes. It drops scripts,
   styles and frames along with their content, removes every remote reference
   and counts what it removed. Links survive only for http, https and mailto.
2. The QML engine's network manager refuses every request that is not for the
   app's own resources, so a sanitizer bug alone cannot leak.

The composer works the same way in reverse: what it sends is re-emitted from
the document model through a whitelist, with a plain-text alternative. It is
never the editor's own serialisation.

## Amendment — 2026-10-07: images the message carries

Images a message carries itself (`multipart/related`, referenced as `cid:`)
are shown in place. They are the message's own stored parts, so showing them
fetches nothing and tells the sender nothing; both barriers stand.

- The sanitizer keeps an `<img>` only when its source is `cid:`, with a
  numeric width capped at 600 and its alt text; every other image is still
  replaced by its alt text and counted as blocked when remote.
- The app resolves `cid:` to an `image://cid/` provider that serves decoded
  parts from memory. It decodes only PNG, JPEG, GIF and WebP, at most 5 MiB,
  and refuses images claiming more than 40 million pixels. The QML engine's
  network manager is unchanged: it still refuses everything.
- Decoding a stranger's image is new attack surface (Qt's image plugins).
  Before this, those bytes were only ever written to a file the person chose.
- A part shown inline is not listed again as an attachment.
