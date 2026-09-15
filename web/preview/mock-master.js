/*
 * Simulated master node for the hosted preview of the admin page.
 * Mirrors the rules in firmware/node/main/web_admin.c so the flows can be
 * tried without hardware. Never shipped to a board.
 */
(function () {
  const KEY = "lg-preview-master";
  const fresh = () => ({
    configured: false, grid_name: "", password: "", timezone: "",
    time_offset_s: 0, time_quality: 0, token: "", csrf: "",
    failures: 0, locked_until: 0, boot_ms: Date.now(),
  });
  const load = () => { try { return JSON.parse(localStorage.getItem(KEY)) || fresh(); } catch (e) { return fresh(); } };
  const save = () => { try { localStorage.setItem(KEY, JSON.stringify(m)); } catch (e) { /* storage blocked */ } };
  let m = load();

  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const hex = () => Array.from(crypto.getRandomValues(new Uint8Array(16)), (b) => b.toString(16).padStart(2, "0")).join("");
  const fail = (msg) => { throw new Error(msg); };
  const nowS = () => Math.floor(Date.now() / 1000);
  const jitter = (base, spread) => base + Math.round((Math.random() * 2 - 1) * spread);

  function newSession() {
    m.token = hex();
    m.csrf = hex();
    save();
    return { ok: true, csrf: m.csrf };
  }

  window.LG_MOCK_API = async function (path, body, csrf) {
    await sleep(150 + Math.random() * 200);
    const loggedIn = !!m.token;

    switch (path) {
      case "/api/state":
        return { configured: m.configured, logged_in: loggedIn, csrf: loggedIn ? m.csrf : "", grid_name: m.configured ? m.grid_name : "" };

      case "/api/setup": {
        if (m.configured) fail("LocalGrid is already set up. Log in instead.");
        const name = (body.grid_name || "").trim();
        if (!name || name.length > 32) fail("Grid name must be 1 to 32 characters.");
        if (!body.password || body.password.length < 12 || body.password.length > 64) fail("Password must be 12 to 64 characters.");
        if (!(body.unix_ms > 1700000000000)) fail("The browser time or time zone looks invalid.");
        await sleep(900);   // the real master hashes the password for about 0.9 s
        Object.assign(m, {
          configured: true, grid_name: name, password: body.password, timezone: body.timezone || "",
          time_offset_s: Math.round(body.unix_ms / 1000) - nowS(), time_quality: 2,
        });
        return newSession();
      }

      case "/api/login": {
        if (!m.configured) fail("LocalGrid is not set up yet.");
        if (m.locked_until > Date.now()) {
          fail(`Too many failed attempts. Try again in ${Math.ceil((m.locked_until - Date.now()) / 1000)} seconds.`);
        }
        await sleep(900);
        if (body.password !== m.password) {
          m.failures += 1;
          if (m.failures >= 5) {
            const shift = m.failures - 5;
            m.locked_until = Date.now() + Math.min(30000 * 2 ** shift, 300000);
          }
          save();
          fail("Wrong password.");
        }
        m.failures = 0;
        m.locked_until = 0;
        return newSession();
      }

      case "/api/logout":
        m.token = "";
        m.csrf = "";
        save();
        return { ok: true };

      case "/api/status": {
        if (!loggedIn) fail("Log in to see grid status.");
        const uptime = Math.floor((Date.now() - m.boot_ms) / 1000);
        return {
          grid_name: m.grid_name, timezone: m.timezone, node: 0, node_name: "MAIN", boot: 6,
          uptime_s: uptime, grid_time: m.time_quality ? nowS() + m.time_offset_s : 0, time_quality: m.time_quality,
          heap_free: jitter(63300, 900), heap_min: 56784, handhelds: 1,
          links: [
            { node: 1, up: true, rssi: jitter(-58, 3), age_ms: Math.round(Math.random() * 2000) },
            { node: 2, up: true, rssi: jitter(-73, 4), age_ms: Math.round(Math.random() * 2000) },
          ],
          devices: [],
        };
      }

      case "/api/time": {
        if (!loggedIn) fail("Log in to set the time.");
        if (csrf !== m.csrf) fail("Request blocked. Reload the page and try again.");
        if (!(body.unix_ms > 1700000000000)) fail("A valid time is required.");
        m.time_offset_s = Math.round(body.unix_ms / 1000) - nowS();
        m.time_quality = 2;
        if (body.timezone) m.timezone = body.timezone;
        save();
        return { ok: true };
      }

      default:
        fail(`Unknown endpoint ${path}.`);
    }
  };

  /* Preview controls, styled with the page's theme variables. */
  function addBanner() {
    const bar = document.createElement("div");
    bar.setAttribute("role", "note");
    bar.style.cssText = "max-width:760px;margin:0 auto 14px;padding:10px 14px;border:1px dashed var(--warn);" +
      "border-radius:var(--radius);color:var(--warn);font-size:14px;display:flex;flex-wrap:wrap;gap:8px;align-items:center";
    bar.innerHTML = '<span style="flex:1;min-width:220px"><strong>Preview.</strong> A simulated master node answers this page; ' +
      'no ESP32 is involved. The real page is at http://192.168.4.1/ on LG-MAIN.</span>';
    const btn = (label, onClick) => {
      const b = document.createElement("button");
      b.className = "ghost";
      b.type = "button";
      b.textContent = label;
      b.style.cssText = "padding:6px 10px;font-size:13px;border-color:var(--warn);color:var(--warn)";
      b.addEventListener("click", onClick);
      bar.appendChild(b);
    };
    btn("Simulate power loss", () => { m.time_quality = 0; m.boot_ms = Date.now(); save(); location.reload(); });
    btn("Restart setup", () => { try { localStorage.removeItem(KEY); } catch (e) { /* ignore */ } location.reload(); });
    document.body.insertBefore(bar, document.body.firstChild);
  }
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", addBanner);
  else addBanner();
})();
