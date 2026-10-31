<p align="center">
  <img src="./assets/icons/scalable/apps/io.github.CrimsonOrg.CrimsonChat.svg" alt="Crimson Chat logo" width="128" height="128">
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

<p align="center">
  <a href="https://github.com/Crimson-Org/crimson-chat/releases/latest"><b>Download Here!</b></a>
</p>

## Why

IRC networks now offer history, replies, reactions and read markers, but most clients still show a scrolling wall of text.\
Crimson Chat looks and feels like a modern messenger, and its bouncer keeps your networks connected when the computer is off.

## About

### A messenger layout

The first start helps you add a bouncer or a server, and suggests networks with good IRCv3 support.\
Rooms and private chats sit in one list, newest first, and messages from the same person group into bubbles.\
A rail of servers shows down the left once you use more than one.

### Rooms in plain words

Changes to a room read as sentences, such as "Alice made Bob a moderator and muted the room".\
A room's page shows its settings, and moderators change them and keep its ban and invite lists there.

### Modern IRC

Replies, reactions and typing notices, and read markers that follow you between devices.\
Avatars, display names and status lines come from the server, and you set your own in Account.\
Pictures and web links show a preview, and pasting or dropping a file shares it.\
History loads as you scroll up, and search finds what was said in a room.

### Your own bouncer

`crimson-chat-bouncer` is a small program for a server or a computer that is always on.\
It keeps your networks connected, saves their history and sends it to each device you connect.\
Start it once to set the admin login, then add it in Crimson Chat with that login.\
Admins invite people with links and manage accounts on the bouncer's website.

### Notifications and the tray

Mentions and private messages raise a desktop notification that opens the conversation or marks it read.\
Closing the window keeps the chats connected in the tray, and Ctrl+Q quits.

### Keyboard

Ctrl+K searches the chats, Ctrl+J joins a room and Ctrl+N messages someone.\
Commands such as /join, /msg and /me work like in any IRC client.

## Building

You need a [C compiler](https://gcc.gnu.org/), [make](https://www.gnu.org/software/make/), [pkg-config](https://www.freedesktop.org/wiki/Software/pkg-config/) and the development files for [GTK4](https://gtk.org) 4.14, [libadwaita](https://gnome.pages.gitlab.gnome.org/libadwaita/) 1.6, [libsoup](https://libsoup.gnome.org) 3, [SQLite](https://sqlite.org) and [GnuTLS](https://gnutls.org) 3.6.13.\
On [openSUSE](https://www.opensuse.org/) these come from `sudo zypper install gcc make pkg-config gtk4-devel libadwaita-devel libsoup-devel sqlite3-devel libgnutls-devel`.

Steps:
 1. Clone using `git clone --recurse-submodules https://github.com/Crimson-Org/crimson-chat`
 2. Run `sh .crimson-build/install.sh`

`sh .crimson-build/build-bouncer.sh` builds only the bouncer, which needs no graphical libraries.\
`sh .crimson-build/uninstall.sh` removes it again.
