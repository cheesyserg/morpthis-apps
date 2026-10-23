#ifndef TLS_H
#define TLS_H

#include <stdbool.h>
#include <stddef.h>

// TLS for http.c, on top of the OpenSSL the device already ships.
//
// The rootfs carries /usr/lib/libssl.so.1.1 and libcrypto.so.1.1 (OpenSSL
// 1.1.1f), because the stock player links libcurl. The cross toolchain has no
// headers for them and nothing to link against, so they are opened at run time
// with dlopen() and the two dozen functions needed are looked up by name. That
// is the only way to use a library present on the target and absent from the
// build host, and it has a second virtue: a device whose rootfs lacks them
// still runs the app, it just cannot open an https:// URL and says so.
//
// Verification is on by default: the certificate chain is checked against a CA
// bundle and the hostname against the certificate. A TLS connection that
// checks neither is a slower plain socket with a false sense of security, and
// a Subsonic server can be anywhere on the network.
//
// Where the CA bundle is looked for, first hit wins:
//
//   1. the app's `ca_bundle` setting -- an absolute path
//   2. <card>/.local/cacert.pem -- refreshable without reflashing anything,
//      which matters because certificates expire and firmware does not
//   3. /etc/ssl/cert.pem and /etc/ssl/certs/ -- where this OpenSSL looks by
//      default (its OPENSSLDIR is "/etc/ssl"), so a bundle dropped there by a
//      firmware repack is found with no configuration at all
//
// The bundle to use is Mozilla's, which is what a browser trusts. curl
// publishes it, extracted from Firefox's own store, at
// https://curl.se/ca/cacert.pem.
//
// Relaxed mode is for a server the user owns -- a Subsonic/Gonic box with a
// self-signed certificate, which no bundle can ever validate. See
// tls_verify_t below for exactly how far it goes.

// True when libssl and libcrypto were found and every symbol resolved. Cheap
// to call repeatedly; the loading happens once. In strict mode this also means
// a CA bundle was found: without one there is nothing to verify against, and
// refusing is the honest answer. In relaxed mode a bundle is optional, because
// the certificates that mode is for have no anchor to find.
bool tls_available(void);

// What OpenSSL says it is, for the log. Empty when unavailable.
const char *tls_version(void);

// Where the CA bundle was actually loaded from, or NULL when none was found.
// Worth logging once: "cannot open https" and "found no certificates" are very
// different problems with identical symptoms.
const char *tls_ca_source(void);

// The card's root, so <card>/.local/cacert.pem can be looked for. Call once at
// startup, before the first connection. NULL or unset simply drops that entry
// from the search.
void tls_set_card_root(const char *sd_root);

// How much of a server's certificate is demanded. One setting, two values,
// `tls_verify` in the app's settings.json:
//
//   strict   (default, and what an absent setting means) the chain must end at
//            a trust anchor from the CA bundle, the certificate must name the
//            host from the URL, and its validity dates must cover now.
//
//   relaxed  a self-signed certificate, or one whose issuer is not in the
//            bundle, is accepted instead of failing the handshake. The name and
//            the dates are still enforced, so an expired certificate and a
//            certificate for a different server are refused exactly as in
//            strict mode.
//
// Relaxed is what makes a home server with its own certificate reachable at
// all. It is a deliberate weakening, so it is off unless the user turns it on.
typedef enum {
	TLS_VERIFY_STRICT = 0,
	TLS_VERIFY_RELAXED = 1,
} tls_verify_t;

// The mode configured right now. Cheap: a read of the value the app set at
// startup, so callers read it per connection rather than caching it.
tls_verify_t tls_verify_mode(void);

// Applies the mode. The next connection uses it; connections already open keep
// the rules they started with. Call it after loading settings, before the
// first request.
void tls_set_verify_mode(tls_verify_t mode);

// An explicit CA bundle from the app's settings, or NULL/empty for the search
// above. Same timing rule as tls_set_verify_mode().
void tls_set_ca_bundle(const char *path);

typedef struct tls_conn tls_conn_t;

// Wraps an already-connected socket in TLS and performs the handshake,
// including certificate and hostname verification. `hostname` is both the SNI
// sent and the name checked against the certificate, so it must be the name
// from the URL and never an address the resolver produced.
//
// Returns NULL on failure, with a human-readable reason in `err` -- meant to
// be shown to the user, so it is in the same language as the rest of the
// player. The socket is left alone: the caller still owns and closes it.
tls_conn_t *tls_client(int fd, const char *hostname, char *err, size_t err_size);

// Same shapes as recv()/send(): >0 bytes moved, 0 for a clean end of stream,
// -1 on error (a socket timeout included).
int tls_read(tls_conn_t *c, void *buf, int len);
int tls_write(tls_conn_t *c, const void *buf, int len);

// Drops the TLS session. Does not close the socket.
void tls_free(tls_conn_t *c);

#endif /* TLS_H */
