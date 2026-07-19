#!/usr/bin/env -S gjs -m
/* Import each module and report failures.
 *
 * A bare `gjs -c "import(...)"` never terminates -- the dynamic import keeps
 * the main loop alive -- so this exits explicitly instead.
 *
 * app.js is deliberately not checked here: importing it would construct the
 * application and open a window. `make run` covers it. */

import System from "system";
import GLib from "gi://GLib";

const HERE = GLib.path_get_dirname(import.meta.url.replace("file://", ""));
const MODULES = ["config.js", "daemon.js", "audio.js", "sessions.js"];

let failures = 0;
for (const m of MODULES) {
    try {
        await import(`file://${HERE}/src/${m}`);
        print(`ok   src/${m}`);
    } catch (e) {
        print(`FAIL src/${m}: ${e.message}`);
        failures++;
    }
}
System.exit(failures ? 1 : 0);
