# Privacy Policy

**Last updated:** 2 August 2026

Smatchet is a desktop application that runs on your own computer. It is free and
open source under the [MIT License](LICENSE), and its full source code is
available at <https://github.com/alexandrosk0/Smatchet>.

**Summary:** Smatchet has no analytics, no advertising, and no background
telemetry. It does not send your data anywhere unless you connect it to a
service yourself, or explicitly submit a bug report. Nothing is sold or shared
with data brokers.

## Data stored on your computer

Smatchet stores its working data locally, under `%LocalAppData%\Smatchet` on
Windows. This includes:

- application settings and preferences
- credentials and API keys you enter (issue-tracker tokens, AI provider keys)
- a local cache database of issues, fields, and attachments you have viewed
- diagnostic logs and, if a crash occurs, crash dumps

This data stays on your machine. Uninstalling the application does not
automatically erase this folder; you may delete it manually at any time.

## Services you connect to

Smatchet is a client for issue trackers and other services. When you configure
one, Smatchet communicates directly with that service using the credentials you
supply. Your data is handled by that provider under **their** privacy policy,
not this one:

| Service | When it is contacted |
|---|---|
| Jira / Atlassian, GitHub, Linear, Plane | when you connect an issue tracker and sync, view, or edit issues |
| OpenAI, Anthropic, DeepSeek | only if you configure an AI provider key — your prompts and the issue context you include are sent to that provider |
| Hugging Face | only if you enable Whisper dictation — used to download the speech model. Dictation itself is transcribed locally on your machine |
| Perforce / self-hosted servers | when you configure them, at the address you specify |

You may point Smatchet at self-hosted or proxied endpoints instead of the
defaults. If you configure no services, Smatchet performs no network requests
during normal use.

## Bug reports

Bug reporting is **opt-in** — it happens only when you explicitly submit a
report from within the application.

When you do, the report is sent to a relay service operated by the project
(`smatchet-bug-report-relay.smatchet.workers.dev`), which creates an issue in
the public [Smatchet GitHub repository](https://github.com/alexandrosk0/Smatchet/issues).
A submitted report may include:

- the title and description you write
- a screenshot, if you attach one (an optional censoring tool lets you redact
  regions before sending)
- a short summary of your environment (OS and application version)
- a recent tail of application logs and audit events
- a crash minidump, if the report follows a crash

Smatchet runs an automatic redaction pass over logs, audit events, and
environment data to strip credentials and tokens before sending. **This
redaction is best-effort and cannot be guaranteed to catch everything** —
please review a report's contents before submitting it, and use the screenshot
censoring tool for anything sensitive on screen.

**Bug reports become public GitHub issues.** Do not include information you are
not willing to publish. You can also point Smatchet at your own relay, or clear
the relay setting entirely, in Preferences.

## Updates

If you use the in-application updater, Smatchet contacts GitHub to check for
new releases and to download the installer. This reveals your IP address to
GitHub, as any download would. Downloaded installers are verified before they
are launched.

## What Smatchet does not do

- No analytics, tracking pixels, advertising, or usage profiling
- No background phoning-home; performance instrumentation stays on your machine
- No selling, renting, or sharing of personal data
- No account registration with the project is required to use the application

## Your choices

Because your data is stored locally and sent only to services you configure,
you remain in control of it:

- remove stored credentials and settings by clearing them in Preferences or
  deleting `%LocalAppData%\Smatchet`
- disable AI features, Whisper dictation, or the MCP server so those components
  make no network requests
- clear or replace the bug-report relay endpoint
- for data already held by a connected service (Jira, GitHub, an AI provider),
  exercise your rights directly with that provider under their policy

## Children

Smatchet is a developer tool and is not directed at children.

## Changes to this policy

Any changes to this policy will be published in this file, and its revision
history is publicly visible in the project's Git history.

## Contact

For questions about this policy, or about privacy in Smatchet, open an issue at
<https://github.com/alexandrosk0/Smatchet/issues> or contact the maintainer at
the address listed on the [project's GitHub profile](https://github.com/alexandrosk0).
