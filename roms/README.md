Drop your own legally-owned ROM/ISO dumps here, named exactly:

- `baserom.us.z64` — Super Mario 64 (US), needed for `SM64Challenge`
- `jak1.iso` — Jak & Daxter: The Precursor Legacy, needed for `JakChallenge`

Neither file is redistributed with this project (see `.gitignore` — `*.z64`/`*.iso`
are never tracked). `just build-sm64-linux`/`build-sm64-windows`/`build-jak1-linux`
(and `mad2launcher`'s "Build Games" page, which drives the same steps via
`mad2buildgames`) symlink whichever file they need from here into the right
`extern/*` subproject automatically — you only need one copy of each, dropped
in this one folder, not one per subproject.
