<p align="center">
  <img src="./data/icons/scalable/apps/io.github.CrimsonOrg.CrimsonChat.svg" alt="Crimson Chat logo" width="128" height="128">
</p>

<h1 align="center">Crimson Chat</h1>
<p align="center">An <a href="https://ircv3.net">IRCv3</a> client for Linux with replies, reactions, typing notices and synced history.<br>
Comes with its own bouncer that keeps you online while you are away.<br>
Old protocol, new manners.</p>

<p align="center">
  <a href="https://gtk.org">
    <img src="https://img.shields.io/badge/Toolkit-GTK4-4a86cf?logo=gtk&logoColor=white" alt="GTK4">
  </a>
  <a href="https://gnome.pages.gitlab.gnome.org/libadwaita/">
    <img src="https://img.shields.io/badge/Style-libadwaita-3584e4?logo=gnome&logoColor=white" alt="libadwaita">
  </a>
  <a href="https://ircv3.net">
    <img src="https://img.shields.io/badge/Protocol-IRCv3-c8102e?logoColor=white" alt="IRCv3">
  </a>
</p>

## Why

IRC networks now offer history, replies, reactions and read markers, but most clients still show a scrolling wall of text.\
Crimson Chat looks and feels like a modern messenger, and its bouncer keeps your networks connected when the computer is off.

## Features

### A messenger layout

The first start welcomes you with a choice between a bouncer and a server, followed by a short guide for either.\
The guide suggests Crimson's own server, chat.skenliv.se, first, followed by well known networks with good IRCv3 support.\
Once a server connects, it lists its rooms busiest first, so you can tick several and join them at once.\
Each server's rooms and private chats sit in one list, newest first, with rooms as rounded squares and people as circles.\
A rail of servers appears down the left only when you use more than one.\
Right-click a room to leave it, or a person to message, mention or look them up, with moderation in rooms you run.\
Your account on each server sets your nickname, display name, picture and login, wherever that server supports them.\
Without an account, a banner offers to register your nickname, with the IRCv3 commands or NickServ, and to log in when the nickname turns out to be registered already.\
Messages from the same person group into bubbles under one avatar, with your own on the right.

### Modern IRC

Replies quote the message they answer, and a right click reacts with an emoji, copies the text or deletes your message.\
Typing notices show who is writing, and the read position follows you between your devices through the bouncer.\
Long and multi-line messages arrive in one piece on servers with multiline batches, and history loads as you scroll up.\
Avatars, display names, pronouns, status lines, homepages and network icons come from the IRCv3 metadata the server shares, and you set your own in Account.\
Clicking the picture of a person, room or server lets you choose your own, which stays on this computer.\
Links to pictures over HTTPS show the picture, and bold, italics, colours and monospace keep their formatting.\
Typing **bold** or *italics*, with stars or underscores, sends them formatted.

### Your own bouncer

`crimson-chat-bouncer` is a small command line program for a server or an always on computer.\
The first launch prompts for an admin username and password, creates a TLS certificate and prints its fingerprint.\
Add the bouncer in Crimson Chat with that login, trust the fingerprint once, and add networks through it from then on.\
The bouncer joins your channels, keeps their history in [SQLite](https://sqlite.org) and replays it to each client you connect.\
Admins invite people with links made in Crimson Chat or on the bouncer's website, where the invited pick their own login.\
It speaks the bouncer network extension of [soju](https://soju.im), which other modern IRC clients understand as well.

### Notifications and the tray

Mentions and private messages raise a desktop notification that opens the conversation.\
Closing the window keeps the chats connected in the tray, and Ctrl+Q quits.

### Keyboard

Ctrl+J joins a room, Ctrl+N messages someone and Ctrl+K searches the chats.\
Alt+Up and Alt+Down step through the rooms, and Tab completes nicks in the message field.\
Commands such as /join, /msg, /me, /topic, /away and /whois work in any room, and other commands go to the server unchanged.

## Building

You need a [C compiler](https://gcc.gnu.org/), [pkg-config](https://www.freedesktop.org/wiki/Software/pkg-config/) and the development files for [GTK4](https://gtk.org) 4.14, [libadwaita](https://gnome.pages.gitlab.gnome.org/libadwaita/) 1.6, [libsoup](https://libsoup.gnome.org) 3, SQLite and [GnuTLS](https://gnutls.org) 3.6.13.\
On [openSUSE](https://www.opensuse.org/) these come from `sudo zypper install gcc make pkg-config gtk4-devel libadwaita-devel libsoup-devel sqlite3-devel libgnutls-devel`.

Clone it with `git clone --recurse-submodules` so the shared Crimson Core code comes along.\
`sh .crimson-build/install.sh` installs both programs and `sh .crimson-build/uninstall.sh` removes them.\
`sh .crimson-build/run.sh` builds and starts Crimson Chat, and `sh .crimson-build/archive.sh` writes a tar.gz to `dist/`.\
`sh .crimson-build/build-bouncer.sh` builds only the bouncer, which needs GLib, SQLite and GnuTLS but no graphical libraries.\
`crimson-chat-bouncer --listen 6697 --web 8443` sets the IRC and website ports, `--password` changes a password, and `--admin NAME` lets that user invite people too.\
People can upload pictures for themselves and for rooms they run, which the bouncer scales, keeps and sets as their avatar on servers with IRCv3 metadata.\
The settings file takes `avatars`, `avatar-format`, `avatar-quality`, `avatar-size`, `avatar-largest-mb` and `public-url`, the address pictures are served from, which needs a certificate other people's apps trust.
