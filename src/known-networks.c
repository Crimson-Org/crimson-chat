#include "../lang/en.h"
#include "known-networks.h"

/* Modern networks are those the ircv3.net network table shows with broad IRCv3 support, legacy ones are the largest
 * of the rest, and both groups go from most to fewest users on netsplit.de */
const KnownNetwork KNOWN_NETWORKS[] = {
	{"Crimson", "chat.skenliv.se", TEXT_CRIMSON_ABOUT, "icon:crimson-server", KNOWN_FEATURED, TLS_PORT, TRUE},
	{"Libera.Chat", "irc.libera.chat", TEXT_LIBERA_ABOUT, "icon:server-libera", KNOWN_MODERN, TLS_PORT, TRUE},
	{"HybridIRC", "irc.hybridirc.com", TEXT_HYBRIDIRC_ABOUT, "icon:server-hybridirc", KNOWN_MODERN, TLS_PORT, TRUE},
	{"hackint", "irc.hackint.org", TEXT_HACKINT_ABOUT, "icon:server-hackint", KNOWN_MODERN, TLS_PORT, TRUE},
	{"Snoonet", "irc.snoonet.org", TEXT_SNOONET_ABOUT, "icon:server-snoonet", KNOWN_MODERN, TLS_PORT, TRUE},
	{"tilde.chat", "irc.tilde.chat", TEXT_TILDE_ABOUT, "icon:server-tilde", KNOWN_MODERN, TLS_PORT, TRUE},
	{"Ergo", "irc.ergo.chat", TEXT_ERGO_ABOUT, "icon:server-ergo", KNOWN_MODERN, TLS_PORT, TRUE},
	{"OFTC", "irc.oftc.net", TEXT_OFTC_ABOUT, "icon:server-oftc", KNOWN_LEGACY, TLS_PORT, TRUE},
	{"Undernet", "irc.undernet.org", TEXT_UNDERNET_ABOUT, "icon:server-undernet", KNOWN_LEGACY, PLAIN_PORT, FALSE},
	{"Rizon", "irc.rizon.net", TEXT_RIZON_ABOUT, "icon:server-rizon", KNOWN_LEGACY, TLS_PORT, TRUE},
	{"EFnet", "irc.efnet.org", TEXT_EFNET_ABOUT, "icon:server-efnet", KNOWN_LEGACY, PLAIN_PORT, FALSE},
	{"DALnet", "irc.dal.net", TEXT_DALNET_ABOUT, "icon:server-dalnet", KNOWN_LEGACY, TLS_PORT, TRUE},
	{"QuakeNet", "irc.quakenet.org", TEXT_QUAKENET_ABOUT, "icon:server-quakenet", KNOWN_LEGACY, PLAIN_PORT, FALSE},
	{"IRCnet", "open.ircnet.net", TEXT_IRCNET_ABOUT, "icon:server-ircnet", KNOWN_LEGACY, TLS_PORT, TRUE},
};
const guint KNOWN_NETWORK_COUNT = G_N_ELEMENTS(KNOWN_NETWORKS);


const KnownNetwork *find_known_network(const char *host) {
	for (guint index = 0; host && index < KNOWN_NETWORK_COUNT; index++)
		if (g_ascii_strcasecmp(KNOWN_NETWORKS[index].host, host) == 0)
			return &KNOWN_NETWORKS[index];
	return NULL;
}
