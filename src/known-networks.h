#pragma once

#include <glib.h>

/* The usual IRC ports with and without encryption */
#define TLS_PORT 6697
#define PLAIN_PORT 6667

/* Where the server guide lists a network: on its first page, or behind the modern or legacy servers row */
typedef enum {
	KNOWN_FEATURED,
	KNOWN_MODERN,
	KNOWN_LEGACY,
} KnownNetworkGroup;

/* A well known network the server guide offers, with the logo built into the app */
typedef struct {
	const char *name, *host, *about, *picture;
	KnownNetworkGroup group;
	int port;
	gboolean uses_tls;
} KnownNetwork;

extern const KnownNetwork KNOWN_NETWORKS[];
extern const guint KNOWN_NETWORK_COUNT;

const KnownNetwork *find_known_network(const char *host);
