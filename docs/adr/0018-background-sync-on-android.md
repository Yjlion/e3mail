# 0018 — Android: background sync through a foreground service, if asked

**Status:** Accepted — 2026-10-16. Supersedes, for Android, the part of
[ADR 0016](0016-android.md) that says sync happens only while e3mail runs.

## Context
ADR 0016 left a background service as a separate decision. Without one,
Android freezes e3mail's process soon after it leaves the front, so:
- mail arrives only when e3mail is opened;
- a phone is never reachable by the person's other devices, so their changes
  reach it only through the mailbox fallback (ADR 0010), with the server
  holding mail until the phone has it.

Qt also pauses its own event loop in the background unless the manifest
says otherwise, so a service alone would keep a process whose sync is
frozen.

## Decision
- **Opt in, per device.** Settings, under Notifications, has "Keep syncing
  in the background" (Android only, off by default, in `app.ini`). It says
  what it costs: a notification for as long as it is on, and battery.
- **A foreground service that does no work of its own**
  (`E3SyncService.java`). It shows the notification, with Stop, and keeps
  the process alive. The mail connection (IMAP IDLE), device sync over Iroh
  and the mailbox fallback are the same code as in the front, in the same
  process. Stop turns the setting off.
- **Qt keeps running in the background** (`android.app.background_running`
  in the manifest). Without the service, Android freezes the cached process
  anyway, so this costs nothing then.
- **Type `specialUse`**, with a subtype that says what it is for. `dataSync`
  was the alternative, but Android 15 limits it to six hours a day, and a
  mail client that should stay reachable runs longer than that. Google Play
  asks for a declaration of `specialUse`, made in Play Console by a person.
  If Play refuses it, fall back to `dataSync` and stop at its timeout.
- **Started only from the front** (at launch and when the setting is turned
  on), as Android requires. **It ends with the task** (`stopWithTask`):
  swiping e3mail away from the recent apps stops syncing, and the setting
  says so. It is not restarted by Android (`START_NOT_STICKY`), because
  without the app there is nothing in the process to sync.
- **No WorkManager.** Waking every fifteen minutes would fetch mail, but a
  peer would still be unreachable almost all the time.

## Consequences
- With it on, a phone is a peer like a desktop: other devices' changes reach
  it within seconds, and the server can let go of mail sooner.
- The notification never goes away while it is on. That is Android's rule
  for foreground services, and also honest.
- A Play Console declaration is needed before a release with it reaches
  testers.
