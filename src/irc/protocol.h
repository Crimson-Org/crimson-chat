#pragma once

/* What the Crimson Chat app and its bouncer say to each other on top of IRC, kept once for both */

/* Every name of their own starts like this, so the bouncer can drop such names when a server sends them */
#define CRIMSON_NAMESPACE "crimson-chat/"
/* The command the app and the bouncer manage networks, invites and searches with */
#define CRIMSON_COMMAND "CRIMSON"
/* The bouncer's name, also the one it announces when its admin gave it none */
#define BOUNCER_NAME "crimson-chat-bouncer"

/* The capability that tells the app it reached its own bouncer, and the batches the bouncer answers in */
#define BOUNCER_CAP CRIMSON_NAMESPACE "bouncer"
#define NETWORK_LISTING_BATCH CRIMSON_NAMESPACE "networks"
#define SEARCH_BATCH CRIMSON_NAMESPACE "search"
/* The tag that names the network a line belongs to */
#define NETWORK_TAG CRIMSON_NAMESPACE "network"
/* The network attributes that say whether the bouncer reconnects by itself, and what kind of problem stopped it */
#define RETRY_ATTRIBUTE CRIMSON_NAMESPACE "retrying"
#define FAILURE_ATTRIBUTE CRIMSON_NAMESPACE "failure"
/* The tag a kept message of the day carries its lines in, joined by line breaks */
#define WELCOME_MESSAGE_TAG CRIMSON_NAMESPACE "motd"
/* Where the app uploads pictures and files, as a whole address or the website's port, and the largest file */
#define AVATARS_TOKEN CRIMSON_NAMESPACE "AVATARS"
#define FILES_TOKEN CRIMSON_NAMESPACE "FILES"
#define FILE_LIMIT_TOKEN CRIMSON_NAMESPACE "FILE-LIMIT"
/* The conversation the server's own chat is kept and asked for under, which no nick or channel can be */
#define SERVER_TARGET "*"

/* The welcome reply that lists what a server supports */
#define ISUPPORT_REPLY "005"
/* IRC over TLS answers here unless a server says otherwise */
#define IRC_TLS_PORT 6697
/* SASL sends its answer in pieces this long */
#define AUTHENTICATE_CHUNK 400
