# Security Policy

## Reporting a vulnerability

Please do not open a public issue or pull request for a security vulnerability in the PPUC applications (`ppuc-pinmame`, `ppuc-menu`, `ppuc-backbox`).

Report it by email to **git@kalki.de** instead. Include what you found, the version or commit it applies to,
and the smallest input or steps that reproduce it. A proposed patch is welcome but not required.

You will get a reply as soon as the report has been read. This is a volunteer-maintained project, so there is
no guaranteed response time, but security reports are handled before other work. Please give the fix a
reasonable time to be released before disclosing the problem publicly.

## Scope

In scope is how the applications handle data they do not control: files in a game folder (configuration, Lua
rules, media), firmware images, data received from IO boards, and network input to `ppuc-backbox`. Bugs that
live entirely in a dependency (PinMAME, the VPX plugins, libdmdutil, SDL and others) should go to that project.
[libppuc](https://github.com/PPUC/libppuc) has its own policy.

Only the latest release and the default branch are supported; fixes are not backported to older versions.
