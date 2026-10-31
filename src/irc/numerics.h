#pragma once

/* Server replies by their standard names, from the modern IRC client protocol at modern.ircdocs.horse */
#define RPL_AWAY 301
#define RPL_WHOISUSER 311
#define RPL_WHOISSERVER 312
#define RPL_WHOISIDLE 317
#define RPL_ENDOFWHOIS 318
#define RPL_WHOISCHANNELS 319
#define RPL_LISTSTART 321
#define RPL_LIST 322
#define RPL_LISTEND 323
#define RPL_CHANNELMODEIS 324
#define RPL_WHOISACCOUNT 330
#define RPL_TOPIC 332
#define RPL_ENDOFINVEXLIST 347
#define RPL_ENDOFEXCEPTLIST 349
#define RPL_ENDOFNAMES 366
#define RPL_ENDOFBANLIST 368
#define RPL_MOTD 372
#define RPL_MOTDSTART 375
#define RPL_ENDOFMOTD 376
#define ERR_NOSUCHNICK 401
#define ERR_NOSUCHSERVER 402
#define ERR_NOSUCHCHANNEL 403
#define ERR_CANNOTSENDTOCHAN 404
#define ERR_NEEDREGGEDNICK 477
#define RPL_WHOISSECURE 671
#define RPL_MONONLINE 730
#define RPL_MONOFFLINE 731
#define RPL_LOGGEDIN 900

/* Replies from 400 up to 600 report problems, and the rest report what happened */
#define FIRST_PROBLEM_REPLY 400
#define LAST_PROBLEM_REPLY 599
