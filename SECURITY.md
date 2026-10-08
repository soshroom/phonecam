# Security Policy

PhoneCam is an early-stage LAN-only project. The Android control server and H.264 stream currently do not provide authentication or transport encryption. This is a documented design limitation, not a supported internet-facing deployment mode.

## Supported versions

Security fixes are currently made only on the latest `main` branch and the newest automated release.

## Reporting a vulnerability

Please do not publish sensitive vulnerability details in a public issue.

If GitHub private vulnerability reporting is enabled for this repository, use the repository's **Security -> Report a vulnerability** flow.

If private reporting is not available, open a minimal issue asking for a private contact channel without including exploit details, credentials, private addresses, or other sensitive information.

Useful reports include:

- affected PhoneCam build or commit
- affected Android or Windows version
- reproduction conditions
- impact
- whether the issue is reachable only from the LAN or can cross a trust boundary

## Deployment assumptions

PhoneCam assumes a trusted local network. Do not expose Android ports `8080` or `8554` to the public internet and do not configure router port forwarding for them.

The Windows decoded-frame bridge uses `127.0.0.1:8765` and is intended to remain loopback-only.
