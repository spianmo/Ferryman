/* Ferryman PWA service worker.
 *
 * Goals:
 * - Never cache Vite dev modules (avoid stale refresh in development).
 * - In production, cache only explicit static assets.
 * - Keep navigations network-first with offline shell fallback.
 */

const CACHE_NAME = "ferryman-pwa-v3";
const CORE_ASSETS = ["/", "/index.html", "/manifest.webmanifest", "/icon-192.png", "/icon-512.png"];
const DEV_HOSTNAMES = new Set(["localhost", "127.0.0.1", "0.0.0.0", "::1"]);
const DEV_PORTS = new Set(["5173", "4173"]);

const IS_DEV_RUNTIME =
  (DEV_HOSTNAMES.has(self.location.hostname) || self.location.hostname.endsWith(".local")) &&
  DEV_PORTS.has(self.location.port);

function isViteInternalPath(pathname) {
  return pathname.startsWith("/@") || pathname.startsWith("/src/") || pathname.startsWith("/node_modules/");
}

function isCacheableStaticPath(pathname) {
  return (
    pathname.endsWith(".js") ||
    pathname.endsWith(".mjs") ||
    pathname.endsWith(".css") ||
    pathname.endsWith(".ico") ||
    pathname.endsWith(".png") ||
    pathname.endsWith(".jpg") ||
    pathname.endsWith(".jpeg") ||
    pathname.endsWith(".svg") ||
    pathname.endsWith(".webp") ||
    pathname.endsWith(".avif") ||
    pathname.endsWith(".woff") ||
    pathname.endsWith(".woff2") ||
    pathname.endsWith(".ttf") ||
    pathname.endsWith(".otf") ||
    pathname.endsWith(".webmanifest")
  );
}

async function deleteAllCaches() {
  const keys = await caches.keys();
  await Promise.all(keys.map((key) => caches.delete(key)));
}

self.addEventListener("install", (event) => {
  if (IS_DEV_RUNTIME) {
    event.waitUntil(self.skipWaiting());
    return;
  }

  event.waitUntil(
    caches
      .open(CACHE_NAME)
      .then((cache) => cache.addAll(CORE_ASSETS))
      .then(() => self.skipWaiting())
  );
});

self.addEventListener("activate", (event) => {
  if (IS_DEV_RUNTIME) {
    event.waitUntil(
      deleteAllCaches()
        .then(() => self.registration.unregister())
        .then(() => self.clients.claim())
    );
    return;
  }

  event.waitUntil(
    caches
      .keys()
      .then((keys) =>
        Promise.all(keys.map((key) => (key === CACHE_NAME ? Promise.resolve() : caches.delete(key))))
      )
      .then(() => self.clients.claim())
  );
});

self.addEventListener("fetch", (event) => {
  if (IS_DEV_RUNTIME) {
    return;
  }

  const req = event.request;
  if (req.method !== "GET") return;

  const url = new URL(req.url);
  if (url.origin !== self.location.origin) return;

  // Never cache authenticated/control APIs. Session is carried via headers, and caching
  // responses by URL would be unsafe (wrong-user response reuse).
  if (url.pathname.startsWith("/api/") || url.pathname.startsWith("/ws/") || req.headers.has("X-Session-Token")) {
    event.respondWith(fetch(req));
    return;
  }

  // Never touch Vite dev/HMR module paths (defensive; dev should be disabled already).
  if (isViteInternalPath(url.pathname)) {
    return;
  }

  if (req.mode === "navigate") {
    event.respondWith(
      fetch(req)
        .then((res) => {
          if (res && res.status === 200) {
            const copy = res.clone();
            caches.open(CACHE_NAME).then((cache) => cache.put("/index.html", copy));
          }
          return res;
        })
        .catch(async () => {
          const cached = await caches.match("/index.html");
          return cached || Response.error();
        })
    );
    return;
  }

  if (!isCacheableStaticPath(url.pathname)) {
    return;
  }

  // Stale-while-revalidate for static assets.
  event.respondWith(
    caches.open(CACHE_NAME).then(async (cache) => {
      const cached = await cache.match(req);
      const networkPromise = fetch(req)
        .then((res) => {
          if (res && res.status === 200) {
            cache.put(req, res.clone());
          }
          return res;
        })
        .catch(() => null);

      if (cached) {
        void networkPromise;
        return cached;
      }

      const networkRes = await networkPromise;
      return networkRes || Response.error();
    })
  );
});
