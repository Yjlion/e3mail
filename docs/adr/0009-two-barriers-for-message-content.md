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
