/* What this build is. Written by tools/wasm/build_web.py into the staged directory, so the same page sources
 * make both the developer build (served from build/wasm/web) and the redistributable one; the copy here is the
 * developer build, so serving web/ straight off a disk still works.
 *
 *   redist     true on a redistributable build: a top-right button opens the controls menu, and there is no
 *               level select, no debug switches, no log console and no scripting hook. The module is built
 *               without the SABER_* environment and debug-key exports, so those are gone from the binary too
 *               and not merely hidden by the page.
 *   label      the panel's subtitle, so a redist build is not claiming to be a developer build.
 *   port       the port the developer build's serve target uses (0 = none).
 */
window.SABER_BUILD = { redist: false, label: 'developer build', port: 8009 };
